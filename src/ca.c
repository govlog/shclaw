/*
 * ca.c — TLS CA trust anchor loading from PEM files
 *
 * Cascade:
 *   1. System CA bundle paths
 *   2. data/cacert.pem
 */

#include "../include/tc.h"
#include <bearssl.h>

static br_x509_trust_anchor *g_anchors = NULL;
static size_t g_anchor_count = 0;
static size_t g_anchor_cap = 0;

static const char *CA_PATHS[] = {
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/cert.pem",
    "/etc/openssl/certs/ca-certificates.crt", /* NetBSD */
    NULL,
};

/* DN accumulation callback for br_x509_decoder */
typedef struct {
    unsigned char *buf;
    size_t len;
    size_t cap;
} dn_buf_t;

static void dn_append(void *ctx, const void *data, size_t len) {
    dn_buf_t *dn = ctx;
    if (dn->len + len > dn->cap) {
        size_t new_cap = (dn->len + len) * 2;
        if (new_cap < 256) new_cap = 256;
        unsigned char *grown = realloc(dn->buf, new_cap);
        if (!grown) return;
        dn->buf = grown;
        dn->cap = new_cap;
    }
    memcpy(dn->buf + dn->len, data, len);
    dn->len += len;
}

static void add_trust_anchor(const unsigned char *der, size_t der_len) {
    /* Decode the certificate to extract DN and public key */
    dn_buf_t dn = {0};
    br_x509_decoder_context dc;
    br_x509_decoder_init(&dc, dn_append, &dn);
    br_x509_decoder_push(&dc, der, der_len);

    int err = br_x509_decoder_last_error(&dc);
    if (err != 0) {
        free(dn.buf);
        return;
    }

    /* Only keep CA certificates */
    if (!br_x509_decoder_isCA(&dc)) {
        free(dn.buf);
        return;
    }

    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk) {
        free(dn.buf);
        return;
    }

    /* Grow anchor array */
    if (g_anchor_count >= g_anchor_cap) {
        size_t new_cap = g_anchor_cap ? g_anchor_cap * 2 : 64;
        br_x509_trust_anchor *grown = realloc(g_anchors,
                            new_cap * sizeof(br_x509_trust_anchor));
        if (!grown) { free(dn.buf); return; }
        g_anchors = grown;
        g_anchor_cap = new_cap;
    }

    br_x509_trust_anchor *ta = &g_anchors[g_anchor_count];
    memset(ta, 0, sizeof(*ta));

    /* Store the DN (properly extracted by the decoder callback) */
    ta->dn.data = dn.buf;
    ta->dn.len = dn.len;
    ta->flags = BR_X509_TA_CA;

    /* Copy public key */
    if (pk->key_type == BR_KEYTYPE_RSA) {
        unsigned char *n = malloc(pk->key.rsa.nlen);
        unsigned char *e = malloc(pk->key.rsa.elen);
        if (!n || !e) { free(n); free(e); free(dn.buf); return; }
        memcpy(n, pk->key.rsa.n, pk->key.rsa.nlen);
        memcpy(e, pk->key.rsa.e, pk->key.rsa.elen);
        ta->pkey.key_type = BR_KEYTYPE_RSA;
        ta->pkey.key.rsa.n = n;
        ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->pkey.key.rsa.e = e;
        ta->pkey.key.rsa.elen = pk->key.rsa.elen;
        g_anchor_count++;
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        unsigned char *q = malloc(pk->key.ec.qlen);
        if (!q) { free(dn.buf); return; }
        memcpy(q, pk->key.ec.q, pk->key.ec.qlen);
        ta->pkey.key_type = BR_KEYTYPE_EC;
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.q = q;
        ta->pkey.key.ec.qlen = pk->key.ec.qlen;
        g_anchor_count++;
    } else {
        free(dn.buf);
    }
}

