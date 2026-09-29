/*
 * http.c — HTTPS client over BearSSL
 */

#include "../include/tc.h"
#include <bearssl.h>

/* From ca.c */
extern br_x509_trust_anchor *ca_get_anchors(size_t *count);

/*
 * Minimal TLS client — only modern ciphers, TLS 1.2 only.
 * Drops 3DES, AES-CBC, AES-CCM, MD5, SHA-1, SHA-224, TLS 1.0/1.1.
 * Saves ~10-15KB vs br_ssl_client_init_full().
 */
int tls_client_start(void *sc_ptr, void *xc_ptr, void *ioc,
                     unsigned char *iobuf, size_t iobuf_sz,
                     const char *host, int *fd,
                     int (*rd)(void *, unsigned char *, size_t),
                     int (*wr)(void *, const unsigned char *, size_t)) {
    br_ssl_client_context *cc = sc_ptr;
    br_x509_minimal_context *xc = xc_ptr;
    size_t anchor_count;
    const br_x509_trust_anchor *ta = ca_get_anchors(&anchor_count);
    if (!ta || anchor_count == 0)
        return -1;

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

    br_ssl_engine_set_buffer(&cc->eng, iobuf, iobuf_sz, 1);
    br_ssl_client_reset(cc, host, 0);
    br_sslio_init(ioc, &cc->eng, rd, fd, wr, fd);
    return 0;
}

/* Per-thread request settings, reset after each request */
#define MAX_EXTRA_HDRS 4
static __thread struct { char name[128]; char value[512]; } extra_hdrs[MAX_EXTRA_HDRS];
static __thread int n_extra_hdrs = 0;
static __thread int req_timeout = 0;     /* seconds, 0 = TC_HTTP_TIMEOUT */
static __thread int req_timed_out = 0;

/* CR/LF or other control bytes would let a URL or header value inject
 * extra header lines into the request. */
static int has_ctl(const char *s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f) return 1;
    return 0;
}

void http_set_header(const char *name, const char *value) {
    if (!name || !name[0]) { n_extra_hdrs = 0; return; }
    if (n_extra_hdrs >= MAX_EXTRA_HDRS) return;
    snprintf(extra_hdrs[n_extra_hdrs].name, 128, "%s", name);
    snprintf(extra_hdrs[n_extra_hdrs].value, 512, "%s", value ? value : "");
    n_extra_hdrs++;
}

void http_set_timeout(int seconds) {
    req_timeout = seconds > 0 ? seconds : 0;
}

/* Plugin-facing wrappers */
void tc_http_header(const char *name, const char *value) {
    http_set_header(name, value);
}

const char *http_strerror(int status) {
    switch (status) {
    case HTTP_ERR_CONNECT: return "cannot connect";
    case HTTP_ERR_TLS:     return "TLS handshake failed";
    case HTTP_ERR_PROTO:   return "no valid HTTP response";
    case HTTP_ERR_REQUEST: return "invalid URL or header";
    case HTTP_ERR_NOMEM:   return "out of memory";
    case HTTP_ERR_TIMEOUT: return "timed out";
    }
    return "HTTP error";
}

int net_connect(const char *host, int port, int timeout_s) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        fd = socket(r->ai_family, r->ai_socktype | SOCK_CLOEXEC, r->ai_protocol);
        if (fd < 0) continue;
        /* Non-blocking connect: an unreachable host fails after timeout_s,
         * not after the kernel's multi-minute SYN retries. */
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int ok = connect(fd, r->ai_addr, r->ai_addrlen) == 0;
        if (!ok && errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            int err = 0;
            socklen_t elen = sizeof(err);
            ok = poll(&pfd, 1, timeout_s * 1000) == 1 &&
                 getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 &&
                 err == 0;
        }
        if (ok) {
            fcntl(fd, F_SETFL, fl);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Wait for data; the timeout is per read, so a slow but alive peer is fine */
static int wait_readable(int fd) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int timeout = req_timeout > 0 ? req_timeout : TC_HTTP_TIMEOUT;
    int r;
    do r = poll(&pfd, 1, timeout * 1000); while (r < 0 && errno == EINTR);
    if (r == 0) req_timed_out = 1;
    return r > 0 ? 0 : -1;
}

/* BearSSL I/O callbacks */
static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    if (wait_readable(fd) != 0) return -1;
    ssize_t n = read(fd, buf, len);
    return n > 0 ? (int)n : -1;
}

int net_write(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n = write(fd, buf, len);
    return n > 0 ? (int)n : -1;
}

