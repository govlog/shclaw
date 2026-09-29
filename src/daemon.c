/*
 * daemon.c — event loop, config loading, agent lifecycle
 */

#include "../include/tc.h"
#include <dirent.h>

static void load_providers(daemon_t *d, ini_t *cfg) {
    d->n_providers = 0;
    /* Scan for [provider:*] and [provider.*] sections */
    for (int i = 0; i < cfg->count && d->n_providers < 8; i++) {
        const char *section = cfg->entries[i].section;
        if (strncmp(section, "provider:", 9) != 0 &&
            strncmp(section, "provider.", 9) != 0)
            continue;
        const char *pname = section + 9;
        if (!pname[0]) continue;

        int exists = 0;
        for (int j = 0; j < d->n_providers; j++)
            if (strcmp(d->providers[j].provider_name, pname) == 0) { exists = 1; break; }
        if (exists) continue;

        provider_ref_t *p = &d->providers[d->n_providers++];
        memset(p, 0, sizeof(*p));
        const char *type = ini_get(cfg, section, "type");
        const char *key = ini_get(cfg, section, "api_key");
        const char *base = ini_get(cfg, section, "base_url");
        snprintf(p->provider_name, sizeof(p->provider_name), "%s", pname);
        /* Anything but Anthropic (Ollama, vLLM...) speaks the OpenAI protocol */
        snprintf(p->provider_type, sizeof(p->provider_type), "%s",
                 strcmp(type ? type : pname, "anthropic") == 0 ? "anthropic" : "openai");
        snprintf(p->api_key, sizeof(p->api_key), "%s", key ? key : "");
        snprintf(p->base_url, sizeof(p->base_url), "%s", base ? base : "");
        p->max_tokens = ini_get_int(cfg, section, "max_tokens", 0);
        p->timeout = ini_get_int(cfg, section, "timeout", 0);
    }
    log_info("Providers: %d loaded", d->n_providers);
}

static void load_tiers(daemon_t *d, ini_t *cfg) {
    d->n_tiers = 0;
    for (int i = 0; i < cfg->count && d->n_tiers < 8; i++) {
        if (strcmp(cfg->entries[i].section, "tiers") != 0) continue;
        int idx = d->n_tiers++;
        snprintf(d->tiers[idx].tier, sizeof(d->tiers[idx].tier), "%s", cfg->entries[i].key);
        snprintf(d->tiers[idx].model_ref, sizeof(d->tiers[idx].model_ref), "%s",
                 cfg->entries[i].value);
    }
}

int daemon_resolve_provider(daemon_t *d, const char *model_ref, provider_ref_t *out) {
    /* A tier name maps to provider/model (one level, no recursion) */
    for (int i = 0; i < d->n_tiers; i++) {
        if (strcmp(d->tiers[i].tier, model_ref) == 0) {
            model_ref = d->tiers[i].model_ref;
            break;
        }
    }

    /* "provider/model", or a bare model for the first provider with a key */
    const char *slash = strchr(model_ref, '/');
    size_t plen = slash ? (size_t)(slash - model_ref) : 0;
    for (int i = 0; i < d->n_providers; i++) {
        provider_ref_t *p = &d->providers[i];
        int match = slash ? strlen(p->provider_name) == plen &&
                            strncmp(p->provider_name, model_ref, plen) == 0
                          : p->api_key[0] != '\0';
        if (match) {
            *out = *p;
            snprintf(out->model, sizeof(out->model), "%s", slash ? slash + 1 : model_ref);
            return 0;
        }
    }
    return -1;
}

