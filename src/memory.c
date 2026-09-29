/*
 * memory.c — JSONL memory log + key-value facts + keyword search
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

static void lowercase(char *s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s += 32;
}

static int is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '@' || c == '.' ||
           (unsigned char)c >= 0x80;   /* UTF-8: keep accented words whole */
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

/* Auto-extract tags: words >= 4 chars, lowercased */
static void extract_tags(const char *content, cJSON *tags_arr) {
    char word[64];
    int wi = 0;
    int tag_count = 0;

    for (const char *p = content; ; p++) {
        if (*p && is_word_char(*p) && wi < 62) {
            word[wi++] = (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p;
            continue;
        }
        if (wi >= 4 && tag_count < 10) {
            word[wi] = '\0';
            int dup = 0;
            cJSON *item;
            cJSON_ArrayForEach(item, tags_arr) {
                const char *iv = cJSON_GetStringValue(item);
                if (iv && strcmp(iv, word) == 0) { dup = 1; break; }
            }
            if (!dup) {
                cJSON_AddItemToArray(tags_arr, cJSON_CreateString(word));
                tag_count++;
            }
        }
        wi = 0;
        if (*p == '\0') break;
    }
}

void memory_init(memory_t *m, const char *dir) {
    snprintf(m->memory_dir, sizeof(m->memory_dir), "%s", dir);
    mkdirs(dir);
    pthread_mutex_init(&m->lock, NULL);
    m->cache = NULL;
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

    /* User-provided tags, then auto-extracted ones */
    cJSON *tags = cJSON_CreateArray();
    if (tags_csv && tags_csv[0]) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", tags_csv);
        char *saveptr = NULL;
        for (char *tok = strtok_r(buf, ",", &saveptr); tok;
             tok = strtok_r(NULL, ",", &saveptr)) {
            while (*tok == ' ') tok++;
            char *end = tok + strlen(tok);
            while (end > tok && end[-1] == ' ') *--end = '\0';
            lowercase(tok);
            if (tok[0])
                cJSON_AddItemToArray(tags, cJSON_CreateString(tok));
        }
    }
    extract_tags(content, tags);
    cJSON_AddItemToObject(entry, "tags", tags);

    char *json_line = cJSON_PrintUnformatted(entry);
    int wrote = 0;

    pthread_mutex_lock(&m->lock);
    load_cache(m);
    FILE *f = json_line ? fopen(memory_log_path(m), "a") : NULL;
    if (f) {
        wrote = fputs(json_line, f) != EOF && fputc('\n', f) != EOF;
        if (fclose(f) != 0) wrote = 0;
    }
    /* Cache only what reached the disk */
    if (wrote) {
        cJSON_AddItemToArray(m->cache, entry);
        entry = NULL;
    }
    pthread_mutex_unlock(&m->lock);

    free(json_line);
    cJSON_Delete(entry);
    if (!wrote)
        snprintf(out, out_sz, "Error: cannot append memory log");
    else
        snprintf(out, out_sz, "Remembered (id=%s): %.80s", mid, content);
    return out;
}

/* How many query words appear in the entry (content, category or tags) */
static int match_score(cJSON *entry, char words[][32], int n_words) {
    char hay[TC_BUF_LG];
    size_t off = 0;
    const char *content = j_str(entry, "content");
    const char *category = j_str(entry, "category");
    buf_appendf(hay, sizeof(hay), &off, "%s %s", content ? content : "",
                category ? category : "");
    cJSON *tag;
    cJSON_ArrayForEach(tag, cJSON_GetObjectItem(entry, "tags"))
        if (cJSON_IsString(tag))
            buf_appendf(hay, sizeof(hay), &off, " %s", tag->valuestring);
    lowercase(hay);

    int score = 0;
    for (int i = 0; i < n_words; i++)
        if (strstr(hay, words[i])) score++;
    return score;
}