/* Parse URL → host, port, path (fragment dropped), is_https */
static int parse_url(const char *url, char *host, int *port, char *path, int *is_https) {
    if (!url || has_ctl(url))
        return -1;

    *is_https = strncmp(url, "https://", 8) == 0;
    if (*is_https) url += 8;
    else if (strncmp(url, "http://", 7) == 0) url += 7;
    *port = *is_https ? 443 : 80;

    /* Authority: host[:port] or [IPv6][:port], up to the first / ? # */
    size_t alen = strcspn(url, "/?#");
    const char *h = url, *colon;
    size_t hlen;
    if (url[0] == '[') {
        const char *rb = memchr(url, ']', alen);
        if (!rb) return -1;
        h = url + 1;
        hlen = (size_t)(rb - h);
        colon = rb + 1 < url + alen ? rb + 1 : NULL;
        if (colon && *colon != ':') return -1;
    } else {
        colon = memchr(url, ':', alen);
        hlen = colon ? (size_t)(colon - url) : alen;
    }
    if (hlen == 0 || hlen >= 256) return -1;
    memcpy(host, h, hlen);
    host[hlen] = '\0';
    if (colon) *port = atoi(colon + 1);

    const char *rest = url + alen;
    int plen = (int)strcspn(rest, "#");
    if (snprintf(path, 2048, "%s%.*s", *rest == '/' ? "" : "/", plen, rest) >= 2048)
        return -1;
    return *port > 0 && *port < 65536 ? 0 : -1;
}

/* Plugins build URLs from raw input ("New York", "Zürich"): percent-encode
 * what cannot appear in a request line. -1 if it does not fit. */
static int encode_path(const char *in, char *out, size_t sz) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (o + 4 > sz) return -1;
        if (*p <= 0x20 || *p >= 0x7f || strchr("\"<>\\^`{|}", *p)) {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o] = '\0';
    return 0;
}

/* Value of header `name` in the header block ending at `end`, or NULL */
static const char *find_header(const char *buf, const char *end, const char *name) {
    size_t nlen = strlen(name);
    for (const char *p = strstr(buf, "\r\n"); p && p < end; p = strstr(p + 2, "\r\n")) {
        const char *line = p + 2;
        if (strncasecmp(line, name, nlen) == 0 && line[nlen] == ':') {
            line += nlen + 1;
            while (*line == ' ' || *line == '\t') line++;
            return line;
        }
    }
    return NULL;
}

size_t http_dechunk(char *body, size_t len) {
    char *p = body, *end = body + len, *out = body;
    while (p < end) {
        char *eol = memmem(p, (size_t)(end - p), "\r\n", 2);
        if (!eol) break;
        unsigned long chunk = strtoul(p, NULL, 16);
        p = eol + 2;
        if (chunk == 0) break;
        /* Compare sizes, not pointers: a huge chunk size must not wrap */
        if (chunk > (unsigned long)(end - p))
            chunk = (unsigned long)(end - p);
        memmove(out, p, chunk);
        out += chunk;
        p += chunk;
        if (end - p >= 2 && p[0] == '\r' && p[1] == '\n')
            p += 2;
    }
    *out = '\0';
    return (size_t)(out - body);
}

