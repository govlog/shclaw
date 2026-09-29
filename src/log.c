/*
 * log.c — file + stderr logging, one file per day
 */

#include "../include/tc.h"

static FILE *log_fp = NULL;
static char log_dir_path[4096];
static int log_day = -1;          /* tm_yday of the open file */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Hold the lock across fork(): a child (exec tool, plugin) must never
 * inherit it, or its stdio buffers, in a locked state. */
static void fork_prepare(void) { pthread_mutex_lock(&log_mutex); }
static void fork_release(void) { pthread_mutex_unlock(&log_mutex); }

/* Caller holds log_mutex */
static void log_open_day(const struct tm *tm) {
    if (log_fp) fclose(log_fp);
    char path[4200];
    snprintf(path, sizeof(path), "%s/shclaw_%04d-%02d-%02d.log",
             log_dir_path, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    log_fp = fopen(path, "a");
    if (!log_fp)
        fprintf(stderr, "warning: cannot open log file %s\n", path);
    log_day = tm->tm_yday;
}

void log_init(const char *log_dir) {
    static int atfork_done;
    if (!atfork_done) {
        pthread_atfork(fork_prepare, fork_release, fork_release);
        atfork_done = 1;
    }
    mkdirs_for(log_dir);
    mkdir(log_dir, 0700);   /* private when we create it */
    snprintf(log_dir_path, sizeof(log_dir_path), "%s", log_dir);
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    pthread_mutex_lock(&log_mutex);
    log_open_day(&tm);
    pthread_mutex_unlock(&log_mutex);
}

void log_close(void) {
    pthread_mutex_lock(&log_mutex);
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
    log_dir_path[0] = '\0';
    pthread_mutex_unlock(&log_mutex);
}

__attribute__((format(printf, 2, 0)))
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
    if (log_dir_path[0] && tm.tm_yday != log_day)
        log_open_day(&tm);
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
