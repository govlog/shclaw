/*
 * session.c — session/thread log, one JSON file per session
 */

#include "../include/tc.h"

static const char *status_str(session_status_t s) {
    switch (s) {
    case SESS_ACTIVE:  return "active";
    case SESS_CLOSED:  return "closed";
    case SESS_FAILED:  return "failed";
    }
    return "unknown";
}

static const char *msgtype_str(msg_type_t t) {
    switch (t) {
    case MSG_TEXT:        return "text";
    case MSG_THINKING:    return "thinking";
    case MSG_TOOL_CALL:   return "tool_call";
    case MSG_TOOL_RESULT: return "tool_result";
    case MSG_DELEGATION:  return "delegation";
    }
    return "text";
}

static char *session_file(session_store_t *s, const char *sid) {
    static __thread char path[4200];
    snprintf(path, sizeof(path), "%s/%s.json", s->sessions_dir, sid);
    return path;
}

static cJSON *load_session(session_store_t *s, const char *sid) {
    if (!sid || !sid[0]) return NULL;
    char *data = file_slurp(session_file(s, sid), NULL);
    if (!data) return NULL;
    cJSON *obj = cJSON_Parse(data);
    free(data);
    return obj;
}

static void set_string(cJSON *obj, const char *key, const char *value) {
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddStringToObject(obj, key, value);
}

void session_store_init(session_store_t *s, const char *dir) {
    snprintf(s->sessions_dir, sizeof(s->sessions_dir), "%s", dir);
    mkdirs(dir);
    pthread_mutex_init(&s->lock, NULL);
}

const char *session_create(session_store_t *s, const char *type,
                           const char *title, const char *initiator, char sid[12]) {
    char now[32];
    uuid_short(sid, 10);
    now_iso(now, sizeof(now));

    cJSON *session = cJSON_CreateObject();
    cJSON_AddStringToObject(session, "id", sid);
    cJSON_AddStringToObject(session, "trigger_type", type);
    cJSON_AddStringToObject(session, "title", title);
    cJSON_AddStringToObject(session, "initiator", initiator ? initiator : "system");
    cJSON_AddStringToObject(session, "created_at", now);
    cJSON_AddStringToObject(session, "updated_at", now);
    cJSON_AddStringToObject(session, "status", "active");
    cJSON_AddItemToObject(session, "agents_involved", cJSON_CreateArray());
    cJSON_AddItemToObject(session, "messages", cJSON_CreateArray());

    pthread_mutex_lock(&s->lock);
    int rc = json_save_atomic(session_file(s, sid), session, 0);
    pthread_mutex_unlock(&s->lock);
    cJSON_Delete(session);

    if (rc != 0) {
        log_error("Session [%s] cannot be saved", sid);
        return NULL;
    }
    log_info("Session [%s] %s", sid, title);
    return sid;
}

int session_add_message(session_store_t *s, const char *sid,
                        const char *sender, const char *recipient,
                        const char *content, msg_type_t msg_type) {
    char mid[10], now[32];
    uuid_short(mid, 8);
    now_iso(now, sizeof(now));

    pthread_mutex_lock(&s->lock);
    cJSON *session = load_session(s, sid);
    if (!session) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "id", mid);
    cJSON_AddStringToObject(msg, "sender", sender);
    cJSON_AddStringToObject(msg, "recipient", recipient ? recipient : "");
    cJSON_AddStringToObject(msg, "content", content);
    cJSON_AddStringToObject(msg, "type", msgtype_str(msg_type));
    cJSON_AddStringToObject(msg, "timestamp", now);

    cJSON_AddItemToArray(cJSON_GetObjectItem(session, "messages"), msg);
    set_string(session, "updated_at", now);

    /* Track agents_involved */
    static const char *ignored[] = {"owner", "system", "all", NULL};
    cJSON *agents = cJSON_GetObjectItem(session, "agents_involved");
    const char *names[] = {sender, recipient};
    for (int n = 0; n < 2; n++) {
        if (!names[n] || !names[n][0]) continue;
        int skip = 0;
        for (int j = 0; ignored[j] && !skip; j++)
            skip = strcmp(names[n], ignored[j]) == 0;
        cJSON *item;
        cJSON_ArrayForEach(item, agents) {
            const char *iv = cJSON_GetStringValue(item);
            if (iv && strcmp(iv, names[n]) == 0) { skip = 1; break; }
        }
        if (!skip)
            cJSON_AddItemToArray(agents, cJSON_CreateString(names[n]));
    }

    int rc = json_save_atomic(session_file(s, sid), session, 0);
    cJSON_Delete(session);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int session_set_status(session_store_t *s, const char *sid, session_status_t status) {
    char now[32];
    now_iso(now, sizeof(now));

    pthread_mutex_lock(&s->lock);
    cJSON *session = load_session(s, sid);
    if (!session) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    set_string(session, "status", status_str(status));
    set_string(session, "updated_at", now);
    int rc = json_save_atomic(session_file(s, sid), session, 0);
    cJSON_Delete(session);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

/* ── Owner conversation ─────────────────────────────────── */

void chat_init(chat_log_t *c, int idle_s) {
    memset(c, 0, sizeof(*c));
    c->idle = idle_s;
    pthread_mutex_init(&c->lock, NULL);
}

/* Caller holds the lock */
static void chat_expire(chat_log_t *c) {
    if (c->n && time(NULL) - c->last >= c->idle) {
        c->n = c->next = 0;
        c->id++;
    }
}

void chat_add(chat_log_t *c, const char *agent, const char *who, const char *text) {
    if (!c || !text || !text[0]) return;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);

    pthread_mutex_lock(&c->lock);
    chat_expire(c);
    c->last = t;
    c->lines[c->next].seq = ++c->seq;
    snprintf(c->lines[c->next].agent, sizeof(c->lines[0].agent), "%s", agent);
    char *line = c->lines[c->next].text;
    int max = TC_CHAT_LINE - 1;
    int off = snprintf(line, TC_CHAT_LINE, "%02d:%02d %s: ", tm.tm_hour, tm.tm_min, who);
    if (off < 0 || off > max) off = max;
    int keep = utf8_prefix(text, max - off);
    memcpy(line + off, text, (size_t)keep);
    line[off + keep] = '\0';
    for (char *p = line; *p; p++)   /* one line per entry */
        if (*p == '\n' || *p == '\r') *p = ' ';
    c->next = (c->next + 1) % TC_CHAT_LINES;
    if (c->n < TC_CHAT_LINES) c->n++;
    pthread_mutex_unlock(&c->lock);
}

unsigned chat_id(chat_log_t *c) {
    if (!c) return 0;
    pthread_mutex_lock(&c->lock);
    chat_expire(c);
    unsigned id = c->id;
    pthread_mutex_unlock(&c->lock);
    return id;
}

unsigned chat_render(chat_log_t *c, unsigned after, const char *skip, char *out, size_t sz) {
    size_t off = 0;
    out[0] = '\0';
    if (!c) return 0;
    pthread_mutex_lock(&c->lock);
    for (int i = 0; i < c->n; i++) {
        int k = (c->next - c->n + i + TC_CHAT_LINES) % TC_CHAT_LINES;
        if (c->lines[k].seq > after && !(skip && !strcmp(c->lines[k].agent, skip)))
            buf_appendf(out, sz, &off, "%s\n", c->lines[k].text);
    }
    unsigned seq = c->seq;
    pthread_mutex_unlock(&c->lock);
    return seq;
}
