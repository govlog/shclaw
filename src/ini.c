/*
 * ini.c — minimal INI config parser
 */

#include "../include/tc.h"

#define INI_INITIAL_CAP 64

ini_t *ini_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    ini_t *ini = calloc(1, sizeof(ini_t));
    ini->capacity = INI_INITIAL_CAP;
    ini->entries = calloc(ini->capacity, sizeof(ini_entry_t));

    char section[64] = "";
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing whitespace / newline */
        char *end = line + strlen(line) - 1;
        while (end >= line && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t'))
            *end-- = '\0';

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
        end = key + strlen(key) - 1;
        while (end >= key && (*end == ' ' || *end == '\t')) *end-- = '\0';

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
        snprintf(e->value, sizeof(e->value), "%s", val);
    }

    fclose(f);
    return ini;
}

void ini_free(ini_t *ini) {
    if (!ini) return;
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