static void generate_irc_secret(daemon_t *d) {
    unsigned char rand_bytes[16];
    memset(rand_bytes, 0, sizeof(rand_bytes));
    FILE *f = fopen("/dev/urandom", "r");
    size_t rd = 0;
    if (f) {
        rd = fread(rand_bytes, 1, sizeof(rand_bytes), f);
        fclose(f);
    }
    if (rd != sizeof(rand_bytes)) {
        uint64_t seed = (uint64_t)time(NULL) ^ (uint64_t)getpid() ^ (uintptr_t)d;
        for (int i = 0; i < 16; i++) {
            seed = seed * 6364136223846793005ULL + 1;
            rand_bytes[i] = (unsigned char)(seed >> 32);
        }
    }

    /* Channel: #tb-XXXXXX, unless configured */
    if (!d->irc.channel[0])
        snprintf(d->irc.channel, sizeof(d->irc.channel),
                 "#tb-%02x%02x%02x", rand_bytes[0], rand_bytes[1], rand_bytes[2]);

    /* Key: 12 random alphanumeric, unless configured */
    if (!d->irc.channel_key[0]) {
        static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        for (int i = 0; i < 12; i++)
            d->irc.channel_key[i] = charset[rand_bytes[3 + i] % 36];
        d->irc.channel_key[12] = '\0';
    }

    /* For `shclaw irc-info`; the key never goes to the log */
    char path[4200], content[512];
    snprintf(path, sizeof(path), "%s/irc.secret", d->data_dir);
    snprintf(content, sizeof(content),
             "channel=%s\nkey=%s\nhost=%s\nport=%d\nnick=%s\n",
             d->irc.channel, d->irc.channel_key,
             d->irc_host, d->irc_port, d->irc.base_nick);
    atomic_write(path, content, strlen(content), 0600);
    log_info("IRC: channel=%s (key in %s)", d->irc.channel, path);
}