static http_response_t do_request(const char *method, const char *url,
                                   const char *content_type,
                                   const char *body, size_t body_len) {
    http_response_t resp = { .status = HTTP_ERR_REQUEST };
    char host[256], path[2048], epath[3 * 1024];
    int port, is_https;
    int timeout = req_timeout;
    req_timeout = 0;
    req_timed_out = 0;

    /* Build the request first: headers are per-request and consumed here */
    char request[TC_BUF_LG];
    size_t rlen = 0;
    int bad = parse_url(url, host, &port, path, &is_https) != 0 ||
              encode_path(path, epath, sizeof(epath)) != 0 ||
              (content_type && has_ctl(content_type));

    if (!bad) {
        int v6 = strchr(host, ':') != NULL;
        int default_port = port == (is_https ? 443 : 80);
        buf_appendf(request, sizeof(request), &rlen,
                    "%s %s HTTP/1.1\r\nHost: %s%s%s", method, epath,
                    v6 ? "[" : "", host, v6 ? "]" : "");
        if (!default_port)
            buf_appendf(request, sizeof(request), &rlen, ":%d", port);
        buf_appendf(request, sizeof(request), &rlen, "\r\nConnection: close\r\n");
        if (content_type)
            buf_appendf(request, sizeof(request), &rlen, "Content-Type: %s\r\n",
                        content_type);
        if (body_len > 0)
            buf_appendf(request, sizeof(request), &rlen, "Content-Length: %zu\r\n",
                        body_len);
        for (int h = 0; h < n_extra_hdrs; h++) {
            if (has_ctl(extra_hdrs[h].name) || has_ctl(extra_hdrs[h].value))
                bad = 1;
            buf_appendf(request, sizeof(request), &rlen, "%s: %s\r\n",
                        extra_hdrs[h].name, extra_hdrs[h].value);
        }
        buf_appendf(request, sizeof(request), &rlen, "\r\n");
        if (rlen >= sizeof(request) - 1)
            bad = 1;
    }
    n_extra_hdrs = 0;
    if (bad)
        return resp;

    int fd = net_connect(host, port, TC_CONNECT_TIMEOUT);
    if (fd < 0) {
        resp.status = HTTP_ERR_CONNECT;
        return resp;
    }
    req_timeout = timeout;  /* used by the I/O callbacks below */

    /* TLS setup if HTTPS */
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    br_sslio_context ioc;
    int use_tls = is_https;

    if (use_tls && tls_client_start(&sc, &xc, &ioc, iobuf, sizeof(iobuf), host, &fd,
                                    sock_read, net_write) != 0) {
        close(fd);
        req_timeout = 0;
        resp.status = HTTP_ERR_TLS;
        return resp;
    }

    /* Send request */
    int sent;
    if (use_tls) {
        sent = br_sslio_write_all(&ioc, request, rlen) == 0 &&
               (!body || body_len == 0 ||
                br_sslio_write_all(&ioc, body, body_len) == 0) &&
               br_sslio_flush(&ioc) == 0;
    } else {
        sent = write_all(fd, request, rlen) == 0 &&
               (!body || body_len == 0 || write_all(fd, body, body_len) == 0);
    }

    /* Read response (dynamically grown buffer) */
    size_t buf_cap = 65536;
    size_t buf_max = 4 * 1024 * 1024; /* 4MB max */
    char *buf = sent ? malloc(buf_cap) : NULL;
    size_t total = 0;

    while (buf && total < buf_max) {
        if (total + 8192 > buf_cap) {
            size_t new_cap = buf_cap * 2;
            if (new_cap > buf_max) new_cap = buf_max;
            char *tmp = realloc(buf, new_cap);
            if (!tmp) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = tmp;
            buf_cap = new_cap;
        }
        int n;
        if (use_tls)
            n = br_sslio_read(&ioc, buf + total, buf_cap - total - 1);
        else if (wait_readable(fd) != 0)
            break;
        else
            n = (int)read(fd, buf + total, buf_cap - total - 1);
        if (n <= 0) break;
        total += n;
    }

    int tls_err = use_tls ? br_ssl_engine_last_error(&sc.eng) : 0;
    if (use_tls)
        br_ssl_engine_close(&sc.eng);
    close(fd);
    req_timeout = 0;

    if (!buf) {
        if (sent)
            resp.status = HTTP_ERR_NOMEM;
        else if (req_timed_out)
            resp.status = HTTP_ERR_TIMEOUT;
        else if (tls_err)
            resp.status = HTTP_ERR_TLS;
        else
            resp.status = HTTP_ERR_CONNECT;
        if (tls_err && !req_timed_out)
            log_error("HTTP: TLS error %d with %s", tls_err, host);
        return resp;
    }
    buf[total] = '\0';

    /* Parse response */
    char *header_end = strstr(buf, "\r\n\r\n");
    if (!header_end || sscanf(buf, "HTTP/%*s %d", &resp.status) != 1) {
        if (req_timed_out)
            resp.status = HTTP_ERR_TIMEOUT;
        else if (total == 0 && tls_err)
            resp.status = HTTP_ERR_TLS;
        else
            resp.status = HTTP_ERR_PROTO;
        if (tls_err && total == 0)
            log_error("HTTP: TLS error %d with %s", tls_err, host);
        free(buf);
        return resp;
    }

    const char *ra = find_header(buf, header_end, "Retry-After");
    if (ra)
        resp.retry_after = atoi(ra);
    const char *loc = find_header(buf, header_end, "Location");
    if (loc)
        snprintf(resp.location, sizeof(resp.location), "%.*s",
                 (int)strcspn(loc, "\r\n"), loc);

    int is_chunked = 0;
    const char *te = find_header(buf, header_end, "Transfer-Encoding");
    if (te) {
        const char *eol = strstr(te, "\r\n");
        for (const char *p = te; p + 7 <= eol; p++)
            if (strncasecmp(p, "chunked", 7) == 0) { is_chunked = 1; break; }
    }

    /* Reuse the buffer for the body */
    char *body_start = header_end + 4;
    size_t blen = total - (size_t)(body_start - buf);
    memmove(buf, body_start, blen);
    buf[blen] = '\0';
    if (is_chunked)
        blen = http_dechunk(buf, blen);
    resp.body = buf;
    resp.body_len = blen;
    return resp;
}

