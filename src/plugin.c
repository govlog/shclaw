/*
 * plugin.c — hot-reload C plugins via TinyCC in-memory compilation
 */

#include "../include/tc.h"

#ifdef TC_NO_PLUGINS

/* Stubs when plugins are disabled (e.g. Cosmopolitan/APE build) */

void plugin_init(plugin_registry_t *r, const char *plugins_dir) {
    (void)plugins_dir;
    memset(r, 0, sizeof(*r));
    pthread_mutex_init(&r->lock, NULL);
}

void plugin_scan(plugin_registry_t *r) { (void)r; }

int plugin_compile(plugin_registry_t *r, const char *src_path, time_t mtime) {
    (void)r; (void)src_path; (void)mtime;
    return -1;
}

cJSON *plugin_get_schemas(plugin_registry_t *r) {
    (void)r;
    return cJSON_CreateArray();
}

const char *plugin_execute(plugin_registry_t *r, const char *name, cJSON *input,
                           char *out, size_t out_sz) {
    (void)r; (void)name; (void)input;
    snprintf(out, out_sz, "Plugins not available in this build");
    return NULL;
}

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

void plugin_init(plugin_registry_t *r, const char *plugins_dir) {
    snprintf(r->dir, sizeof(r->dir), "%s", plugins_dir);
    pthread_mutex_init(&r->lock, NULL);
    r->count = 0;
    r->n_failed = 0;
    if (!tc_plugins_available()) return;
    mkdirs(plugins_dir);
    plugin_scan(r);
}

/* Check if this src+mtime already failed compilation */
static int is_known_failure(plugin_registry_t *r, const char *src, time_t mtime) {
    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < r->n_failed; i++) {
        if (r->failed_mtime[i] == mtime && strcmp(r->failed_src[i], src) == 0) {
            pthread_mutex_unlock(&r->lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&r->lock);
    return 0;
}

static void record_failure(plugin_registry_t *r, const char *src, time_t mtime) {
    pthread_mutex_lock(&r->lock);
    if (r->n_failed >= TC_MAX_PLUGINS) r->n_failed = 0; /* wrap */
    snprintf(r->failed_src[r->n_failed], sizeof(r->failed_src[0]), "%s", src);
    r->failed_mtime[r->n_failed] = mtime;
    r->n_failed++;
    pthread_mutex_unlock(&r->lock);
}

static void tcc_error_handler(void *opaque, const char *msg) {
    plugin_registry_t *r = (plugin_registry_t *)opaque;
    log_error("tcc: %s", msg);
    /* Append to last_error so the agent can see what went wrong */
    if (r) {
        size_t cur = strlen(r->last_error);
        if (cur + 1 < sizeof(r->last_error))
            snprintf(r->last_error + cur, sizeof(r->last_error) - cur,
                     "%s%s", cur ? "\n" : "", msg);
    }
}

/* ── Plugin-facing wrappers for libc primitives ── */

static int tc_plugin_strlen(const char *s) { return (int)strlen(s); }

static int tc_plugin_read_file(const char *path, char *buf, size_t buf_sz) {
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
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) {
            close(fd);
            return -1;
        }
        written += (size_t)n;
    }
    close(fd);
    return 0;
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

