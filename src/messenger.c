/*
 * messenger.c — file-based inter-agent inbox
 */

#include "../include/tc.h"
#include <dirent.h>

void messenger_init(messenger_t *m, const char *dir) {
    snprintf(m->dir, sizeof(m->dir), "%s", dir);
    mkdirs(dir);
    pthread_mutex_init(&m->lock, NULL);
    m->n_agents = 0;
}

/* "owner" is a valid recipient but has no inbox: it is reached over IRC */
void messenger_register(messenger_t *m, const char *agent_name) {
    if (messenger_resolve(m, agent_name)) return;
    if (m->n_agents >= TC_MAX_AGENTS + 1) return;

    snprintf(m->agents[m->n_agents], sizeof(m->agents[0]), "%s", agent_name);
    m->n_agents++;

    if (strcmp(agent_name, "owner") != 0) {
        char inbox[4200];
        snprintf(inbox, sizeof(inbox), "%s/%s", m->dir, agent_name);
        mkdirs(inbox);
    }
}

const char *messenger_resolve(messenger_t *m, const char *name) {
    if (!name) return NULL;
    if (*name == '@') name++;   /* small models often write "@oracle" */
    for (int i = 0; i < m->n_agents; i++)
        if (strcasecmp(m->agents[i], name) == 0)
            return m->agents[i];
    return NULL;
}

static const char *send_one(messenger_t *m, const char *from, const char *to,
                            const char *content, const char *thread_id, int reply,
                            char *out, size_t out_sz) {
    char mid[10], now[32];
    uuid_short(mid, 8);
    now_iso(now, sizeof(now));

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "id", mid);
    cJSON_AddStringToObject(msg, "from", from);
    cJSON_AddStringToObject(msg, "to", to);
    cJSON_AddStringToObject(msg, "content", content);
    cJSON_AddStringToObject(msg, "thread_id", thread_id ? thread_id : "");
    cJSON_AddStringToObject(msg, "timestamp", now);
    if (reply)
        cJSON_AddTrueToObject(msg, "reply");

    char path[4200];
    snprintf(path, sizeof(path), "%s/%s/%s.json", m->dir, to, mid);

    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    pthread_mutex_lock(&m->lock);
    int rc = json ? atomic_write(path, json, strlen(json), 0600) : -1;
    pthread_mutex_unlock(&m->lock);
    free(json);

    if (rc != 0)
        snprintf(out, out_sz, "Error: cannot deliver the message to %s", to);
    else
        snprintf(out, out_sz, "Message sent to %s.", to);
    return out;
}

const char *messenger_send(messenger_t *m, const char *from, const char *to,
                           const char *content, const char *thread_id, int reply,
                           char *out, size_t out_sz) {
    if (to && strcasecmp(to, "all") == 0) {
        int sent = 0;
        for (int i = 0; i < m->n_agents; i++) {
            if (strcmp(m->agents[i], from) == 0) continue;
            if (strcmp(m->agents[i], "owner") == 0) continue;
            char tmp[256];
            send_one(m, from, m->agents[i], content, thread_id, reply, tmp, sizeof(tmp));
            sent++;
        }
        snprintf(out, out_sz, "Broadcast sent to %d agents.", sent);
        return out;
    }

    const char *name = messenger_resolve(m, to);
    if (!name || strcmp(name, "owner") == 0) {
        size_t off = 0;
        buf_appendf(out, out_sz, &off, "Error: unknown recipient '%s'. Valid:",
                    to ? to : "");
        for (int i = 0; i < m->n_agents; i++)
            buf_appendf(out, out_sz, &off, " %s,", m->agents[i]);
        buf_appendf(out, out_sz, &off, " all");
        return out;
    }
    if (strcmp(name, from) == 0) {
        snprintf(out, out_sz, "Error: you cannot send a message to yourself. "
                 "Reply in text to answer the owner.");
        return out;
    }
    return send_one(m, from, name, content, thread_id, reply, out, out_sz);
}

int messenger_receive(messenger_t *m, const char *agent_name, cJSON **out) {
    char inbox[4200];
    snprintf(inbox, sizeof(inbox), "%s/%s", m->dir, agent_name);

    *out = cJSON_CreateArray();
    int count = 0;

    pthread_mutex_lock(&m->lock);
    DIR *dir = opendir(inbox);
    if (!dir) {
        pthread_mutex_unlock(&m->lock);
        return 0;
    }

    struct dirent *de;
    while ((de = readdir(dir))) {
        if (de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 5, ".json") != 0) continue;

        char path[4200];
        snprintf(path, sizeof(path), "%s/%s", inbox, de->d_name);

        char *data = file_slurp(path, NULL);
        if (data) {
            cJSON *msg = cJSON_Parse(data);
            if (msg) {
                cJSON_AddItemToArray(*out, msg);
                count++;
            }
            free(data);
        }

        /* Consume: delete the file */
        unlink(path);
    }
    closedir(dir);
    pthread_mutex_unlock(&m->lock);

    return count;
}
