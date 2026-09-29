/*
 * plugin.c — hot-reload C plugins via TinyCC in-memory compilation
 */

#include "../include/tc.h"

#ifdef TC_NO_PLUGINS

/* Stubs when plugins are disabled */

void plugin_init(plugin_registry_t *r, const char *plugins_dir) {
    (void)plugins_dir;
    memset(r, 0, sizeof(*r));
    pthread_mutex_init(&r->lock, NULL);
}

void plugin_scan(plugin_registry_t *r) { (void)r; }

int plugin_compile(plugin_registry_t *r, const char *src_path, const char *code,
                   time_t mtime, char *err, size_t err_sz) {
    (void)r; (void)src_path; (void)code; (void)mtime;
    snprintf(err, err_sz, "plugins disabled in this build");
    return -1;
}

cJSON *plugin_get_schemas(plugin_registry_t *r) {
    (void)r;
    return cJSON_CreateArray();
}

const char *plugin_execute(plugin_registry_t *r, const char *name, cJSON *input,
                           int trace, char *out, size_t out_sz) {
    (void)r; (void)name; (void)input; (void)trace; (void)out; (void)out_sz;
    return NULL;
}

const char *plugin_api_names(void) { return ""; }

#else /* !TC_NO_PLUGINS */

#include <libtcc.h>
#include <dirent.h>

/*
 * Runtime platform check for Cosmopolitan APE builds.
 * TCC generates x86_64 ELF — works on Linux/NetBSD/FreeBSD,
 * but not on Windows (different executable format expectations).
 */
#ifdef __COSMOPOLITAN__
#include <cosmo.h>
static int tc_plugins_available(void) { return !IsWindows(); }
#else
static int tc_plugins_available(void) { return 1; }
#endif

/* One compile at a time, so no thread is inside TinyCC when we fork */
static pthread_mutex_t compile_lock = PTHREAD_MUTEX_INITIALIZER;

void plugin_init(plugin_registry_t *r, const char *plugins_dir) {
    snprintf(r->dir, sizeof(r->dir), "%s", plugins_dir);
    pthread_mutex_init(&r->lock, NULL);
    r->count = 0;
    r->n_failed = 0;
    if (!tc_plugins_available()) return;
    plugin_api_names();   /* build the list once, before any thread needs it */
    mkdirs(plugins_dir);
    plugin_scan(r);
}

/* Check if this src+mtime already failed compilation */
static int is_known_failure(plugin_registry_t *r, const char *src, time_t mtime) {
    int found = 0;
    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < r->n_failed && !found; i++)
        found = r->failed_mtime[i] == mtime && strcmp(r->failed_src[i], src) == 0;
    pthread_mutex_unlock(&r->lock);
    return found;
}

static void record_failure(plugin_registry_t *r, const char *src, time_t mtime) {
    pthread_mutex_lock(&r->lock);
    if (r->n_failed >= TC_MAX_PLUGINS) r->n_failed = 0; /* wrap */
    snprintf(r->failed_src[r->n_failed], sizeof(r->failed_src[0]), "%s", src);
    r->failed_mtime[r->n_failed] = mtime;
    r->n_failed++;
    pthread_mutex_unlock(&r->lock);
}

/* Compiler diagnostics go to a buffer, or to a pipe from the check child */
typedef struct {
    char  *buf;
    size_t sz;
    int    fd;
} diag_t;

static void diag(void *opaque, const char *msg) {
    diag_t *d = opaque;
    if (d->fd >= 0) {
        write_all(d->fd, msg, strlen(msg));
        write_all(d->fd, "\n", 1);
        return;
    }
    size_t cur = strlen(d->buf);
    if (cur + 1 < d->sz)
        snprintf(d->buf + cur, d->sz - cur, "%s%s", cur ? "\n" : "", msg);
}

/* ── Plugin-facing wrappers for libc primitives ── */

/* Plugins get wrappers, never libc's own addresses: with Cosmopolitan,
 * taking the address of strstr() or strcpy() breaks them for the whole
 * program (they are IFUNCs), and cJSON or getaddrinfo() then crash. */
