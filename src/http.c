/*
 * http.c — HTTPS client over BearSSL
 */

#include "../include/tc.h"
#include <bearssl.h>

/* From ca.c */
extern br_x509_trust_anchor *ca_get_anchors(size_t *count);

/*
 * Minimal TLS client init — only modern ciphers, TLS 1.2 only.
 * Drops 3DES, AES-CBC, AES-CCM, MD5, SHA-1, SHA-224, TLS 1.0/1.1.
 * Saves ~10-15KB vs br_ssl_client_init_full().
 */
void ssl_client_init_minimal(void *sc_ptr, void *xc_ptr,
                             const void *anchors_ptr, size_t anchor_count) {
    br_ssl_client_context *cc = sc_ptr;
    br_x509_minimal_context *xc = xc_ptr;
    const br_x509_trust_anchor *ta = anchors_ptr;

    static const uint16_t suites[] = {
        BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    };

    br_ssl_client_zero(cc);
    br_ssl_engine_set_versions(&cc->eng, BR_TLS12, BR_TLS12);

    br_x509_minimal_init(xc, &br_sha256_vtable, ta, anchor_count);

    br_ssl_engine_set_suites(&cc->eng, suites,
        (sizeof suites) / (sizeof suites[0]));
    br_ssl_client_set_default_rsapub(cc);
    br_ssl_engine_set_default_rsavrfy(&cc->eng);
    br_ssl_engine_set_default_ecdsa(&cc->eng);
    br_x509_minimal_set_rsa(xc, br_ssl_engine_get_rsavrfy(&cc->eng));
    br_x509_minimal_set_ecdsa(xc,
        br_ssl_engine_get_ec(&cc->eng),
        br_ssl_engine_get_ecdsa(&cc->eng));

    /* Only SHA-256 and SHA-384 (needed for GCM suites) */
    br_ssl_engine_set_hash(&cc->eng, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_hash(&cc->eng, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(xc, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(xc, br_sha384_ID, &br_sha384_vtable);

    br_ssl_engine_set_x509(&cc->eng, &xc->vtable);

    /* TLS 1.2 PRF only */
    br_ssl_engine_set_prf_sha256(&cc->eng, &br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(&cc->eng, &br_tls12_sha384_prf);

    /* Only AES-GCM + ChaCha20-Poly1305 */
    br_ssl_engine_set_default_aes_gcm(&cc->eng);
    br_ssl_engine_set_default_chapol(&cc->eng);
}

/* Per-thread extra headers (up to 4) */
#define MAX_EXTRA_HDRS 4
static __thread struct { char name[128]; char value[512]; } extra_hdrs[MAX_EXTRA_HDRS];
static __thread int n_extra_hdrs = 0;

void http_set_header(const char *name, const char *value) {
    if (!name || !name[0]) { n_extra_hdrs = 0; return; }
    if (n_extra_hdrs >= MAX_EXTRA_HDRS) return;
    snprintf(extra_hdrs[n_extra_hdrs].name, 128, "%s", name);
    snprintf(extra_hdrs[n_extra_hdrs].value, 512, "%s", value ? value : "");
    n_extra_hdrs++;
}

/* Plugin-facing wrappers */
void tc_http_header(const char *name, const char *value) {
    http_set_header(name, value);
}

static int tcp_connect(const char *host, int port) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* BearSSL I/O callbacks */
static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, TC_HTTP_TIMEOUT * 1000);
    if (r <= 0) return -1;
    ssize_t n = read(fd, buf, len);
    return (int)n;
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n = write(fd, buf, len);
    return (int)n;
}

static int write_all_fd(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Parse URL → host, port, path, is_https */
static int parse_url(const char *url, char *host, int *port, char *path, int *is_https) {
    *is_https = 0;
    *port = 80;

    if (strncmp(url, "https://", 8) == 0) {
        url += 8;
        *is_https = 1;
        *port = 443;
    } else if (strncmp(url, "http://", 7) == 0) {
        url += 7;
    }

    const char *slash = strchr(url, '/');
    const char *colon = strchr(url, ':');

    if (colon && (!slash || colon < slash)) {
        int hlen = (int)(colon - url);
        memcpy(host, url, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
        if (slash)
            snprintf(path, 2048, "%s", slash);
        else
            strcpy(path, "/");
    } else if (slash) {
        int hlen = (int)(slash - url);
        memcpy(host, url, hlen);
        host[hlen] = '\0';
        snprintf(path, 2048, "%s", slash);
    } else {
        snprintf(host, 256, "%s", url);
        strcpy(path, "/");
    }

    return 0;
}

static http_response_t do_request(const char *method, const char *url,
                                   const char *content_type,
                                   const char *body, size_t body_len) {
    http_response_t resp = {0};
    char host[256], path[2048];
    int port, is_https;

    parse_url(url, host, &port, path, &is_https);

    int fd = tcp_connect(host, port);
    if (fd < 0) {
        resp.status = -1;
        return resp;
    }

    /* TLS setup if HTTPS */
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    br_sslio_context ioc;
    int use_tls = is_https;

    if (use_tls) {
        size_t anchor_count;
        br_x509_trust_anchor *anchors = ca_get_anchors(&anchor_count);

        if (!anchors || anchor_count == 0) {
            close(fd);
            resp.status = -2;
            return resp;
        }

        ssl_client_init_minimal(&sc, &xc, anchors, anchor_count);
        br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);
        br_ssl_client_reset(&sc, host, 0);
        br_sslio_init(&ioc, &sc.eng, sock_read, &fd, sock_write, &fd);
    }

    /* Build HTTP request */
    char request[TC_BUF_LG];
    int rlen = snprintf(request, sizeof(request),
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n",
        method, path, host);

    if (content_type && body_len > 0) {
        rlen += snprintf(request + rlen, sizeof(request) - rlen,
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n",
            content_type, body_len);
    }

    for (int h = 0; h < n_extra_hdrs; h++) {
        rlen += snprintf(request + rlen, sizeof(request) - rlen,
            "%s: %s\r\n", extra_hdrs[h].name, extra_hdrs[h].value);
    }
    n_extra_hdrs = 0;

    rlen += snprintf(request + rlen, sizeof(request) - rlen, "\r\n");

    /* Send request */
    if (use_tls) {
        br_sslio_write_all(&ioc, request, rlen);
        if (body && body_len > 0)
            br_sslio_write_all(&ioc, body, body_len);
        br_sslio_flush(&ioc);
    } else {
        if (write_all_fd(fd, request, (size_t)rlen) != 0 ||
            (body && body_len > 0 && write_all_fd(fd, body, body_len) != 0)) {
            close(fd);
            resp.status = -4;
            return resp;
        }
    }

    /* Read response (dynamically grown buffer) */
    size_t buf_cap = 65536;
    size_t buf_max = 4 * 1024 * 1024; /* 4MB max */
    char *buf = malloc(buf_cap);
    size_t total = 0;

    while (total < buf_max) {
        if (total + 8192 > buf_cap) {
            buf_cap *= 2;
            if (buf_cap > buf_max) buf_cap = buf_max;
            buf = realloc(buf, buf_cap);
        }
        int n;
        if (use_tls)
            n = br_sslio_read(&ioc, buf + total, buf_cap - total - 1);
        else {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            int pr = poll(&pfd, 1, TC_HTTP_TIMEOUT * 1000);
            if (pr <= 0) break;
            n = read(fd, buf + total, buf_cap - total - 1);
        }
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';

    if (use_tls) {
        if (total == 0) {
            int eng_err = br_ssl_engine_last_error(&sc.eng);
            if (eng_err != 0)
                log_error("HTTP TLS error: %d (0 bytes read from %s)", eng_err, host);
        }
        br_ssl_engine_close(&sc.eng);
    }
    close(fd);

    /* Parse response */
    char *header_end = strstr(buf, "\r\n\r\n");
    if (!header_end) {
        if (total > 0)
            log_error("HTTP: no header terminator in %zu bytes from %s", total, host);
        resp.status = -3;
        free(buf);
        return resp;
    }

    /* Parse status */
    sscanf(buf, "HTTP/%*s %d", &resp.status);

    /* Parse content-type */
    char *ct = strcasestr(buf, "Content-Type:");
    if (ct && ct < header_end) {
        ct += 13;
        while (*ct == ' ') ct++;
        char *ct_end = strpbrk(ct, "\r\n;");
        int ct_len = ct_end ? (int)(ct_end - ct) : 127;
        if (ct_len > 127) ct_len = 127;
        memcpy(resp.content_type, ct, ct_len);
        resp.content_type[ct_len] = '\0';
    }

    /* Body — handle chunked transfer encoding */
    char *body_start = header_end + 4;
    size_t raw_len = total - (body_start - buf);

    int is_chunked = 0;
    char *te = strcasestr(buf, "Transfer-Encoding:");
    if (te && te < header_end && strcasestr(te, "chunked"))
        is_chunked = 1;

    if (is_chunked) {
        /* Decode chunked body */
        resp.body = malloc(raw_len + 1);
        resp.body_len = 0;
        char *p = body_start;
        char *end = body_start + raw_len;
        while (p < end) {
            /* Read chunk size (hex) */
            unsigned long chunk_sz = strtoul(p, NULL, 16);
            char *data_start = strstr(p, "\r\n");
            if (!data_start) break;
            data_start += 2;
            if (chunk_sz == 0) break;
            if (data_start + chunk_sz > end)
                chunk_sz = end - data_start;
            memcpy(resp.body + resp.body_len, data_start, chunk_sz);
            resp.body_len += chunk_sz;
            p = data_start + chunk_sz;
            if (p + 2 <= end && p[0] == '\r' && p[1] == '\n')
                p += 2;
        }
        resp.body[resp.body_len] = '\0';
    } else {
        resp.body_len = raw_len;
        resp.body = malloc(resp.body_len + 1);
        memcpy(resp.body, body_start, resp.body_len);
        resp.body[resp.body_len] = '\0';
    }

    free(buf);
    return resp;
}

http_response_t http_get(const char *url) {
    return do_request("GET", url, NULL, NULL, 0);
}

http_response_t http_post(const char *url, const char *content_type,
                          const char *body, size_t body_len) {
    return do_request("POST", url, content_type, body, body_len);
}

http_response_t http_post_json(const char *url, const char *json) {
    return do_request("POST", url, "application/json",
                      json, json ? strlen(json) : 0);
}

void http_response_free(http_response_t *r) {
    if (r->body) {
        free(r->body);
        r->body = NULL;
    }
}

/* Plugin-facing wrappers */
int tc_http_get(const char *url, char *buf, size_t buf_sz) {
    http_response_t r = http_get(url);
    if (r.body) {
        snprintf(buf, buf_sz, "%s", r.body);
        int status = r.status;
        http_response_free(&r);
        return status;
    }
    snprintf(buf, buf_sz, "HTTP error %d", r.status);
    return r.status;
}

int tc_http_post(const char *url, const char *content_type,
                 const char *body, size_t body_len,
                 char *resp_buf, size_t resp_sz) {
    http_response_t r = http_post(url, content_type, body, body_len);
    if (r.body) {
        snprintf(resp_buf, resp_sz, "%s", r.body);
        int status = r.status;
        http_response_free(&r);
        return status;
    }
    snprintf(resp_buf, resp_sz, "HTTP error %d", r.status);
    return r.status;
}

int tc_http_post_json(const char *url, const char *json,
                      char *resp_buf, size_t resp_sz) {
    return tc_http_post(url, "application/json", json,
                        json ? strlen(json) : 0, resp_buf, resp_sz);
}
