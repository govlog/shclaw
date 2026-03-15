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

void messenger_register(messenger_t *m, const char *agent_name) {
    for (int i = 0; i < m->n_agents; i++)
        if (strcmp(m->agents[i], agent_name) == 0) return;
    if (m->n_agents >= TC_MAX_AGENTS) return;

    snprintf(m->agents[m->n_agents], 32, "%s", agent_name);
    m->n_agents++;

    /* Create inbox dir */
    char inbox[4200];
    snprintf(inbox, sizeof(inbox), "%s/%s", m->dir, agent_name);
    mkdirs(inbox);
}

static const char *send_one(messenger_t *m, const char *from, const char *to,
                            const char *content, const char *thread_id,
                            char *out, size_t out_sz) {
    /* Check agent exists */
    int found = 0;
    for (int i = 0; i < m->n_agents; i++)
        if (strcmp(m->agents[i], to) == 0) { found = 1; break; }
    if (!found) {
        snprintf(out, out_sz, "Error: unknown agent '%s'", to);
        return out;
    }

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

    char path[4200];
    snprintf(path, sizeof(path), "%s/%s/%s.json", m->dir, to, mid);

    char *json = cJSON_PrintUnformatted(msg);
    if (json) {
        pthread_mutex_lock(&m->lock);
        atomic_write(path, json, strlen(json));
        pthread_mutex_unlock(&m->lock);
        free(json);
    }

    cJSON_Delete(msg);
    snprintf(out, out_sz, "Message sent to %s.", to);
    return out;
}

const char *messenger_send(messenger_t *m, const char *from, const char *to,
                           const char *content, const char *thread_id,
                           char *out, size_t out_sz) {
    if (strcmp(to, "all") == 0) {
        int sent = 0;
        for (int i = 0; i < m->n_agents; i++) {
            if (strcmp(m->agents[i], from) == 0) continue;
            if (strcmp(m->agents[i], "owner") == 0) continue;
            char tmp[256];
            send_one(m, from, m->agents[i], content, thread_id, tmp, sizeof(tmp));
            sent++;
        }
        snprintf(out, out_sz, "Broadcast sent to %d agents.", sent);
        return out;
    }

    return send_one(m, from, to, content, thread_id, out, out_sz);
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