static void *tc_plugin_malloc(size_t n) { return malloc(n); }
static void *tc_plugin_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *tc_plugin_memset(void *s, int c, size_t n) { return memset(s, c, n); }
static int tc_plugin_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static int tc_plugin_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static char *tc_plugin_strcpy(char *d, const char *s) { return strcpy(d, s); }
static char *tc_plugin_strncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static char *tc_plugin_strstr(const char *h, const char *n) { return strstr(h, n); }
static char *tc_plugin_strchr(const char *s, int c) { return strchr(s, c); }
static char *tc_plugin_strrchr(const char *s, int c) { return strrchr(s, c); }
static char *tc_plugin_strcat(char *d, const char *s) { return strcat(d, s); }
static char *tc_plugin_strncat(char *d, const char *s, size_t n) { return strncat(d, s, n); }
static char *tc_plugin_strdup(const char *s) { return strdup(s); }
static int tc_plugin_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
/* ASCII only, no locale */
static int tc_plugin_isdigit(int c) { return c >= '0' && c <= '9'; }
static int tc_plugin_isalpha(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static int tc_plugin_isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static int tc_plugin_tolower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int tc_plugin_toupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int tc_plugin_strlen(const char *s) { return (int)strlen(s); }
/* Each plugin call runs in a child that exits right after: memory is
 * reclaimed then, and freeing early only enables use-after-free bugs. */
static void tc_plugin_free(void *p) { (void)p; }
static int tc_plugin_atoi(const char *s) { return atoi(s); }

static int tc_plugin_snprintf(char *buf, size_t sz, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sz, fmt, ap);
    va_end(ap);
    return n;
}

static int tc_plugin_read_file(const char *path, char *buf, size_t buf_sz) {
    if (buf_sz == 0) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, buf_sz - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return (int)n;
}

static int tc_plugin_write_file(const char *path, const char *data, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int rc = write_all(fd, data, len);
    close(fd);
    return rc;
}

static int tc_plugin_gethostname(char *buf, size_t sz) {
    return gethostname(buf, sz);
}

static const char *tc_plugin_json_string(void *node) {
    return cJSON_GetStringValue((cJSON *)node);
}

static int tc_plugin_json_int(void *node) {
    cJSON *n = (cJSON *)node;
    return (n && cJSON_IsNumber(n)) ? n->valueint : 0;
}

static double tc_plugin_json_double(void *node) {
    cJSON *n = (cJSON *)node;
    return (n && cJSON_IsNumber(n)) ? n->valuedouble : 0.0;
}

static const struct { const char *name; const void *fn; } plugin_api[] = {
    /* String/memory primitives */
    {"tc_malloc",          tc_plugin_malloc},
    {"tc_free",            tc_plugin_free},
    {"tc_strlen",          tc_plugin_strlen},
    {"tc_memcpy",          tc_plugin_memcpy},
    {"tc_memset",          tc_plugin_memset},
    {"tc_strcmp",          tc_plugin_strcmp},
    {"tc_strncmp",         tc_plugin_strncmp},
    {"tc_strcpy",          tc_plugin_strcpy},
    {"tc_strncpy",         tc_plugin_strncpy},
    {"tc_snprintf",        tc_plugin_snprintf},
    {"tc_strstr",          tc_plugin_strstr},
    {"tc_strchr",          tc_plugin_strchr},
    {"tc_strrchr",         tc_plugin_strrchr},
    {"tc_strcat",          tc_plugin_strcat},
    {"tc_strncat",         tc_plugin_strncat},
    {"tc_strdup",          tc_plugin_strdup},
    {"tc_memcmp",          tc_plugin_memcmp},
    {"tc_isdigit",         tc_plugin_isdigit},
    {"tc_isalpha",         tc_plugin_isalpha},
    {"tc_isspace",         tc_plugin_isspace},
    {"tc_tolower",         tc_plugin_tolower},
    {"tc_toupper",         tc_plugin_toupper},
    {"tc_atoi",            tc_plugin_atoi},
    /* Syscall helpers */
    {"tc_read_file",       tc_plugin_read_file},
    {"tc_write_file",      tc_plugin_write_file},
    {"tc_gethostname",     tc_plugin_gethostname},
    /* HTTP */
    {"tc_http_get",        tc_http_get},
    {"tc_http_post",       tc_http_post},
    {"tc_http_post_json",  tc_http_post_json},
    {"tc_http_header",     tc_http_header},
    /* Logging */
    {"tc_log",             log_info},
    /* JSON (cJSON wrappers) */
    {"tc_json_parse",      cJSON_Parse},
    {"tc_json_free",       tc_plugin_free},
    {"tc_json_print",      cJSON_Print},
    {"tc_json_get",        cJSON_GetObjectItem},
    {"tc_json_index",      cJSON_GetArrayItem},
    {"tc_json_array_size", cJSON_GetArraySize},
    {"tc_json_string",     tc_plugin_json_string},
    {"tc_json_int",        tc_plugin_json_int},
    {"tc_json_double",     tc_plugin_json_double},
};

