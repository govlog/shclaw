/*
 * memory.c — JSONL memory log + key-value facts + tag-based search
 */

#include "../include/tc.h"

static char *memory_log_path(memory_t *m) {
    static __thread char path[4200];
    snprintf(path, sizeof(path), "%s/memory.jsonl", m->memory_dir);
    return path;
}

static char *facts_path(memory_t *m) {
    static __thread char path[4200];
    snprintf(path, sizeof(path), "%s/facts.json", m->memory_dir);
    return path;
}

static void load_cache(memory_t *m) {
    if (m->cache) return;
    m->cache = cJSON_CreateArray();

    char *data = file_slurp(memory_log_path(m), NULL);
    if (!data) return;

    char *saveptr = NULL;
    char *line = strtok_r(data, "\n", &saveptr);
    while (line) {
        if (line[0]) {
            cJSON *entry = cJSON_Parse(line);
            if (entry)
                cJSON_AddItemToArray(m->cache, entry);
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    free(data);
}

/* Auto-extract tags from content */
static void extract_tags(const char *content, cJSON *tags_arr) {
    if (!content || !tags_arr) return;

    /* Simple word extraction: words >= 4 chars, lowercased */
    char word[64];
    int wi = 0;
    int tag_count = 0;

    for (const char *p = content; ; p++) {
        int is_word_char = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                           (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
                           *p == '@' || *p == '.';
        if (is_word_char && wi < 62) {
            word[wi++] = (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p;
        } else {
            if (wi >= 4 && tag_count < 10) {
                word[wi] = '\0';
                /* Check not duplicate */
                int dup = 0;
                cJSON *item;
                cJSON_ArrayForEach(item, tags_arr)
                    if (cJSON_GetStringValue(item) && strcmp(cJSON_GetStringValue(item), word) == 0)
                        { dup = 1; break; }
                if (!dup) {
                    cJSON_AddItemToArray(tags_arr, cJSON_CreateString(word));
                    tag_count++;
                }
            }
            wi = 0;
        }
        if (*p == '\0') break;
    }
}

void memory_init(memory_t *m, const char *dir) {
    snprintf(m->memory_dir, sizeof(m->memory_dir), "%s", dir);
    mkdirs(dir);
    pthread_mutex_init(&m->lock, NULL);
    m->cache = NULL;
    m->dirty = 0;
}

const char *memory_add(memory_t *m, const char *content, const char *category,
                       int importance, const char *tags_csv,
                       char *out, size_t out_sz) {
    char mid[10], now[32];
    uuid_short(mid, 8);
    now_iso(now, sizeof(now));

    if (!content || !content[0]) {
        snprintf(out, out_sz, "Error: content is required");
        return out;
    }

    if (!category || !category[0]) category = "general";
    if (importance <= 0) importance = 5;
    if (importance > 10) importance = 10;

    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "id", mid);
    cJSON_AddStringToObject(entry, "content", content);
    cJSON_AddStringToObject(entry, "category", category);
    cJSON_AddNumberToObject(entry, "importance", importance);
    cJSON_AddStringToObject(entry, "timestamp", now);

    /* Build tags array */
    cJSON *tags = cJSON_CreateArray();

    /* User-provided tags */
    if (tags_csv && tags_csv[0]) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", tags_csv);
        char *saveptr = NULL;
        char *tok = strtok_r(buf, ",", &saveptr);
        while (tok) {
            while (*tok == ' ') tok++;
            char *end = tok + strlen(tok) - 1;
            while (end > tok && *end == ' ') *end-- = '\0';
            if (tok[0])
                cJSON_AddItemToArray(tags, cJSON_CreateString(tok));
            tok = strtok_r(NULL, ",", &saveptr);
        }
    }

    /* Auto-extracted tags */
    extract_tags(content, tags);
    cJSON_AddItemToObject(entry, "tags", tags);

    pthread_mutex_lock(&m->lock);
    load_cache(m);

    /* Append to JSONL file */
    char *json_line = cJSON_PrintUnformatted(entry);
    if (json_line) {
        FILE *f = fopen(memory_log_path(m), "a");
        if (f) {
            fprintf(f, "%s\n", json_line);
            fclose(f);
        }
        free(json_line);
    }

    /* Add to cache */
    cJSON_AddItemToArray(m->cache, cJSON_Duplicate(entry, 1));
    pthread_mutex_unlock(&m->lock);

    snprintf(out, out_sz, "Remembered (id=%s): %.80s", mid, content);
    cJSON_Delete(entry);
    return out;
}

const char *memory_search(memory_t *m, const char *query, int n,
                          char *out, size_t out_sz) {
    if (n <= 0) n = 20;

    pthread_mutex_lock(&m->lock);
    load_cache(m);

    int total = cJSON_GetArraySize(m->cache);

    cJSON *results = cJSON_CreateArray();
    int found = 0;

    if (!query || !query[0]) {
        /* Return recent */
        int start = total - n;
        if (start < 0) start = 0;
        for (int i = start; i < total && found < n; i++) {
            cJSON_AddItemToArray(results, cJSON_Duplicate(cJSON_GetArrayItem(m->cache, i), 1));
            found++;
        }
    } else {
        /* Keyword search (case-insensitive) */
        char q_lower[256];
        snprintf(q_lower, sizeof(q_lower), "%s", query);
        for (char *p = q_lower; *p; p++)
            if (*p >= 'A' && *p <= 'Z') *p += 32;

        /* Search backwards (most recent first) */
        for (int i = total - 1; i >= 0 && found < n; i--) {
            cJSON *entry = cJSON_GetArrayItem(m->cache, i);
            const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "content"));
            if (!content) continue;

            /* Lowercase content for comparison */
            char content_lower[TC_BUF_LG];
            snprintf(content_lower, sizeof(content_lower), "%s", content);
            for (char *p = content_lower; *p; p++)
                if (*p >= 'A' && *p <= 'Z') *p += 32;

            if (strstr(content_lower, q_lower)) {
                cJSON_AddItemToArray(results, cJSON_Duplicate(entry, 1));
                found++;
            }
        }
    }

    pthread_mutex_unlock(&m->lock);

    char *json = cJSON_Print(results);
    snprintf(out, out_sz, "%s", json ? json : "[]");
    free(json);
    cJSON_Delete(results);
    return out;
}

