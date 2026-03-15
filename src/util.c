/*
 * util.c — helpers: uuid, atomic write, file ops, time
 */

#include "../include/tc.h"
#include <bearssl.h>

void uuid_short(char *out, int len) {
    unsigned char buf[16];
    memset(buf, 0, sizeof(buf));
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
    for (int i = 0; i < len && i < 16; i++)
        sprintf(out + i * 2, "%02x", buf[i]);
    out[len * 2] = '\0';
    /* Truncate to requested length (chars, not bytes) */
    if (len < 16)
        out[len] = '\0';
    /* Actually, uuid_short(out, 5) should produce 5 hex chars = "a1b2c" */
    /* Let's use len as number of output chars */
    int n_bytes = (len + 1) / 2;
    if (n_bytes > 16) n_bytes = 16;
    char tmp[33];
    for (int i = 0; i < n_bytes; i++)
        sprintf(tmp + i * 2, "%02x", buf[i]);
    tmp[n_bytes * 2] = '\0';
    memcpy(out, tmp, len);
    out[len] = '\0';
}

int atomic_write(const char *path, const char *data, size_t len) {
    char tmp[4200];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp);
            return -1;
        }
        written += n;
    }
    fsync(fd);
    close(fd);

    if (rename(tmp, path) != 0) {
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

void now_iso(char *buf, size_t sz) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(buf, sz, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sha256_hex(const void *data, size_t len, char out[65]) {
    br_sha256_context ctx;
    unsigned char hash[32];
    br_sha256_init(&ctx);
    br_sha256_update(&ctx, data, len);
    br_sha256_out(&ctx, hash);
    for (int i = 0; i < 32; i++)
        sprintf(out + i * 2, "%02x", hash[i]);
    out[64] = '\0';
}