const char *plugin_api_names(void) {
    static char names[TC_BUF_MD];
    if (!names[0]) {
        size_t off = 0;
        for (size_t i = 0; i < sizeof(plugin_api) / sizeof(plugin_api[0]); i++)
            buf_appendf(names, sizeof(names), &off, "%s%s", i ? ", " : "",
                        plugin_api[i].name);
    }
    return names;
}

/* Compile (code, or the file src when code is NULL) and relocate; returns
 * the plugin name, or NULL with the reason reported through diag. */
static const char *build(TCCState *tcc, const char *src, const char *code, diag_t *d,
                         const char *(**exec_fn)(const char *),
                         const char **desc, const char **schema) {
    tcc_set_error_func(tcc, d, diag);
    tcc_set_options(tcc, "-nostdlib -nostdinc");
    tcc_set_lib_path(tcc, ".");
    tcc_set_output_type(tcc, TCC_OUTPUT_MEMORY);
    tcc_add_include_path(tcc, "include");
    for (size_t i = 0; i < sizeof(plugin_api) / sizeof(plugin_api[0]); i++)
        tcc_add_symbol(tcc, plugin_api[i].name, plugin_api[i].fn);

    if ((code ? tcc_compile_string(tcc, code) : tcc_add_file(tcc, src)) == -1 ||
        tcc_relocate(tcc) == -1)
        return NULL;

    const char **pname = tcc_get_symbol(tcc, "TC_PLUGIN_NAME");
    *exec_fn = tcc_get_symbol(tcc, "tc_execute");
    if (!pname || !*exec_fn) {
        diag(d, "missing TC_PLUGIN_NAME or tc_execute");
        return NULL;
    }
    /* The name becomes an API tool name: a bad one breaks every agent */
    if (!*pname || !tool_name_valid(*pname)) {
        diag(d, "TC_PLUGIN_NAME must match [A-Za-z0-9_-]{1,64}");
        return NULL;
    }
    if (tool_find(*pname) >= 0) {
        diag(d, "TC_PLUGIN_NAME is already a built-in tool name");
        return NULL;
    }
    /* Read these here too: declared as arrays instead of pointers, they
     * crash the check child rather than the daemon */
    const char **pdesc = tcc_get_symbol(tcc, "TC_PLUGIN_DESC");
    const char **pschema = tcc_get_symbol(tcc, "TC_PLUGIN_SCHEMA");
    *desc = pdesc ? *pdesc : NULL;
    *schema = pschema ? *pschema : NULL;
    if ((*desc && strlen(*desc) > 1024) || (*schema && strlen(*schema) > 16384)) {
        diag(d, "TC_PLUGIN_DESC or TC_PLUGIN_SCHEMA is too long");
        return NULL;
    }
    return *pname;
}

/* TinyCC runs inside the daemon: compile first in a throwaway child, so a
 * compiler crash or a bad plugin symbol cannot take the daemon down. */
static int compile_check(const char *src, const char *code, char *err, size_t err_sz) {
    int pfd[2];
    if (pipe(pfd) != 0) {
        snprintf(err, err_sz, "pipe() failed");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        snprintf(err, err_sz, "fork() failed");
        return -1;
    }
    if (pid == 0) {
        close(pfd[0]);
        diag_t d = { NULL, 0, pfd[1] };
        const char *(*fn)(const char *), *desc, *schema;
        TCCState *tcc = tcc_new();
        _exit(tcc && build(tcc, src, code, &d, &fn, &desc, &schema) ? 0 : 1);
    }
    close(pfd[1]);

    size_t len;
    int st = child_collect(pid, pfd[0], 30, err, err_sz - 1, &len, NULL);
    while (len > 0 && err[len - 1] == '\n')
        err[--len] = '\0';
    if (st == -1) {
        snprintf(err, err_sz, "compiler timed out");
        return -1;
    }
    if (WIFSIGNALED(st)) {
        snprintf(err + len, err_sz - len, "%scompiler crashed (signal %d)",
                 len ? "\n" : "", WTERMSIG(st));
        return -1;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        if (!len) snprintf(err, err_sz, "compilation failed");
        return -1;
    }
    return 0;
}