const char *memory_get_relevant(memory_t *m, cJSON *hints, int max_results,
                                char *out, size_t out_sz) {
    /* For now, delegate to search with first hint, padded with recent */
    const char *first_hint = NULL;
    if (hints && cJSON_GetArraySize(hints) > 0)
        first_hint = cJSON_GetStringValue(cJSON_GetArrayItem(hints, 0));

    return memory_search(m, first_hint, max_results, out, out_sz);
}

void memory_clear(memory_t *m) {
    pthread_mutex_lock(&m->lock);
    if (m->cache) {
        cJSON_Delete(m->cache);
        m->cache = NULL;
    }
    unlink(memory_log_path(m));
    pthread_mutex_unlock(&m->lock);
}

/* ── Facts ──────────────────────────────────────────────── */

static cJSON *load_facts(memory_t *m) {
    char *data = file_slurp(facts_path(m), NULL);
    if (!data) return cJSON_CreateObject();
    cJSON *obj = cJSON_Parse(data);
    free(data);
    return obj ? obj : cJSON_CreateObject();
}

static void save_facts(memory_t *m, cJSON *facts) {
    char *json = cJSON_Print(facts);
    if (json) {
        atomic_write(facts_path(m), json, strlen(json));
        free(json);
    }
}

const char *facts_set(memory_t *m, const char *key, const char *value,
                      char *out, size_t out_sz) {
    if (!key || !key[0] || !value) {
        snprintf(out, out_sz, "Error: key and value are required");
        return out;
    }

    pthread_mutex_lock(&m->lock);
    cJSON *facts = load_facts(m);

    cJSON *existing = cJSON_GetObjectItem(facts, key);
    if (existing)
        cJSON_SetValuestring(existing, value);
    else
        cJSON_AddStringToObject(facts, key, value);

    save_facts(m, facts);
    cJSON_Delete(facts);
    pthread_mutex_unlock(&m->lock);

    snprintf(out, out_sz, "Fact saved: %s = %s", key, value);
    return out;
}

void facts_clear(memory_t *m) {
    pthread_mutex_lock(&m->lock);
    atomic_write(facts_path(m), "{}", 2);
    pthread_mutex_unlock(&m->lock);
}

const char *facts_get(memory_t *m, const char *key,
                      char *out, size_t out_sz) {
    pthread_mutex_lock(&m->lock);
    cJSON *facts = load_facts(m);
    pthread_mutex_unlock(&m->lock);

    if (!key || !key[0]) {
        /* List all facts */
        char *json = cJSON_Print(facts);
        snprintf(out, out_sz, "%s", json ? json : "{}");
        free(json);
    } else {
        cJSON *val = cJSON_GetObjectItem(facts, key);
        if (val && cJSON_GetStringValue(val))
            snprintf(out, out_sz, "%s", cJSON_GetStringValue(val));
        else
            snprintf(out, out_sz, "(not found)");
    }

    cJSON_Delete(facts);
    return out;
}