int plugin_compile(plugin_registry_t *r, const char *src_path, time_t mtime) {
    if (!tc_plugins_available()) {
        log_warn("plugin: not supported on this platform");
        return -1;
    }
    TCCState *tcc = tcc_new();
    if (!tcc) {
        log_error("plugin: tcc_new() failed");
        return -1;
    }

    r->last_error[0] = '\0';
    tcc_set_error_func(tcc, r, tcc_error_handler);
    tcc_set_options(tcc, "-nostdlib -nostdinc");
    tcc_set_lib_path(tcc, ".");
    tcc_set_output_type(tcc, TCC_OUTPUT_MEMORY);
    tcc_add_include_path(tcc, "include");

    /* Inject string/memory primitives */
    tcc_add_symbol(tcc, "tc_malloc",          malloc);
    tcc_add_symbol(tcc, "tc_free",            free);
    tcc_add_symbol(tcc, "tc_strlen",          tc_plugin_strlen);
    tcc_add_symbol(tcc, "tc_memcpy",          memcpy);
    tcc_add_symbol(tcc, "tc_memset",          memset);
    tcc_add_symbol(tcc, "tc_strcmp",           strcmp);
    tcc_add_symbol(tcc, "tc_strncmp",         strncmp);
    tcc_add_symbol(tcc, "tc_strcpy",          strcpy);
    tcc_add_symbol(tcc, "tc_strncpy",         strncpy);
    tcc_add_symbol(tcc, "tc_snprintf",        snprintf);

    /* Syscall helpers */
    tcc_add_symbol(tcc, "tc_read_file",       tc_plugin_read_file);
    tcc_add_symbol(tcc, "tc_write_file",      tc_plugin_write_file);
    tcc_add_symbol(tcc, "tc_gethostname",     tc_plugin_gethostname);

    /* HTTP */
    tcc_add_symbol(tcc, "tc_http_get",        tc_http_get);
    tcc_add_symbol(tcc, "tc_http_post",       tc_http_post);
    tcc_add_symbol(tcc, "tc_http_post_json",  tc_http_post_json);
    tcc_add_symbol(tcc, "tc_http_header",     tc_http_header);

    /* Logging */
    tcc_add_symbol(tcc, "tc_log",             log_info);

    /* JSON (cJSON wrappers) */
    tcc_add_symbol(tcc, "tc_json_parse",      cJSON_Parse);
    tcc_add_symbol(tcc, "tc_json_free",       cJSON_Delete);
    tcc_add_symbol(tcc, "tc_json_print",      cJSON_Print);
    tcc_add_symbol(tcc, "tc_json_get",        cJSON_GetObjectItem);
    tcc_add_symbol(tcc, "tc_json_index",      cJSON_GetArrayItem);
    tcc_add_symbol(tcc, "tc_json_array_size", cJSON_GetArraySize);
    tcc_add_symbol(tcc, "tc_json_string",     tc_plugin_json_string);
    tcc_add_symbol(tcc, "tc_json_int",        tc_plugin_json_int);
    tcc_add_symbol(tcc, "tc_json_double",     tc_plugin_json_double);

    if (tcc_add_file(tcc, src_path) == -1) {
        log_error("plugin: compile failed: %s", src_path);
        record_failure(r, src_path, mtime);
        tcc_delete(tcc);
        return -1;
    }

    if (tcc_relocate(tcc) == -1) {
        log_error("plugin: relocate failed: %s", src_path);
        record_failure(r, src_path, mtime);
        tcc_delete(tcc);
        return -1;
    }

    /* Look up exported symbols */
    const char **pname = tcc_get_symbol(tcc, "TC_PLUGIN_NAME");
    const char *(*exec_fn)(const char *) = tcc_get_symbol(tcc, "tc_execute");

    if (!pname || !exec_fn) {
        log_error("plugin: %s missing TC_PLUGIN_NAME or tc_execute", src_path);
        record_failure(r, src_path, mtime);
        tcc_delete(tcc);
        return -1;
    }

    const char **pdesc = tcc_get_symbol(tcc, "TC_PLUGIN_DESC");

    pthread_mutex_lock(&r->lock);

    /* Find or create slot */
    int slot = -1;
    for (int i = 0; i < r->count; i++) {
        if (strcmp(r->plugins[i].name, *pname) == 0) {
            slot = i;
            /* Free old TCC state */
            if (r->plugins[i].tcc_state)
                tcc_delete(r->plugins[i].tcc_state);
            break;
        }
    }
    if (slot < 0) {
        if (r->count >= TC_MAX_PLUGINS) {
            pthread_mutex_unlock(&r->lock);
            tcc_delete(tcc);
            return -1;
        }
        slot = r->count++;
    }

    plugin_entry_t *p = &r->plugins[slot];
    snprintf(p->name, sizeof(p->name), "%s", *pname);
    snprintf(p->src_path, sizeof(p->src_path), "%s", src_path);
    p->tcc_state = tcc;  /* keep alive — symbols live in TCC's relocated memory */
    p->execute = exec_fn;
    p->mtime = mtime;

    /* Build schema */
    if (p->schema) cJSON_Delete(p->schema);
    p->schema = cJSON_CreateObject();
    cJSON_AddStringToObject(p->schema, "name", *pname);
    cJSON_AddStringToObject(p->schema, "description", pdesc ? *pdesc : "Plugin tool");

    /* Check if plugin exports TC_PLUGIN_SCHEMA (JSON string for input_schema) */
    const char **pschema = tcc_get_symbol(tcc, "TC_PLUGIN_SCHEMA");
    cJSON *input_schema = NULL;
    if (pschema && *pschema) {
        input_schema = cJSON_Parse(*pschema);
        if (!input_schema)
            log_warn("plugin: %s has invalid TC_PLUGIN_SCHEMA JSON, using empty", *pname);
    }
    if (!input_schema) {
        input_schema = cJSON_CreateObject();
        cJSON_AddStringToObject(input_schema, "type", "object");
        cJSON_AddItemToObject(input_schema, "properties", cJSON_CreateObject());
    }
    cJSON_AddItemToObject(p->schema, "input_schema", input_schema);

    pthread_mutex_unlock(&r->lock);

    log_info("plugin: loaded %s from %s", *pname, src_path);
    return 0;
}

void plugin_scan(plugin_registry_t *r) {
    if (!tc_plugins_available()) return;
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

        if (need_compile) {
            /* Skip if this exact src+mtime already failed */
            if (is_known_failure(r, src, st.st_mtime))
                continue;
            plugin_compile(r, src, st.st_mtime);
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
                           char *out, size_t out_sz) {
    char *input_json = input ? cJSON_PrintUnformatted(input) : strdup("{}");
    if (!input_json) {
        snprintf(out, out_sz, "Plugin input serialization failed");
        return out;
    }

    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < r->count; i++) {
        if (strcmp(r->plugins[i].name, name) == 0 && r->plugins[i].execute) {
            const char *(*fn)(const char *) = r->plugins[i].execute;
            const char *result = fn(input_json);
            snprintf(out, out_sz, "%s", result ? result : "Plugin returned NULL");
            pthread_mutex_unlock(&r->lock);
            free(input_json);
            return out;
        }
    }
    pthread_mutex_unlock(&r->lock);
    free(input_json);
    return NULL;
}

#endif /* TC_NO_PLUGINS */
