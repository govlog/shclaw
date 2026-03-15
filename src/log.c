/*
 * log.c — file + stderr logging
 */

#include "../include/tc.h"

static FILE *log_fp = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_init(const char *log_dir) {
    mkdirs(log_dir);
    char path[4096];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(path, sizeof(path), "%s/shclaw_%04d-%02d-%02d.log",
             log_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    log_fp = fopen(path, "a");
    if (!log_fp)
        fprintf(stderr, "warning: cannot open log file %s\n", path);
}

void log_close(void) {
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
}

static void log_write(const char *level, const char *fmt, va_list ap) {
    char timebuf[32];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(timebuf, sizeof(timebuf), "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    char msg[TC_BUF_LG];
    vsnprintf(msg, sizeof(msg), fmt, ap);

    pthread_mutex_lock(&log_mutex);
    fprintf(stderr, "%s [%s] %s\n", timebuf, level, msg);
    if (log_fp) {
        fprintf(log_fp, "%s [%s] %s\n", timebuf, level, msg);
        fflush(log_fp);
    }
    pthread_mutex_unlock(&log_mutex);
}

void log_info(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    log_write("INFO", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    log_write("WARN", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    log_write("ERROR", fmt, ap);
    va_end(ap);
}

void log_debug(const char *fmt, ...) {
#ifdef DEBUG
    va_list ap; va_start(ap, fmt);
    log_write("DEBUG", fmt, ap);
    va_end(ap);
#else
    (void)fmt;
#endif
}