static void load_agent(daemon_t *d, const char *ini_path) {
    if (d->n_agents >= TC_MAX_AGENTS) {
        log_error("Too many agents (max %d), skipping %s", TC_MAX_AGENTS, ini_path);
        return;
    }
    ini_t *acfg = ini_load(ini_path);
    if (!acfg) { log_error("Cannot load agent config: %s", ini_path); return; }

    const char *name = ini_get(acfg, "agent", "name");
    int dup = 0;
    for (int i = 0; name && i < d->n_agents; i++)
        dup |= strcasecmp(d->agents[i].name, name) == 0;
    /* The name becomes a path, an IRC mention and a message recipient */
    if (!name || !tool_name_valid(name) || strlen(name) >= sizeof(d->agents[0].name) ||
        !strcasecmp(name, "all") || !strcasecmp(name, "owner") || dup) {
        log_error("Agent config %s: missing, invalid or duplicate [agent] name", ini_path);
        ini_free(acfg);
        return;
    }

    agent_ctx_t *a = &d->agents[d->n_agents];
    memset(a, 0, sizeof(*a));
    snprintf(a->name, sizeof(a->name), "%s", name);

    /* Resolve tier/model → provider */
    const char *tier = ini_get(acfg, "agent", "tier");
    const char *model = ini_get(acfg, "agent", "model");
    const char *model_ref = tier ? tier : (model ? model : "standard");

    if (daemon_resolve_provider(d, model_ref, &a->provider) != 0) {
        log_error("[%s] Cannot resolve model '%s'", name, model_ref);
        ini_free(acfg);
        return;
    }

    a->max_turns = ini_get_int(acfg, "agent", "max_turns", TC_MAX_TURNS);
    a->history_budget = ini_get_int(acfg, "agent", "history_budget", 0);
    a->is_hub = ini_get_bool(acfg, "agent", "hub", 0);
    a->is_builder = ini_get_bool(acfg, "agent", "builder", 0);

    const char *spec = ini_get(acfg, "agent", "specialty");
    if (spec) snprintf(a->specialty, sizeof(a->specialty), "%s", spec);

    const char *pers = ini_get(acfg, "agent", "personality");
    if (pers) snprintf(a->personality, sizeof(a->personality), "%s", pers);

    const char *prompt = ini_get(acfg, "agent", "system_prompt_extra");
    if (prompt) snprintf(a->system_prompt_extra, sizeof(a->system_prompt_extra), "%s", prompt);

    /* Objectives, in file order, whatever their keys */
    for (int i = 0; i < acfg->count && a->n_objectives < 16; i++)
        if (strcmp(acfg->entries[i].section, "objectives") == 0)
            snprintf(a->objectives[a->n_objectives++], sizeof(a->objectives[0]),
                     "%s", acfg->entries[i].value);

    /* Init subsystems */
    char mem_dir[4200], sched_path[4200];
    snprintf(mem_dir, sizeof(mem_dir), "%s/%s/memory", d->data_dir, name);
    snprintf(sched_path, sizeof(sched_path), "%s/%s/scheduler.json", d->data_dir, name);
    memory_init(&a->memory, mem_dir);
    scheduler_init(&a->scheduler, sched_path);
    a->messenger = &d->messenger;
    a->sessions = &d->sessions;
    a->plugins = &d->plugins;
    a->irc = &d->irc;
    a->data_dir = d->data_dir;
    a->peers = d->agents;
    a->last_session_time = -TC_SESSION_GAP;   /* idle right away, even at boot */

    messenger_register(&d->messenger, name);

    d->n_agents++;
    log_info("Agent: %s (model: %s/%s, hub=%d, builder=%d)",
             name, a->provider.provider_name, a->provider.model,
             a->is_hub, a->is_builder);

    ini_free(acfg);
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Agent files (etc/agents, *.ini) in name order: the default hub is stable */
static void load_agents(daemon_t *d) {
    DIR *adir = opendir("etc/agents");
    if (!adir) return;
    char *names[64];
    int n = 0;
    struct dirent *de;
    while ((de = readdir(adir)) && n < 64) {
        size_t len = strlen(de->d_name);
        if (len > 4 && strcmp(de->d_name + len - 4, ".ini") == 0)
            if ((names[n] = strdup(de->d_name))) n++;
    }
    closedir(adir);
    qsort(names, (size_t)n, sizeof(names[0]), cmp_str);
    for (int i = 0; i < n; i++) {
        char apath[4200];
        snprintf(apath, sizeof(apath), "etc/agents/%s", names[i]);
        load_agent(d, apath);
        free(names[i]);
    }
    for (int i = 0; i < d->n_agents; i++)
        d->agents[i].n_peers = d->n_agents;
}

/* ── Sessions ───────────────────────────────────────────── */

typedef struct {
    agent_ctx_t *agent;
    trigger_type_t type;
    char *data;
    char sid[12];
    session_store_t *sessions;
} session_worker_t;

static void *session_worker(void *arg) {
    session_worker_t *w = arg;
    int outcome = agent_run_session(w->agent, w->type, w->data, w->sid);
    if (w->sid[0])
        session_set_status(w->sessions, w->sid,
                           outcome == SESSION_COMPLETED ? SESS_CLOSED : SESS_FAILED);
    __atomic_store_n(&w->agent->last_session_time, (int)(now_ms() / 1000), __ATOMIC_RELEASE);
    __atomic_store_n(&w->agent->busy, 0, __ATOMIC_RELEASE);
    free(w->data);
    free(w);
    return NULL;
}

static int agent_idle(agent_ctx_t *a, int now_s) {
    return !__atomic_load_n(&a->busy, __ATOMIC_ACQUIRE) &&
           now_s - __atomic_load_n(&a->last_session_time, __ATOMIC_ACQUIRE) >= TC_SESSION_GAP;
}

static void start_session(daemon_t *d, agent_ctx_t *a, trigger_type_t type,
                          const char *data, const char *initiator) {
    static const char *kinds[] = {
        [TRIG_IRC] = "irc", [TRIG_SCHEDULE] = "schedule", [TRIG_AGENT_MSG] = "agent_message",
    };
    session_worker_t *w = calloc(1, sizeof(*w));
    if (!w || !(w->data = strdup(data))) {
        free(w);
        log_error("[%s] out of memory, trigger dropped", a->name);
        return;
    }
    w->agent = a;
    w->type = type;
    w->sessions = &d->sessions;

    char title[128];
    snprintf(title, sizeof(title), "%s from %s for %s", kinds[type], initiator, a->name);
    if (!session_create(&d->sessions, kinds[type], title, initiator, w->sid))
        w->sid[0] = '\0';
    if (type == TRIG_IRC)
        session_add_message(&d->sessions, w->sid, "owner", a->name, data, MSG_TEXT);

    __atomic_store_n(&a->busy, 1, __ATOMIC_RELEASE);
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512 * 1024); /* 512KB — agent loop uses large buffers */
    if (pthread_create(&tid, &attr, session_worker, w) != 0) {
        log_error("[%s] cannot start a session thread", a->name);
        __atomic_store_n(&a->busy, 0, __ATOMIC_RELEASE);
        if (w->sid[0]) session_set_status(&d->sessions, w->sid, SESS_FAILED);
        free(w->data);
        free(w);
    }
    pthread_attr_destroy(&attr);
}

