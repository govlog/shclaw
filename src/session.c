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
