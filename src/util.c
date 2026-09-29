/*
 * util.c — helpers: uuid, atomic write, file ops, time
 */

#include "../include/tc.h"
#include <limits.h>

void uuid_short(char *out, int len) {
    unsigned char buf[16];
    static const char hex[] = "0123456789abcdef";
    memset(buf, 0, sizeof(buf));

    if (!out || len <= 0) {
        if (out)
            out[0] = '\0';
        return;
    }

    if (len > 32)
        len = 32;

    FILE *f = fopen("/dev/urandom", "r");
    if (f) {
        size_t rd = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        if (rd == sizeof(buf)) goto have_entropy;
    }

    {
        /* Fallback: time-based */
        uint64_t t = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)out;
        memcpy(buf, &t, 8);
        t = ~t * 6364136223846793005ULL + 1;
        memcpy(buf + 8, &t, 8);
    }

have_entropy:
    for (int i = 0; i < len; i++) {
        unsigned char b = buf[i / 2];
        out[i] = hex[(i & 1) ? (b & 0x0f) : (b >> 4)];
    }
    out[len] = '\0';
}

int write_all(int fd, const void *buf, size_t len) {
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

int atomic_write(const char *path, const char *data, size_t len, mode_t mode) {
    static int seq;
    char tmp[4200];
    /* Unique per call: several threads may write the same path */
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d.%d", path, (int)getpid(),
             __sync_fetch_and_add(&seq, 1));

    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (fd < 0) return -1;

    int err = write_all(fd, data, len);
    fsync(fd);
    close(fd);

    if (err != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

char *file_slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz < 0) { fclose(f); return NULL; }

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    buf[rd] = '\0';

    if (out_len) *out_len = rd;
    return buf;
}

int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int mkdirs(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

int mkdirs_for(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash) return 0;
    *slash = '\0';
    return tmp[0] ? mkdirs(tmp) : 0;
}

void buf_appendf(char *buf, size_t sz, size_t *off, const char *fmt, ...) {
    if (sz == 0 || *off >= sz - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, sz - *off, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    *off += (size_t)n;
    if (*off > sz - 1) *off = sz - 1;
}

int utf8_prefix(const char *s, int max) {
    int n = (int)strnlen(s, (size_t)max);
    if (n == max)
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

void cli_data_dir(char *out, size_t sz) {
    ini_t *cfg = ini_load("etc/config.ini");
    const char *dir = ini_get(cfg, "daemon", "data_dir");
    snprintf(out, sz, "%s", dir ? dir : "./data");
    ini_free(cfg);
}

/* ── JSON helpers ─────────────────────────────────────── */

const char *j_str(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* Small models often send numbers and booleans as strings: accept both. */
int j_int(cJSON *obj, const char *key, int def) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(item)) return item->valueint;
    if (cJSON_IsString(item)) {
        char *end;
        errno = 0;
        long v = strtol(item->valuestring, &end, 10);
        if (end != item->valuestring && *end == '\0' && errno == 0 &&
            v >= INT_MIN && v <= INT_MAX)
            return (int)v;
    }
    return def;
}

int j_bool(cJSON *obj, const char *key, int def) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (cJSON_IsNumber(item)) return item->valuedouble != 0;
    if (cJSON_IsString(item)) {
        const char *s = item->valuestring;
        if (!strcasecmp(s, "true") || !strcasecmp(s, "yes") || !strcmp(s, "1"))
            return 1;
        if (!strcasecmp(s, "false") || !strcasecmp(s, "no") || !strcmp(s, "0"))
            return 0;
    }
    return def;
}

cJSON *json_load(const char *path, int want_array) {
    char *data = file_slurp(path, NULL);
    if (!data)
        return errno == ENOENT ? (want_array ? cJSON_CreateArray() : cJSON_CreateObject())
                               : NULL;
    cJSON *json = cJSON_Parse(data);
    free(data);
    if (want_array ? cJSON_IsArray(json) : cJSON_IsObject(json))
        return json;
    cJSON_Delete(json);
    log_error("%s is not valid JSON: fix or delete it", path);
    return NULL;
}

void utf8_scrub(char *s) {
    unsigned char *p = (unsigned char *)s;
    while (*p) {
        int n = *p < 0x80 ? 0 : *p < 0xC2 ? -1 : *p < 0xE0 ? 1 : *p < 0xF0 ? 2 :
                *p < 0xF5 ? 3 : -1;
        int ok = n >= 0;
        for (int i = 1; ok && i <= n; i++)
            ok = (p[i] & 0xC0) == 0x80;
        /* No overlong forms, surrogates or code points past U+10FFFF */
        if (ok && n == 2)
            ok = !(p[0] == 0xE0 && p[1] < 0xA0) && !(p[0] == 0xED && p[1] > 0x9F);
        if (ok && n == 3)
            ok = !(p[0] == 0xF0 && p[1] < 0x90) && !(p[0] == 0xF4 && p[1] > 0x8F);
        if (!ok) {
            *p++ = '?';
            continue;
        }
        p += n + 1;
    }
}

/* Only daemon state goes through here: keep it private to the owner. */
int json_save_atomic(const char *path, cJSON *obj, int formatted) {
    char *json = formatted ? cJSON_Print(obj) : cJSON_PrintUnformatted(obj);
    if (!json) return -1;
    int ret = atomic_write(path, json, strlen(json), 0600);
    free(json);
    return ret;
}

void format_time(time_t t, int local, char *buf, size_t sz) {
    struct tm tm;
    if (local) localtime_r(&t, &tm); else gmtime_r(&t, &tm);
    snprintf(buf, sz, "%04d-%02d-%02dT%02d:%02d:%02d%s",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, local ? "" : "Z");
}

void now_iso(char *buf, size_t sz) {
    format_time(time(NULL), 0, buf, sz);
}

int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