/* Owner messages wait here until their agent is free (main thread only) */
typedef struct {
    agent_ctx_t *agent;
    char *text;
} queued_trigger_t;

static queued_trigger_t trigger_queue[64];
static int trigger_queue_len = 0;

static void enqueue_trigger(agent_ctx_t *a, const char *text) {
    char *copy = strdup(text ? text : "");
    if (!copy || trigger_queue_len >= 64) {
        log_warn("[%s] trigger queue full, message dropped", a->name);
        free(copy);
        return;
    }
    trigger_queue[trigger_queue_len].agent = a;
    trigger_queue[trigger_queue_len].text = copy;
    trigger_queue_len++;
}

/* IRC / TUI / CLI message for an agent (or "all") */
static void on_irc_trigger(const char *agent, const char *from,
                           const char *text, void *ctx) {
    daemon_t *d = ctx;
    (void)from;
    int found = 0;
    for (int i = 0; i < d->n_agents; i++) {
        if (strcasecmp(agent, "all") == 0 || strcasecmp(agent, d->agents[i].name) == 0) {
            enqueue_trigger(&d->agents[i], text);
            found = 1;
        }
    }
    if (!found)
        log_warn("Message for unknown agent '%s' dropped", agent);
}

static int queued_for(agent_ctx_t *a) {
    for (int i = 0; i < trigger_queue_len; i++)
        if (trigger_queue[i].agent == a) return 1;
    return 0;
}

/* Start sessions for idle agents: owner messages first (in order), then
 * inter-agent mail, then due tasks. Mail and tasks stay on disk until
 * their agent is free, so nothing is lost or truncated while it works. */
static void dispatch(daemon_t *d, int check_schedule) {
    int now_s = (int)(now_ms() / 1000);

    for (int i = 0; i < trigger_queue_len; ) {
        queued_trigger_t *t = &trigger_queue[i];
        if (!agent_idle(t->agent, now_s)) { i++; continue; }
        start_session(d, t->agent, TRIG_IRC, t->text, "owner");
        free(t->text);
        trigger_queue_len--;
        memmove(t, t + 1, (size_t)(trigger_queue_len - i) * sizeof(*t));
    }

    for (int i = 0; i < d->n_agents; i++) {
        agent_ctx_t *a = &d->agents[i];
        if (!agent_idle(a, now_s) || queued_for(a)) continue;

        cJSON *items = NULL;
        if (messenger_receive(&d->messenger, a->name, &items) > 0) {
            char *json = cJSON_PrintUnformatted(items);
            const char *from = j_str(cJSON_GetArrayItem(items, 0), "from");
            if (json) start_session(d, a, TRIG_AGENT_MSG, json, from ? from : "unknown");
            free(json);
        } else if (check_schedule) {
            cJSON_Delete(items);
            items = NULL;
            if (sched_get_due(&a->scheduler, &items) > 0) {
                char *json = cJSON_PrintUnformatted(items);
                if (json) start_session(d, a, TRIG_SCHEDULE, json, "scheduler");
                free(json);

                cJSON *ids = cJSON_CreateArray();
                cJSON *task;
                cJSON_ArrayForEach(task, items) {
                    const char *tid = j_str(task, "id");
                    if (tid) cJSON_AddItemToArray(ids, cJSON_CreateString(tid));
                }
                sched_mark_done(&a->scheduler, ids);
                cJSON_Delete(ids);
            }
        }
        cJSON_Delete(items);
    }
}

/* ── IRC link ───────────────────────────────────────────── */

/* A server that keeps dropping us (throttle, ban) gets slower retries */
static void irc_lost(daemon_t *d, int64_t now) {
    log_warn("IRC: connection lost");
    irc_disconnect(&d->irc);
    d->irc_backoff = d->irc_backoff ? d->irc_backoff * 2 : 5;
    if (d->irc_backoff > 300) d->irc_backoff = 300;
    d->irc_retry_at = now + (int64_t)d->irc_backoff * 1000;
}