/* Resolve a redirect target against the current URL; -1 if too long */
static int resolve_location(const char *cur, const char *loc, char *out, size_t sz) {
    const char *scheme_end = strstr(cur, "://");
    const char *origin_end = scheme_end ? scheme_end + 3 + strcspn(scheme_end + 3, "/?#")
                                        : cur + strlen(cur);
    int n;
    if (strstr(loc, "://"))                       /* absolute */
        n = snprintf(out, sz, "%s", loc);
    else if (loc[0] == '/' && loc[1] == '/')      /* scheme-relative */
        n = snprintf(out, sz, "%.*s%s", scheme_end ? (int)(scheme_end - cur + 1) : 0,
                     cur, loc);
    else if (loc[0] == '/')                       /* host-relative */
        n = snprintf(out, sz, "%.*s%s", (int)(origin_end - cur), cur, loc);
    else {                                        /* relative to the current directory */
        const char *path_end = origin_end + strcspn(origin_end, "?#");
        const char *dir_end = path_end;
        while (dir_end > origin_end && dir_end[-1] != '/') dir_end--;
        n = dir_end > origin_end
            ? snprintf(out, sz, "%.*s%s", (int)(dir_end - cur), cur, loc)
            : snprintf(out, sz, "%.*s/%s", (int)(origin_end - cur), cur, loc);
    }
    return n > 0 && (size_t)n < sz ? 0 : -1;
}

/* GET follows redirects: APIs move (http -> https, old -> new domain).
 * Extra headers only go to the first host. */
http_response_t http_get(const char *url) {
    char cur[2048], next[2048];
    if (snprintf(cur, sizeof(cur), "%s", url) >= (int)sizeof(cur))
        return (http_response_t){ .status = HTTP_ERR_REQUEST };
    http_response_t r = do_request("GET", cur, NULL, NULL, 0);
    for (int hop = 0; hop < 3 && r.location[0] &&
                      (r.status == 301 || r.status == 302 || r.status == 303 ||
                       r.status == 307 || r.status == 308); hop++) {
        if (resolve_location(cur, r.location, next, sizeof(next)) != 0)
            break;
        http_response_free(&r);
        memcpy(cur, next, sizeof(cur));
        r = do_request("GET", cur, NULL, NULL, 0);
    }
    return r;
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
int http_trace_fd = -1;

static int copy_response(const char *method, const char *url, http_response_t r,
                         char *buf, size_t buf_sz) {
    if (r.body)
        snprintf(buf, buf_sz, "%s", r.body);
    else
        snprintf(buf, buf_sz, "HTTP error %d (%s)", r.status, http_strerror(r.status));
    /* Shown to the builder when it test-runs a plugin */
    if (http_trace_fd >= 0) {
        char line[768], cut[96] = "";
        if (r.body && r.body_len >= buf_sz)
            snprintf(cut, sizeof(cut), ", cut to %zu of %zu bytes: use a bigger buffer",
                     buf_sz ? buf_sz - 1 : 0, r.body_len);
        int n = snprintf(line, sizeof(line), "[%s %s -> %d%s] %.*s", method, url,
                         r.status, cut, utf8_prefix(buf, 300), buf);
        if (n < 0) n = 0;
        if ((size_t)n > sizeof(line) - 2) n = (int)sizeof(line) - 2;
        for (int i = 0; i < n; i++)     /* one line per call */
            if (line[i] == '\n' || line[i] == '\r') line[i] = ' ';
        line[n++] = '\n';
        write_all(http_trace_fd, line, (size_t)n);
    }
    http_response_free(&r);
    return r.status;
}

int tc_http_get(const char *url, char *buf, size_t buf_sz) {
    return copy_response("GET", url, http_get(url), buf, buf_sz);
}

int tc_http_post(const char *url, const char *content_type,
                 const char *body, size_t body_len,
                 char *resp_buf, size_t resp_sz) {
    return copy_response("POST", url, http_post(url, content_type, body, body_len),
                         resp_buf, resp_sz);
}

int tc_http_post_json(const char *url, const char *json,
                      char *resp_buf, size_t resp_sz) {
    return tc_http_post(url, "application/json", json,
                        json ? strlen(json) : 0, resp_buf, resp_sz);
}