int plugin_compile(plugin_registry_t *r, const char *src_path, const char *code,
                   time_t mtime, char *err, size_t err_sz) {
    err[0] = '\0';
    if (!tc_plugins_available()) {
        snprintf(err, err_sz, "plugins are not supported on this platform");
        return -1;
    }

    pthread_mutex_lock(&compile_lock);
    int rc = -1;
    char loaded[64] = "";
    diag_t d = { err, err_sz, -1 };
    const char *(*exec_fn)(const char *) = NULL;
    const char *name = NULL, *desc = NULL, *schema = NULL;
    TCCState *tcc = NULL;

    if (compile_check(src_path, code, err, err_sz) == 0) {
        tcc = tcc_new();
        if (tcc)
            name = build(tcc, src_path, code, &d, &exec_fn, &desc, &schema);
        else
            snprintf(err, err_sz, "tcc_new() failed");
    }

    if (name) {
        pthread_mutex_lock(&r->lock);
        /* Same file: replace. Same name from another file: refuse, or the
         * two files would replace each other on every scan. */
        int slot = -1, clash = -1;
        for (int i = 0; i < r->count; i++) {
            if (strcmp(r->plugins[i].src_path, src_path) == 0) slot = i;
            else if (strcmp(r->plugins[i].name, name) == 0) clash = i;
        }
        if (clash >= 0) {
            snprintf(err, err_sz, "plugin name '%s' is already used by %s",
                     name, r->plugins[clash].src_path);
        } else if (slot < 0 && r->count >= TC_MAX_PLUGINS) {
            snprintf(err, err_sz, "too many plugins (max %d)", TC_MAX_PLUGINS);
        } else {
            if (slot < 0) {
                slot = r->count++;
                memset(&r->plugins[slot], 0, sizeof(r->plugins[slot]));
            }
            plugin_entry_t *p = &r->plugins[slot];
            if (p->tcc_state) tcc_delete(p->tcc_state);
            cJSON_Delete(p->schema);
            snprintf(p->name, sizeof(p->name), "%s", name);
            snprintf(p->src_path, sizeof(p->src_path), "%s", src_path);
            p->tcc_state = tcc;  /* kept alive: symbols live in its memory */
            p->execute = exec_fn;
            p->mtime = mtime;

            p->schema = cJSON_CreateObject();
            cJSON_AddStringToObject(p->schema, "name", name);
            cJSON_AddStringToObject(p->schema, "description",
                                    desc && *desc ? desc : "Plugin tool");
            cJSON *input_schema = schema && *schema ? cJSON_Parse(schema) : NULL;
            if (!cJSON_IsObject(input_schema)) {
                if (schema && *schema)
                    log_warn("plugin: %s has invalid TC_PLUGIN_SCHEMA JSON, using empty", name);
                cJSON_Delete(input_schema);
                input_schema = cJSON_CreateObject();
            }
            /* The APIs require an object schema with properties */
            if (!cJSON_GetObjectItem(input_schema, "type"))
                cJSON_AddStringToObject(input_schema, "type", "object");
            if (!cJSON_GetObjectItem(input_schema, "properties"))
                cJSON_AddItemToObject(input_schema, "properties", cJSON_CreateObject());
            cJSON_AddItemToObject(p->schema, "input_schema", input_schema);
            snprintf(loaded, sizeof(loaded), "%s", name);
            tcc = NULL;
            rc = 0;
        }
        pthread_mutex_unlock(&r->lock);
    }
    if (tcc) tcc_delete(tcc);
    pthread_mutex_unlock(&compile_lock);

    if (rc == 0) {
        log_info("plugin: loaded %s from %s", loaded, src_path);
        snprintf(err, err_sz, "%s", loaded);
    } else {
        log_error("plugin: %s: %s", src_path, err);
        record_failure(r, src_path, mtime);
    }
    return rc;
}

