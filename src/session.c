/*
 * session.c — session/thread CRUD with JSON file storage
 */

#include "../include/tc.h"

static const char *status_str(session_status_t s) {
    switch (s) {
    case SESS_ACTIVE:  return "active";
    case SESS_CLOSED:  return "closed";
    case SESS_ABORTED: return "aborted";
    case SESS_FAILED:  return "failed";
    }
    return "unknown";
}

static const char *msgtype_str(msg_type_t t) {
    switch (t) {
    case MSG_TEXT:        return "text";
    case MSG_SYSTEM:      return "system";
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

static char *index_file(session_store_t *s) {
    static __thread char path[4200];
    snprintf(path, sizeof(path), "%s/_index.json", s->sessions_dir);
    return path;
}

static cJSON *load_index(session_store_t *s) {
    return json_load_array(index_file(s));
}

static void save_index(session_store_t *s, cJSON *index) {
    json_save_atomic(index_file(s), index, 0);
}

static cJSON *load_session(session_store_t *s, const char *sid) {
    char *data = file_slurp(session_file(s, sid), NULL);
    if (!data) return NULL;
    cJSON *obj = cJSON_Parse(data);
    free(data);
    return obj;
}

static void save_session(session_store_t *s, cJSON *session) {
    const char *sid = j_str(session, "id");
    if (!sid) return;
    json_save_atomic(session_file(s, sid), session, 0);
}

void session_store_init(session_store_t *s, const char *dir) {
    snprintf(s->sessions_dir, sizeof(s->sessions_dir), "%s", dir);
    mkdirs(dir);
    pthread_mutex_init(&s->lock, NULL);
}

cJSON *session_create(session_store_t *s, const char *type,
                      const char *title, const char *initiator) {
    char sid[12], now[32];
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
    save_session(s, session);

    cJSON *idx = load_index(s);
    cJSON *summary = cJSON_CreateObject();
    cJSON_AddStringToObject(summary, "id", sid);
    cJSON_AddStringToObject(summary, "trigger_type", type);
    cJSON_AddStringToObject(summary, "title", title);
    cJSON_AddStringToObject(summary, "initiator", initiator ? initiator : "system");
    cJSON_AddStringToObject(summary, "created_at", now);
    cJSON_AddStringToObject(summary, "updated_at", now);
    cJSON_AddStringToObject(summary, "status", "active");
    cJSON_AddItemToArray(idx, summary);
    save_index(s, idx);
    cJSON_Delete(idx);
    pthread_mutex_unlock(&s->lock);

    log_info("Session [%s] %s", sid, title);
    return session;
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

    cJSON *messages = cJSON_GetObjectItem(session, "messages");
    cJSON_AddItemToArray(messages, msg);
    cJSON_SetValuestring(cJSON_GetObjectItem(session, "updated_at"), now);

    /* Track agents_involved */
    static const char *ignored[] = {"owner", "system", "email", "scheduler", "all", NULL};
    cJSON *agents = cJSON_GetObjectItem(session, "agents_involved");
    const char *names[] = {sender, recipient};
    for (int n = 0; n < 2; n++) {
        if (!names[n] || !names[n][0]) continue;
        int skip = 0;
        for (int j = 0; ignored[j]; j++)
            if (strcmp(names[n], ignored[j]) == 0) { skip = 1; break; }
        if (skip) continue;
        /* Check if already in list */
        int found = 0;
        cJSON *item;
        cJSON_ArrayForEach(item, agents) {
            const char *iv = cJSON_GetStringValue(item);
            if (iv && strcmp(iv, names[n]) == 0) { found = 1; break; }
        }
        if (!found)
            cJSON_AddItemToArray(agents, cJSON_CreateString(names[n]));
    }

    save_session(s, session);

    /* Update index */
    cJSON *idx = load_index(s);
    cJSON *entry;
    cJSON_ArrayForEach(entry, idx) {
        const char *eid = j_str(entry, "id");
        if (eid && strcmp(eid, sid) == 0) {
            cJSON_SetValuestring(cJSON_GetObjectItem(entry, "updated_at"), now);
            break;
        }
    }
    save_index(s, idx);
    cJSON_Delete(idx);

    cJSON_Delete(session);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int session_close(session_store_t *s, const char *sid) {
    return session_set_status(s, sid, SESS_CLOSED);
}

int session_set_status(session_store_t *s, const char *sid, session_status_t status) {
    char now[32];
    now_iso(now, sizeof(now));
    const char *st = status_str(status);

    pthread_mutex_lock(&s->lock);
    cJSON *session = load_session(s, sid);
    if (!session) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }

    cJSON_SetValuestring(cJSON_GetObjectItem(session, "status"), st);
    cJSON_SetValuestring(cJSON_GetObjectItem(session, "updated_at"), now);
    save_session(s, session);

    cJSON *idx = load_index(s);
    cJSON *entry;
    cJSON_ArrayForEach(entry, idx) {
        const char *eid = j_str(entry, "id");
        if (eid && strcmp(eid, sid) == 0) {
            cJSON_SetValuestring(cJSON_GetObjectItem(entry, "status"), st);
            cJSON_SetValuestring(cJSON_GetObjectItem(entry, "updated_at"), now);
            break;
        }
    }
    save_index(s, idx);
    cJSON_Delete(idx);
    cJSON_Delete(session);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

cJSON *session_get(session_store_t *s, const char *sid) {
    pthread_mutex_lock(&s->lock);
    cJSON *session = load_session(s, sid);
    pthread_mutex_unlock(&s->lock);
    return session;
}

cJSON *session_list_recent(session_store_t *s, int n) {
    pthread_mutex_lock(&s->lock);
    cJSON *idx = load_index(s);
    pthread_mutex_unlock(&s->lock);

    /* Return last n entries (already sorted by update time due to append order) */
    int total = cJSON_GetArraySize(idx);
    if (n <= 0) n = 50;

    cJSON *result = cJSON_CreateArray();
    int start = total - n;
    if (start < 0) start = 0;
    for (int i = start; i < total; i++) {
        cJSON *item = cJSON_GetArrayItem(idx, i);
        cJSON_AddItemToArray(result, cJSON_Duplicate(item, 1));
    }

    cJSON_Delete(idx);
    return result;
}