/* Reconnect with backoff, without ever blocking the loop on a sleep */
static void irc_maintain(daemon_t *d, int64_t now) {
    if (d->irc.fd >= 0) {
        if (d->irc.registered)   /* only a working link resets the backoff */
            d->irc_backoff = 0;
        if (irc_keepalive(&d->irc) < 0)
            irc_lost(d, now);
        return;
    }
    if (now < d->irc_retry_at) return;
    if (irc_connect(&d->irc, d->irc_host, d->irc_port) == 0)
        return;
    d->irc_backoff = d->irc_backoff ? d->irc_backoff * 2 : 30;
    if (d->irc_backoff > 300) d->irc_backoff = 300;
    d->irc_retry_at = now_ms() + (int64_t)d->irc_backoff * 1000;
    log_warn("IRC: next attempt in %ds", d->irc_backoff);
}

/* ── Control socket ─────────────────────────────────────── */

static void send_status(daemon_t *d, int fd) {
    cJSON *st = cJSON_CreateObject();
    cJSON *agents = cJSON_AddArrayToObject(st, "agents");
    for (int i = 0; i < d->n_agents; i++) {
        agent_ctx_t *a = &d->agents[i];
        char model[128];
        snprintf(model, sizeof(model), "%s/%s", a->provider.provider_name, a->provider.model);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", a->name);
        cJSON_AddStringToObject(o, "model", model);
        cJSON_AddBoolToObject(o, "busy", __atomic_load_n(&a->busy, __ATOMIC_ACQUIRE));
        cJSON_AddItemToArray(agents, o);
    }
    cJSON_AddStringToObject(st, "irc", !d->irc.enabled ? "disabled" :
                                       d->irc.fd >= 0 ? "connected" : "disconnected");
    cJSON_AddNumberToObject(st, "queued", trigger_queue_len);
    char *json = cJSON_PrintUnformatted(st);
    cJSON_Delete(st);
    if (json) sock_send(fd, SOCK_EVT_STATUS, json, (uint32_t)strlen(json));
    free(json);
}

/* "agent\0text" from the CLI or the TUI */
static void route_msg(daemon_t *d, const char *data, int len, const char *from) {
    const char *sep = len > 0 ? memchr(data, '\0', (size_t)len) : NULL;
    if (sep)
        on_irc_trigger(data, from, sep + 1, d);
}

static void remove_tui_client(daemon_t *d, int i) {
    log_info("TUI: client detached (fd=%d)", d->tui_clients[i]);
    close(d->tui_clients[i]);
    d->tui_clients[i] = d->tui_clients[--d->n_tui_clients];
}

static void handle_new_client(daemon_t *d) {
    int client = accept(d->sock_fd, NULL, NULL);
    if (client < 0) return;
    fcntl(client, F_SETFD, FD_CLOEXEC);
    /* A silent or stuck client must never block the daemon */
    struct timeval tv = { .tv_sec = 2 };
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    uint32_t type;
    char data[TC_BUF_LG];
    int len = sock_read_cmd(client, &type, data, sizeof(data));
    if (len >= 0) {
        switch (type) {
        case SOCK_CMD_STOP:
            d->shutdown = 1;
            break;
        case SOCK_CMD_STATUS:
            send_status(d, client);
            break;
        case SOCK_CMD_IRC_INFO: {
            char info[512];
            int n = snprintf(info, sizeof(info),
                             "channel=%s\nkey=%s\nhost=%s\nport=%d\nnick=%s\n",
                             d->irc.channel, d->irc.channel_key,
                             d->irc_host, d->irc_port, d->irc.nick);
            sock_send(client, SOCK_EVT_IRC_INFO, info, (uint32_t)n);
            break;
        }
        case SOCK_CMD_ATTACH:
            pthread_mutex_lock(&d->tui_lock);
            if (d->n_tui_clients < 8) {
                d->tui_clients[d->n_tui_clients++] = client;
                client = -1;
                log_info("TUI: client attached (total=%d)", d->n_tui_clients);
            }
            pthread_mutex_unlock(&d->tui_lock);
            break;
        case SOCK_CMD_MSG:
            route_msg(d, data, len, "socket");
            break;
        }
    }
    if (client >= 0)
        close(client);
}