static void format_entry(cJSON *e, char *out, size_t out_sz, size_t *off) {
    const char *id = j_str(e, "id");
    const char *cat = j_str(e, "category");
    const char *ts = j_str(e, "timestamp");
    const char *content = j_str(e, "content");
    if (!content) content = "";
    buf_appendf(out, out_sz, off, "- [%s] (%s, %.10s, importance %d) %.*s\n",
                id ? id : "?", cat ? cat : "general", ts ? ts : "",
                j_int(e, "importance", 5), utf8_prefix(content, 500), content);
}

/* Most relevant first (words matched, then recency); "" when nothing */
const char *memory_search(memory_t *m, const char *query, int n,
                          char *out, size_t out_sz) {
    if (n <= 0) n = 20;
    if (n > 50) n = 50;

    /* Query words, lowercased */
    char words[8][32];
    int n_words = 0;
    for (const char *p = query ? query : ""; *p && n_words < 8; ) {
        while (*p && !is_word_char(*p)) p++;
        int len = 0;
        while (is_word_char(p[len])) len++;
        if (len >= 2) {
            snprintf(words[n_words], sizeof(words[0]), "%.*s", len < 31 ? len : 31, p);
            lowercase(words[n_words++]);
        }
        p += len;
    }

    size_t off = 0;
    out[0] = '\0';

    pthread_mutex_lock(&m->lock);
    load_cache(m);
    int total = cJSON_GetArraySize(m->cache);
    cJSON *top[50];
    int score[50], found = 0;

    for (int i = total - 1; i >= 0; i--) {
        cJSON *entry = cJSON_GetArrayItem(m->cache, i);
        int s = n_words ? match_score(entry, words, n_words) : 1;
        if (s == 0) continue;
        /* Insert into the top-n list; ties keep recency order */
        int pos = found;
        while (pos > 0 && score[pos - 1] < s) pos--;
        if (pos >= n) {
            if (!n_words) break;   /* recent-only: list is full */
            continue;
        }
        int last = found < n ? found : n - 1;
        memmove(&top[pos + 1], &top[pos], (size_t)(last - pos) * sizeof(top[0]));
        memmove(&score[pos + 1], &score[pos], (size_t)(last - pos) * sizeof(score[0]));
        top[pos] = entry;
        score[pos] = s;
        if (found < n) found++;
    }
    for (int i = 0; i < found; i++)
        format_entry(top[i], out, out_sz, &off);
    pthread_mutex_unlock(&m->lock);

    return out;
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

const char *facts_set(memory_t *m, const char *key, const char *value,
                      char *out, size_t out_sz) {
    if (!key || !key[0] || !value) {
        snprintf(out, out_sz, "Error: key and value are required");
        return out;
    }

    pthread_mutex_lock(&m->lock);
    cJSON *facts = json_load(facts_path(m), 0);
    int rc = -1;
    if (facts) {
        cJSON_DeleteItemFromObject(facts, key);
        cJSON_AddStringToObject(facts, key, value);
        rc = json_save_atomic(facts_path(m), facts, 1);
        cJSON_Delete(facts);
    }
    pthread_mutex_unlock(&m->lock);

    if (rc != 0)
        snprintf(out, out_sz, "Error: cannot save fact");
    else
        snprintf(out, out_sz, "Fact saved: %s = %s", key, value);
    return out;
}

void facts_clear(memory_t *m) {
    pthread_mutex_lock(&m->lock);
    unlink(facts_path(m));
    pthread_mutex_unlock(&m->lock);
}

/* Empty key: every fact, one "- key: value" line each ("" when none) */
const char *facts_get(memory_t *m, const char *key,
                      char *out, size_t out_sz) {
    pthread_mutex_lock(&m->lock);
    cJSON *facts = json_load(facts_path(m), 0);
    pthread_mutex_unlock(&m->lock);

    out[0] = '\0';
    if (!key || !key[0]) {
        size_t off = 0;
        cJSON *f;
        cJSON_ArrayForEach(f, facts)
            if (cJSON_IsString(f))
                buf_appendf(out, out_sz, &off, "- %s: %s\n", f->string, f->valuestring);
    } else {
        const char *val = j_str(facts, key);
        snprintf(out, out_sz, "%s", val ? val : "(not found)");
    }

    cJSON_Delete(facts);
    return out;
}