void plugin_scan(plugin_registry_t *r) {
    if (!tc_plugins_available()) return;

    /* Forget plugins whose source file is gone */
    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < r->count; i++) {
        if (file_exists(r->plugins[i].src_path)) continue;
        log_info("plugin: unloaded %s (%s removed)", r->plugins[i].name,
                 r->plugins[i].src_path);
        tcc_delete(r->plugins[i].tcc_state);
        cJSON_Delete(r->plugins[i].schema);
        r->plugins[i--] = r->plugins[--r->count];
    }
    pthread_mutex_unlock(&r->lock);

    DIR *dir = opendir(r->dir);
    if (!dir) return;

    struct dirent *de;
    while ((de = readdir(dir))) {
        size_t len = strlen(de->d_name);
        if (len < 3 || strcmp(de->d_name + len - 2, ".c") != 0) continue;
        if (de->d_name[0] == '_') continue;

        char src[4200];
        snprintf(src, sizeof(src), "%s/%s", r->dir, de->d_name);

        struct stat st;
        if (stat(src, &st) != 0) continue;

        /* Check if already loaded with same mtime */
        int need_compile = 1;
        pthread_mutex_lock(&r->lock);
        for (int i = 0; i < r->count; i++) {
            if (strcmp(r->plugins[i].src_path, src) == 0 &&
                r->plugins[i].mtime == st.st_mtime) {
                need_compile = 0;
                break;
            }
        }
        pthread_mutex_unlock(&r->lock);

        /* Skip if this exact src+mtime already failed */
        if (need_compile && !is_known_failure(r, src, st.st_mtime)) {
            char err[TC_BUF_LG];
            plugin_compile(r, src, NULL, st.st_mtime, err, sizeof(err));
        }
    }

    closedir(dir);
}

cJSON *plugin_get_schemas(plugin_registry_t *r) {
    cJSON *arr = cJSON_CreateArray();
    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < r->count; i++) {
        if (r->plugins[i].schema)
            cJSON_AddItemToArray(arr, cJSON_Duplicate(r->plugins[i].schema, 1));
    }
    pthread_mutex_unlock(&r->lock);
    return arr;
}

const char *plugin_execute(plugin_registry_t *r, const char *name, cJSON *input,
                           int trace, char *out, size_t out_sz) {
    /* Run the plugin in a forked child: a crash or a hang (bad pointer,
     * endless loop) costs the child, not the daemon. */
    char *input_json = input ? cJSON_PrintUnformatted(input) : strdup("{}");
    int pfd[2];
    if (!input_json || pipe(pfd) != 0) {
        free(input_json);
        snprintf(out, out_sz, "Error: cannot start plugin '%s'", name);
        return out;
    }

    /* Hold the lock until the fork: a reload must not unmap the code first */
    const char *(*fn)(const char *) = NULL;
    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < r->count && !fn; i++)
        if (strcmp(r->plugins[i].name, name) == 0)
            fn = r->plugins[i].execute;
    pid_t pid = fn ? fork() : -1;
    if (pid == 0) {
        close(pfd[0]);
        if (trace) http_trace_fd = pfd[1];
        const char *result = fn(input_json);
        if (result)
            write_all(pfd[1], result, strnlen(result, out_sz - 1));
        _exit(0);
    }
    pthread_mutex_unlock(&r->lock);

    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        free(input_json);
        if (!fn)
            return NULL;
        snprintf(out, out_sz, "Error: cannot start plugin '%s'", name);
        return out;
    }
    close(pfd[1]);
    free(input_json);

    size_t len;
    int st = child_collect(pid, pfd[0], TC_PLUGIN_TIMEOUT, out, out_sz - 1, &len, NULL);
    if (st == -1) {
        snprintf(out, out_sz, "Error: plugin '%s' timed out after %ds (endless loop?)",
                 name, TC_PLUGIN_TIMEOUT);
        log_error("plugin: '%s' timed out", name);
    } else if (WIFSIGNALED(st)) {
        int sig = WTERMSIG(st);
        snprintf(out, out_sz,
                 "Error: plugin '%s' crashed (signal %d: %s). "
                 "Check the plugin code for bugs (bad pointers, buffer overflows, "
                 "uninitialized static variables).",
                 name, sig,
                 sig == SIGSEGV ? "segfault" :
                 sig == SIGABRT ? "abort" :
                 sig == SIGFPE  ? "floating point exception" :
                 sig == SIGBUS  ? "bus error" : "unknown");
        log_error("plugin: '%s' crashed with signal %d", name, sig);
    } else if (!out[0]) {
        snprintf(out, out_sz, "%s", TC_EMPTY_OUTPUT_MARKER);
    }
    return out;
}

#endif /* TC_NO_PLUGINS */