/* Attached TUI clients may send MSG or DETACH */
static void handle_tui_client(daemon_t *d, int fd, short revents) {
    pthread_mutex_lock(&d->tui_lock);
    for (int i = 0; i < d->n_tui_clients; i++) {
        if (d->tui_clients[i] != fd) continue;
        uint32_t type;
        char data[TC_BUF_LG];
        int len = (revents & POLLIN) ? sock_read_cmd(fd, &type, data, sizeof(data)) : -1;
        if (len < 0 || type == SOCK_CMD_DETACH)
            remove_tui_client(d, i);
        else if (type == SOCK_CMD_MSG)
            route_msg(d, data, len, "tui");
        break;
    }
    pthread_mutex_unlock(&d->tui_lock);
}

int daemon_run(daemon_t *d, ini_t *cfg) {
    d->irc.fd = -1;
    d->sock_fd = -1;

    /* Load global config */
    const char *data_dir = ini_get(cfg, "daemon", "data_dir");
    snprintf(d->data_dir, sizeof(d->data_dir), "%s", data_dir ? data_dir : "./data");

    const char *log_dir = ini_get(cfg, "daemon", "log_dir");
    snprintf(d->log_dir, sizeof(d->log_dir), "%s", log_dir ? log_dir : "./logs");
    log_init(d->log_dir);

    log_info("==========================================");
    log_info("  SHCLAW v%s", TC_VERSION);
    log_info("==========================================");

    /* Sessions, memories and the control socket live here: private */
    mkdirs_for(d->data_dir);
    mkdir(d->data_dir, 0700);
    if (access(d->data_dir, W_OK) != 0) {
        log_error("Cannot write to %s: %s (in Docker, run with --user)",
                  d->data_dir, strerror(errno));
        return 1;
    }

    load_providers(d, cfg);
    load_tiers(d, cfg);

    char sub[4200];
    snprintf(sub, sizeof(sub), "%s/sessions", d->data_dir);
    session_store_init(&d->sessions, sub);
    snprintf(sub, sizeof(sub), "%s/messages", d->data_dir);
    messenger_init(&d->messenger, sub);
    messenger_register(&d->messenger, "owner");

    plugin_init(&d->plugins, "./plugins");
    ca_init(d->data_dir);

    /* IRC is optional: enabled by [irc] host (or server) */
    const char *irc_host = ini_get(cfg, "irc", "host");
    if (!irc_host) irc_host = ini_get(cfg, "irc", "server");
    d->irc.enabled = irc_host && irc_host[0];
    snprintf(d->irc_host, sizeof(d->irc_host), "%s", d->irc.enabled ? irc_host : "");
    d->irc_port = ini_get_int(cfg, "irc", "port", 6697);
    const char *v = ini_get(cfg, "irc", "nick");
    snprintf(d->irc.base_nick, sizeof(d->irc.base_nick), "%s", v ? v : "shclaw");
    v = ini_get(cfg, "irc", "owner");
    snprintf(d->irc.owner, sizeof(d->irc.owner), "%s", v ? v : "");
    v = ini_get(cfg, "irc", "channel");
    snprintf(d->irc.channel, sizeof(d->irc.channel), "%s", v ? v : "");
    v = ini_get(cfg, "irc", "channel_key");
    snprintf(d->irc.channel_key, sizeof(d->irc.channel_key), "%s", v ? v : "");

    load_agents(d);
    if (d->n_agents == 0) {
        log_error("No agents found in etc/agents/");
        return 1;
    }

    /* Hub: the agent with hub = true, else the first one */
    snprintf(d->irc.hub, sizeof(d->irc.hub), "%s", d->agents[0].name);
    for (int i = 0; i < d->n_agents; i++) {
        if (d->agents[i].is_hub) {
            snprintf(d->irc.hub, sizeof(d->irc.hub), "%s", d->agents[i].name);
            break;
        }
    }

    d->irc.n_agents = d->n_agents;
    for (int i = 0; i < d->n_agents; i++)
        memcpy(d->irc.agents[i], d->agents[i].name, sizeof(d->irc.agents[i]));
    d->irc.on_trigger = on_irc_trigger;
    d->irc.ctx = d;
    if (d->irc.enabled)
        generate_irc_secret(d);
    else
        log_info("IRC: disabled (no [irc] host in config)");

    snprintf(d->socket_path, sizeof(d->socket_path), "%s/shclaw.sock", d->data_dir);
    d->sock_fd = sock_server_create(d->socket_path);
    if (d->sock_fd < 0)
        log_error("Socket: cannot listen on %s", d->socket_path);
    pthread_mutex_init(&d->tui_lock, NULL);
    d->n_tui_clients = 0;

    log_info("Agents ready: %d (waiting for triggers)", d->n_agents);

    /* ── Main event loop ── */
    int64_t last_sched = 0, last_plugin = now_ms();
    struct pollfd fds[2 + 8];

    while (!d->shutdown) {
        /* poll() ignores negative fds: a down IRC link or socket is fine */
        int nfds = 0;
        fds[nfds++] = (struct pollfd){ .fd = d->irc.fd, .events = POLLIN };
        fds[nfds++] = (struct pollfd){ .fd = d->sock_fd, .events = POLLIN };
        pthread_mutex_lock(&d->tui_lock);
        for (int i = 0; i < d->n_tui_clients; i++)
            fds[nfds++] = (struct pollfd){ .fd = d->tui_clients[i], .events = POLLIN };
        pthread_mutex_unlock(&d->tui_lock);

        int nready = poll(fds, (nfds_t)nfds, TC_TICK_MS);
        int64_t now = now_ms();

        if (nready > 0) {
            if (fds[0].fd >= 0 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) &&
                irc_poll(&d->irc) < 0)
                irc_lost(d, now);
            if (fds[1].revents & POLLIN)
                handle_new_client(d);
            for (int i = 2; i < nfds; i++)
                if (fds[i].revents)
                    handle_tui_client(d, fds[i].fd, fds[i].revents);
        }
        if (d->irc.enabled)
            irc_maintain(d, now);

        dispatch(d, now - last_sched >= 5000);
        if (now - last_sched >= 5000)
            last_sched = now;

        if (now - last_plugin >= 5000) {
            plugin_scan(&d->plugins);
            last_plugin = now;
        }
    }

    /* Shutdown */
    log_info("Shutting down...");
    irc_disconnect(&d->irc);

    pthread_mutex_lock(&d->tui_lock);
    for (int i = 0; i < d->n_tui_clients; i++) {
        sock_send(d->tui_clients[i], SOCK_EVT_GOODBYE, "shutdown", 8);
        close(d->tui_clients[i]);
    }
    d->n_tui_clients = 0;
    pthread_mutex_unlock(&d->tui_lock);

    for (int i = 0; i < trigger_queue_len; i++)
        free(trigger_queue[i].text);
    trigger_queue_len = 0;

    if (d->sock_fd >= 0) {
        close(d->sock_fd);
        unlink(d->socket_path);
    }
    /* Trust anchors stay allocated: detached sessions may still be in a
     * TLS handshake, and the process exits right after */
    log_info("Goodbye.");
    log_close();
    return 0;
}

void daemon_tui_broadcast(daemon_t *d, const char *agent, const char *text) {
    if (!d || !text || !text[0]) return;

    char line[TC_BUF_LG];
    int n = agent ? snprintf(line, sizeof(line), "%s: %s", agent, text)
                  : snprintf(line, sizeof(line), "%s", text);
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;

    pthread_mutex_lock(&d->tui_lock);
    for (int i = 0; i < d->n_tui_clients; i++)
        if (sock_send(d->tui_clients[i], SOCK_EVT_LINE, line, (uint32_t)n) != 0)
            /* The main loop sees the hangup and closes it: closing here
             * would let the fd number be reused under its poll set. */
            shutdown(d->tui_clients[i], SHUT_RDWR);
    pthread_mutex_unlock(&d->tui_lock);
}
