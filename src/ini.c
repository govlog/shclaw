/*
 * ini.c — minimal INI config parser
 */

#include "../include/tc.h"

#define INI_INITIAL_CAP 64

static void ini_rstrip(char *s) {
    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
            break;
        s[--len] = '\0';
    }
}

static void ini_append(char *dst, size_t dst_sz, const char *src) {
    size_t used = strlen(dst);
    if (used + 1 >= dst_sz) return;
    snprintf(dst + used, dst_sz - used, "%s", src);
}

static int ini_read_logical_line(FILE *f, char *out, size_t out_sz) {
    char line[TC_BUF_LG];
    int have_line = 0;

    out[0] = '\0';

    while (fgets(line, sizeof(line), f)) {
        size_t len;
        int continued;

        len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        continued = (len > 0 && line[len - 1] == '\\');
        if (continued)
            line[--len] = '\0';

        ini_append(out, out_sz, line);
        have_line = 1;

        if (!continued)
            return 1;
    }

    return have_line;
}

static void ini_unescape(char *dst, size_t dst_sz, const char *src) {
    size_t di = 0;

    for (size_t i = 0; src[i] && di + 1 < dst_sz; i++) {
        if (src[i] != '\\' || !src[i + 1]) {
            dst[di++] = src[i];
            continue;
        }

        i++;
        switch (src[i]) {
        case 'n':  dst[di++] = '\n'; break;
        case 'r':  dst[di++] = '\r'; break;
        case 't':  dst[di++] = '\t'; break;
        case '\\': dst[di++] = '\\'; break;
        case '"':  dst[di++] = '"'; break;
        default:
            dst[di++] = '\\';
            if (di + 1 < dst_sz)
                dst[di++] = src[i];
            break;
        }
    }

    dst[di] = '\0';
}

ini_t *ini_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    ini_t *ini = calloc(1, sizeof(ini_t));
    ini->capacity = INI_INITIAL_CAP;
    ini->entries = calloc(ini->capacity, sizeof(ini_entry_t));

    char section[64] = "";
    char line[TC_BUF_XL];

    while (ini_read_logical_line(f, line, sizeof(line))) {
        char value[TC_BUF_XL];
        char *end;

        ini_rstrip(line);

        /* Skip empty lines and comments */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == ';' || *p == '#')
            continue;

        /* Section header */
        if (*p == '[') {
            char *close = strchr(p, ']');
            if (close) {
                *close = '\0';
                snprintf(section, sizeof(section), "%s", p + 1);
            }
            continue;
        }

        /* Key = value */
        char *eq = strchr(p, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        /* Trim key */
        end = key + strlen(key);
        while (end > key && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';

        /* Trim value leading spaces */
        while (*val == ' ' || *val == '\t') val++;

        /* Grow if needed */
        if (ini->count >= ini->capacity) {
            ini->capacity *= 2;
            ini->entries = realloc(ini->entries, ini->capacity * sizeof(ini_entry_t));
        }

        ini_entry_t *e = &ini->entries[ini->count++];
        snprintf(e->section, sizeof(e->section), "%s", section);
        snprintf(e->key, sizeof(e->key), "%s", key);
        ini_unescape(value, sizeof(value), val);
        e->value = strdup(value);
    }

    fclose(f);
    return ini;
}

void ini_free(ini_t *ini) {
    if (!ini) return;
    for (int i = 0; i < ini->count; i++)
        free(ini->entries[i].value);
    free(ini->entries);
    free(ini);
}

const char *ini_get(ini_t *ini, const char *section, const char *key) {
    if (!ini) return NULL;
    for (int i = 0; i < ini->count; i++) {
        if (strcmp(ini->entries[i].section, section) == 0 &&
            strcmp(ini->entries[i].key, key) == 0)
            return ini->entries[i].value;
    }
    return NULL;
}

int ini_get_int(ini_t *ini, const char *section, const char *key, int def) {
    const char *v = ini_get(ini, section, key);
    return v ? atoi(v) : def;
}

int ini_get_bool(ini_t *ini, const char *section, const char *key, int def) {
    const char *v = ini_get(ini, section, key);
    if (!v) return def;
    return (strcmp(v, "true") == 0 || strcmp(v, "1") == 0 ||
            strcmp(v, "yes") == 0);
}

int ini_section_foreach(ini_t *ini, const char *section,
                        void (*cb)(const char *key, const char *val, void *ctx),
                        void *ctx) {
    if (!ini) return 0;
    int count = 0;
    for (int i = 0; i < ini->count; i++) {
        if (strcmp(ini->entries[i].section, section) == 0) {
            cb(ini->entries[i].key, ini->entries[i].value, ctx);
            count++;
        }
    }
    return count;
}