/* Base64 decode table */
static const unsigned char B64[256] = {
    ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
    ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
    ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
    ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
    ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
    ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
    ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
    ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63,
};

static int load_pem_file(const char *path) {
    size_t pem_len;
    char *pem_data = file_slurp(path, &pem_len);
    if (!pem_data) return -1;

    unsigned char *der = NULL;
    size_t der_len = 0, der_cap = 0;
    int in_cert = 0;
    size_t before = g_anchor_count;

    const char *line = pem_data;
    while (*line) {
        const char *eol = strchr(line, '\n');
        if (!eol) eol = line + strlen(line);
        int ll = (int)(eol - line);
        if (ll > 0 && line[ll - 1] == '\r') ll--;

        if (ll >= 27 && strncmp(line, "-----BEGIN CERTIFICATE-----", 27) == 0) {
            in_cert = 1;
            der_len = 0;
            if (!der) {
                der_cap = 4096;
                der = malloc(der_cap);
                if (!der) { free(pem_data); return -1; }
            }
        } else if (in_cert && ll >= 25 &&
                   strncmp(line, "-----END CERTIFICATE-----", 25) == 0) {
            if (der_len > 0)
                add_trust_anchor(der, der_len);
            in_cert = 0;
            der_len = 0;
        } else if (in_cert && ll > 0) {
            /* Base64 decode */
            for (int i = 0; i + 3 < ll; i += 4) {
                unsigned a  = B64[(unsigned char)line[i]];
                unsigned b  = B64[(unsigned char)line[i+1]];
                unsigned c  = B64[(unsigned char)line[i+2]];
                unsigned d2 = B64[(unsigned char)line[i+3]];

                if (der_len + 3 > der_cap) {
                    size_t new_cap = der_cap * 2;
                    unsigned char *grown = realloc(der, new_cap);
                    if (!grown) { free(der); free(pem_data); return -1; }
                    der = grown;
                    der_cap = new_cap;
                }
                der[der_len++] = (a << 2) | (b >> 4);
                if (line[i+2] != '=')
                    der[der_len++] = (b << 4) | (c >> 2);
                if (line[i+3] != '=')
                    der[der_len++] = (c << 6) | d2;
            }
        }

        line = *eol ? eol + 1 : eol;
    }

    free(der);
    free(pem_data);
    return (g_anchor_count > before) ? 0 : -1;
}

int ca_init(void) {
    for (int i = 0; CA_PATHS[i]; i++) {
        if (file_exists(CA_PATHS[i])) {
            if (load_pem_file(CA_PATHS[i]) == 0) {
                log_info("CA: loaded %zu trust anchors from %s",
                         g_anchor_count, CA_PATHS[i]);
                return 0;
            }
        }
    }

    if (file_exists("data/cacert.pem")) {
        if (load_pem_file("data/cacert.pem") == 0) {
            log_info("CA: loaded %zu trust anchors from data/cacert.pem",
                     g_anchor_count);
            return 0;
        }
    }

    log_error("CA: no trust anchors found! HTTPS will fail.");
    log_error("CA: place a PEM bundle at data/cacert.pem or install ca-certificates");
    return -1;
}

void ca_cleanup(void) {
    for (size_t i = 0; i < g_anchor_count; i++) {
        free(g_anchors[i].dn.data);
        if (g_anchors[i].pkey.key_type == BR_KEYTYPE_RSA) {
            free(g_anchors[i].pkey.key.rsa.n);
            free(g_anchors[i].pkey.key.rsa.e);
        } else if (g_anchors[i].pkey.key_type == BR_KEYTYPE_EC) {
            free(g_anchors[i].pkey.key.ec.q);
        }
    }
    free(g_anchors);
    g_anchors = NULL;
    g_anchor_count = 0;
    g_anchor_cap = 0;
}

br_x509_trust_anchor *ca_get_anchors(size_t *count) {
    *count = g_anchor_count;
    return g_anchors;
}
