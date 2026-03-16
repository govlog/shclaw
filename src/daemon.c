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
        const char *pname = NULL;
        if (strncmp(section, "provider:", 9) == 0)
            pname = section + 9;
        else if (strncmp(section, "provider.", 9) == 0)
            pname = section + 9;
        if (!pname || !pname[0]) continue;

        /* Check if we already have this provider */
        int exists = 0;
        for (int j = 0; j < d->n_providers; j++)
            if (strcmp(d->providers[j].name, pname) == 0) { exists = 1; break; }
        if (exists) continue;

        int idx = d->n_providers++;
        snprintf(d->providers[idx].name, 32, "%s", pname);
        const char *type = ini_get(cfg, cfg->entries[i].section, "type");
        snprintf(d->providers[idx].type, 16, "%s", type ? type : pname);
        const char *key = ini_get(cfg, cfg->entries[i].section, "api_key");
        snprintf(d->providers[idx].api_key, 256, "%s", key ? key : "");
        const char *base = ini_get(cfg, cfg->entries[i].section, "base_url");
        snprintf(d->providers[idx].base_url, 256, "%s", base ? base : "");
    }
    log_info("Providers: %d loaded", d->n_providers);
}

static void load_tiers(daemon_t *d, ini_t *cfg) {
    d->n_tiers = 0;
    for (int i = 0; i < cfg->count && d->n_tiers < 8; i++) {
        if (strcmp(cfg->entries[i].section, "tiers") != 0) continue;
        int idx = d->n_tiers++;
        snprintf(d->tiers[idx].tier, 32, "%s", cfg->entries[i].key);
        snprintf(d->tiers[idx].model_ref, 96, "%s", cfg->entries[i].value);
    }
}

int daemon_resolve_provider(daemon_t *d, const char *model_ref, provider_ref_t *out) {
    memset(out, 0, sizeof(*out));

    /* Check if it's a tier name first */
    for (int i = 0; i < d->n_tiers; i++) {
        if (strcmp(d->tiers[i].tier, model_ref) == 0) {
            return daemon_resolve_provider(d, d->tiers[i].model_ref, out);
        }
    }

    /* provider/model format */
    const char *slash = strchr(model_ref, '/');
    if (slash) {
        char pname[32];
        int plen = (int)(slash - model_ref);
        if (plen > 31) plen = 31;
        memcpy(pname, model_ref, plen);
        pname[plen] = '\0';

        for (int i = 0; i < d->n_providers; i++) {
            if (strcmp(d->providers[i].name, pname) == 0) {
                snprintf(out->provider_name, 32, "%s", pname);
                snprintf(out->provider_type, 16, "%s", d->providers[i].type);
                snprintf(out->api_key, 256, "%s", d->providers[i].api_key);
                snprintf(out->base_url, 256, "%s", d->providers[i].base_url);
                snprintf(out->model, 64, "%s", slash + 1);
                /* Ollama uses openai-compat API */
                if (strcmp(out->provider_type, "ollama") == 0)
                    snprintf(out->provider_type, 16, "openai");
                return 0;
            }
        }
    }

    /* Bare model name — try first provider with an API key */
    for (int i = 0; i < d->n_providers; i++) {
        if (d->providers[i].api_key[0]) {
            snprintf(out->provider_name, 32, "%s", d->providers[i].name);
            snprintf(out->provider_type, 16, "%s", d->providers[i].type);
            snprintf(out->api_key, 256, "%s", d->providers[i].api_key);
            snprintf(out->base_url, 256, "%s", d->providers[i].base_url);
            snprintf(out->model, 64, "%s", model_ref);
            if (strcmp(out->provider_type, "ollama") == 0)
                snprintf(out->provider_type, 16, "openai");
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

    /* Channel: #tb-XXXXXX */
    if (!d->irc_channel[0])
        snprintf(d->irc.channel, sizeof(d->irc.channel),
                 "#tb-%02x%02x%02x", rand_bytes[0], rand_bytes[1], rand_bytes[2]);
    else
        snprintf(d->irc.channel, sizeof(d->irc.channel), "%s", d->irc_channel);

    /* Key: 12 random alphanumeric */
    if (!d->irc_channel_key[0]) {
        static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        for (int i = 0; i < 12; i++)
            d->irc.channel_key[i] = charset[rand_bytes[3 + i] % 36];
        d->irc.channel_key[12] = '\0';
    } else {
        snprintf(d->irc.channel_key, sizeof(d->irc.channel_key), "%s", d->irc_channel_key);
    }

    /* Write secret file */
    char path[4200], content[512];
    snprintf(path, sizeof(path), "%s/irc.secret", d->data_dir);
    snprintf(content, sizeof(content),
             "channel=%s\nkey=%s\nhost=%s\nport=%d\nnick=%s\n",
             d->irc.channel, d->irc.channel_key,
             d->irc_host, d->irc_port, d->irc.nick);
    atomic_write(path, content, strlen(content));
    log_info("IRC: channel=%s key=%s", d->irc.channel, d->irc.channel_key);
}

static void load_agent(daemon_t *d, const char *ini_path) {
    ini_t *acfg = ini_load(ini_path);
    if (!acfg) { log_error("Cannot load agent config: %s", ini_path); return; }

    if (d->n_agents >= TC_MAX_AGENTS) { ini_free(acfg); return; }

    agent_ctx_t *a = &d->agents[d->n_agents];
    memset(a, 0, sizeof(*a));

    const char *name = ini_get(acfg, "agent", "name");
    if (!name) { log_error("Agent config missing [agent] name: %s", ini_path); ini_free(acfg); return; }
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
    a->is_hub = ini_get_bool(acfg, "agent", "hub", 0);
    a->is_builder = ini_get_bool(acfg, "agent", "builder", 0);

    const char *spec = ini_get(acfg, "agent", "specialty");
    if (spec) snprintf(a->specialty, sizeof(a->specialty), "%s", spec);

    const char *pers = ini_get(acfg, "agent", "personality");
    if (pers) snprintf(a->personality, sizeof(a->personality), "%s", pers);

    const char *prompt = ini_get(acfg, "agent", "system_prompt_extra");
    if (prompt) snprintf(a->system_prompt_extra, sizeof(a->system_prompt_extra), "%s", prompt);

    /* Objectives */
    a->n_objectives = 0;
    for (int i = 0; i < 16; i++) {
        char key[4];
        snprintf(key, sizeof(key), "%d", i);
        const char *obj = ini_get(acfg, "objectives", key);
        if (obj) {
            snprintf(a->objectives[a->n_objectives], 256, "%s", obj);
            a->n_objectives++;
        }
    }

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
    pthread_mutex_init(&a->session_lock, NULL);
    a->last_session_time = 0;

    messenger_register(&d->messenger, name);

    d->n_agents++;
    log_info("Agent: %s (model: %s/%s, hub=%d, builder=%d)",
             name, a->provider.provider_name, a->provider.model,
             a->is_hub, a->is_builder);

    ini_free(acfg);
}

/* Trigger queue */
typedef struct {
    char agent_name[32];
    trigger_type_t type;
    char data[TC_BUF_LG];
    char thread_id[12];
} queued_trigger_t;

static queued_trigger_t trigger_queue[64];
static int trigger_queue_len = 0;
static pthread_mutex_t trigger_mutex = PTHREAD_MUTEX_INITIALIZER;

static void enqueue_trigger(const char *agent, trigger_type_t type,
                            const char *data, const char *thread_id) {
    pthread_mutex_lock(&trigger_mutex);
    if (trigger_queue_len < 64) {
        queued_trigger_t *t = &trigger_queue[trigger_queue_len++];
        snprintf(t->agent_name, 32, "%s", agent);
        t->type = type;
        snprintf(t->data, TC_BUF_LG, "%s", data ? data : "");
        snprintf(t->thread_id, 12, "%s", thread_id ? thread_id : "");
    }
    pthread_mutex_unlock(&trigger_mutex);
}

/* IRC trigger callback */
static void on_irc_trigger(const char *agent, const char *from,
                           const char *text, void *ctx) {
    daemon_t *d = ctx;
    (void)from;

    if (strcmp(agent, "all") == 0) {
        for (int i = 0; i < d->n_agents; i++)
            enqueue_trigger(d->agents[i].name, TRIG_IRC, text, "");
    } else {
        enqueue_trigger(agent, TRIG_IRC, text, "");
    }
}

/* Session worker thread */
typedef struct {
    agent_ctx_t *agent;
    trigger_type_t trig_type;
    char trig_data[TC_BUF_LG];
    char thread_id[12];
    const char **all_agents;
    int n_agents;
    session_store_t *sessions;
} session_worker_t;

static void *session_worker(void *arg) {
    session_worker_t *w = arg;

    pthread_mutex_lock(&w->agent->session_lock);
    int outcome = agent_run_session(w->agent, w->trig_type, w->trig_data,
                                     w->thread_id, w->all_agents, w->n_agents);
    w->agent->last_session_time = now_ms() / 1000;

    if (outcome == SESSION_COMPLETED && w->thread_id[0])
        session_close(w->sessions, w->thread_id);
    else if (outcome == SESSION_ABORTED && w->thread_id[0])
        session_set_status(w->sessions, w->thread_id, SESS_ABORTED);
    else if (outcome == SESSION_FAILED && w->thread_id[0])
        session_set_status(w->sessions, w->thread_id, SESS_FAILED);

    pthread_mutex_unlock(&w->agent->session_lock);
    free(w);
    return NULL;
}

static void dispatch_session(daemon_t *d, agent_ctx_t *agent,
                             trigger_type_t trig_type, const char *trig_data,
                             const char *thread_id) {
    session_worker_t *w = calloc(1, sizeof(session_worker_t));
    w->agent = agent;
    w->trig_type = trig_type;
    snprintf(w->trig_data, TC_BUF_LG, "%s", trig_data ? trig_data : "");
    snprintf(w->thread_id, 12, "%s", thread_id ? thread_id : "");
    w->sessions = &d->sessions;

    /* Build agent name list */
    static const char *agent_names[TC_MAX_AGENTS];
    for (int i = 0; i < d->n_agents; i++)
        agent_names[i] = d->agents[i].name;
    w->all_agents = agent_names;
    w->n_agents = d->n_agents;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512 * 1024); /* 512KB — agent loop uses large buffers */
    pthread_create(&tid, &attr, session_worker, w);
    pthread_attr_destroy(&attr);
}

void daemon_run(daemon_t *d, ini_t *cfg) {
    /* Load global config */
    const char *data_dir = ini_get(cfg, "daemon", "data_dir");
    snprintf(d->data_dir, sizeof(d->data_dir), "%s", data_dir ? data_dir : "./data");

    const char *log_dir = ini_get(cfg, "daemon", "log_dir");
    snprintf(d->log_dir, sizeof(d->log_dir), "%s", log_dir ? log_dir : "./logs");
    log_init(d->log_dir);

    log_info("==========================================");
    log_info("  SHCLAW v%s", TC_VERSION);
    log_info("==========================================");

    mkdirs(d->data_dir);

    /* Load providers + tiers */
    load_providers(d, cfg);
    load_tiers(d, cfg);

    /* Init subsystems */
    char sessions_dir[4200], messages_dir[4200];
    snprintf(sessions_dir, sizeof(sessions_dir), "%s/sessions", d->data_dir);
    snprintf(messages_dir, sizeof(messages_dir), "%s/messages", d->data_dir);
    session_store_init(&d->sessions, sessions_dir);
    messenger_init(&d->messenger, messages_dir);
    messenger_register(&d->messenger, "owner");

    char plugins_dir[4200];
    memcpy(plugins_dir, "./plugins", sizeof("./plugins"));
    plugin_init(&d->plugins, plugins_dir);

    /* TLS CA init */
    ca_init();

    /* IRC config */
    const char *irc_host = ini_get(cfg, "irc", "host");
    if (!irc_host)
        irc_host = ini_get(cfg, "irc", "server");
    snprintf(d->irc_host, sizeof(d->irc_host), "%s", irc_host ? irc_host : "irc.libera.chat");
    d->irc_port = ini_get_int(cfg, "irc", "port", 6697);
    const char *irc_nick = ini_get(cfg, "irc", "nick");
    snprintf(d->irc.nick, sizeof(d->irc.nick), "%s", irc_nick ? irc_nick : "shclaw");
    const char *irc_owner = ini_get(cfg, "irc", "owner");
    snprintf(d->irc.owner, sizeof(d->irc.owner), "%s", irc_owner ? irc_owner : "");

    /* Optional fixed channel/key */
    const char *ch = ini_get(cfg, "irc", "channel");
    if (ch) snprintf(d->irc_channel, sizeof(d->irc_channel), "%s", ch);
    const char *ck = ini_get(cfg, "irc", "channel_key");
    if (ck) snprintf(d->irc_channel_key, sizeof(d->irc_channel_key), "%s", ck);

    /* Load agents from etc/agents/ */
    d->n_agents = 0;
    char agents_dir[] = "etc/agents";
    DIR *adir = opendir(agents_dir);
    if (adir) {
        struct dirent *de;
        while ((de = readdir(adir))) {
            size_t len = strlen(de->d_name);
            if (len < 4 || strcmp(de->d_name + len - 4, ".ini") != 0) continue;
            char apath[4200];
            snprintf(apath, sizeof(apath), "%s/%s", agents_dir, de->d_name);
            load_agent(d, apath);
        }
        closedir(adir);
    }

    if (d->n_agents == 0) {
        log_error("No agents found in etc/agents/");
        return;
    }

    /* Set hub in IRC config */
    for (int i = 0; i < d->n_agents; i++) {
        if (d->agents[i].is_hub) {
            snprintf(d->irc.hub, sizeof(d->irc.hub), "%s", d->agents[i].name);
            break;
        }
    }
    if (!d->irc.hub[0])
        snprintf(d->irc.hub, sizeof(d->irc.hub), "%s", d->agents[0].name);

    /* Populate IRC agent names */
    d->irc.n_agents = d->n_agents;
    for (int i = 0; i < d->n_agents; i++)
        snprintf(d->irc.agents[i], 32, "%s", d->agents[i].name);

    /* Generate IRC secret + connect */
    generate_irc_secret(d);
    d->irc.on_trigger = on_irc_trigger;
    d->irc.ctx = d;
    irc_connect(&d->irc, d->irc_host, d->irc_port);

    /* Socket */
    snprintf(d->socket_path, sizeof(d->socket_path), "%s/shclaw.sock", d->data_dir);
    d->sock_fd = sock_server_create(d->socket_path);
    pthread_mutex_init(&d->tui_lock, NULL);
    d->n_tui_clients = 0;

    log_info("Agents ready: %d (waiting for triggers)", d->n_agents);

    /* ── Main event loop ── */
    log_info("Event loop started.");
    int64_t last_sched = 0, last_inbox = 0, last_plugin = 0;

    struct pollfd fds[2];
    fds[0].fd = irc_fd(&d->irc);
    fds[0].events = POLLIN;
    fds[1].fd = d->sock_fd;
    fds[1].events = POLLIN;

    while (!d->shutdown) {
        int nready = poll(fds, d->irc.fd >= 0 ? 2 : 1, TC_POLL_INTERVAL);
        int64_t now = now_ms();

        /* IRC */
        if (d->irc.fd >= 0 && (fds[0].revents & POLLIN)) {
            if (irc_poll(&d->irc) < 0) {
                log_warn("IRC: connection lost, reconnecting in 30s...");
                irc_disconnect(&d->irc);
                sleep(30);
                irc_connect(&d->irc, d->irc_host, d->irc_port);
                fds[0].fd = irc_fd(&d->irc);
            }
        }

        /* Socket — new connections */
        if (fds[1].revents & POLLIN) {
            int client = accept(d->sock_fd, NULL, NULL);
            if (client >= 0) {
                uint32_t cmd_type;
                char cmd_data[TC_BUF_LG];
                if (sock_read_cmd(client, &cmd_type, cmd_data, sizeof(cmd_data)) >= 0) {
                    switch (cmd_type) {
                    case SOCK_CMD_STOP:
                        d->shutdown = 1;
                        close(client);
                        break;
                    case SOCK_CMD_STATUS: {
                        char status[TC_BUF_LG];
                        int off = snprintf(status, sizeof(status),
                                           "{\"agents\":[");
                        for (int i = 0; i < d->n_agents; i++) {
                            if (i > 0) off += snprintf(status + off, sizeof(status) - off, ",");
                            off += snprintf(status + off, sizeof(status) - off,
                                "\"%s\"", d->agents[i].name);
                        }
                        off += snprintf(status + off, sizeof(status) - off, "]}");
                        sock_send_event(client, SOCK_EVT_STATUS, status, off);
                        close(client);
                        break;
                    }
                    case SOCK_CMD_IRC_INFO: {
                        char info[512];
                        int len = snprintf(info, sizeof(info),
                            "channel=%s\nkey=%s\nhost=%s\nport=%d\n",
                            d->irc.channel, d->irc.channel_key,
                            d->irc_host, d->irc_port);
                        sock_send_event(client, SOCK_EVT_IRC_INFO, info, len);
                        close(client);
                        break;
                    }
                    case SOCK_CMD_ATTACH: {
                        /* Keep connection open — register as TUI client */
                        pthread_mutex_lock(&d->tui_lock);
                        if (d->n_tui_clients < 8) {
                            d->tui_clients[d->n_tui_clients++] = client;
                            log_info("TUI: client attached (fd=%d, total=%d)",
                                     client, d->n_tui_clients);
                        } else {
                            close(client);
                        }
                        pthread_mutex_unlock(&d->tui_lock);
                        break;
                    }
                    case SOCK_CMD_MSG: {
                        char *sep = memchr(cmd_data, '\0', TC_BUF_LG);
                        if (sep) {
                            const char *agent = cmd_data;
                            const char *text = sep + 1;
                            on_irc_trigger(agent, "socket", text, d);
                        }
                        close(client);
                        break;
                    }
                    case SOCK_CMD_INPUT: {
                        on_irc_trigger(d->irc.hub, "socket", cmd_data, d);
                        close(client);
                        break;
                    }
                    default:
                        close(client);
                        break;
                    }
                } else {
                    close(client);
                }
            }
        }

        /* Poll attached TUI clients for commands */
        pthread_mutex_lock(&d->tui_lock);
        for (int i = 0; i < d->n_tui_clients; i++) {
            struct pollfd tpfd = { .fd = d->tui_clients[i], .events = POLLIN };
            if (poll(&tpfd, 1, 0) > 0) {
                if (tpfd.revents & (POLLHUP | POLLERR)) {
                    /* Client disconnected */
                    log_info("TUI: client disconnected (fd=%d)", d->tui_clients[i]);
                    close(d->tui_clients[i]);
                    d->tui_clients[i] = d->tui_clients[--d->n_tui_clients];
                    i--;
                    continue;
                }
                if (tpfd.revents & POLLIN) {
                    uint32_t cmd_type;
                    char cmd_data[TC_BUF_LG];
                    if (sock_read_cmd(d->tui_clients[i], &cmd_type,
                                      cmd_data, sizeof(cmd_data)) >= 0) {
                        if (cmd_type == SOCK_CMD_MSG) {
                            char *sep = memchr(cmd_data, '\0', TC_BUF_LG);
                            if (sep)
                                on_irc_trigger(cmd_data, "tui", sep + 1, d);
                        } else if (cmd_type == SOCK_CMD_DETACH) {
                            close(d->tui_clients[i]);
                            d->tui_clients[i] = d->tui_clients[--d->n_tui_clients];
                            i--;
                        }
                    } else {
                        /* Read failed — client gone */
                        close(d->tui_clients[i]);
                        d->tui_clients[i] = d->tui_clients[--d->n_tui_clients];
                        i--;
                    }
                }
            }
        }
        pthread_mutex_unlock(&d->tui_lock);

        /* Drain trigger queue */
        pthread_mutex_lock(&trigger_mutex);
        queued_trigger_t triggers[64];
        int n_triggers = trigger_queue_len;
        memcpy(triggers, trigger_queue, n_triggers * sizeof(queued_trigger_t));
        trigger_queue_len = 0;
        pthread_mutex_unlock(&trigger_mutex);

        for (int i = 0; i < n_triggers; i++) {
            agent_ctx_t *agent = NULL;
            for (int j = 0; j < d->n_agents; j++)
                if (strcmp(d->agents[j].name, triggers[i].agent_name) == 0)
                    { agent = &d->agents[j]; break; }
            if (!agent) continue;

            /* Check cooldown and busy — re-queue if not ready */
            int64_t now_sec = now / 1000;
            if (now_sec - agent->last_session_time < TC_SESSION_GAP) {
                enqueue_trigger(triggers[i].agent_name, triggers[i].type,
                                triggers[i].data, triggers[i].thread_id);
                continue;
            }
            if (pthread_mutex_trylock(&agent->session_lock) == 0) {
                pthread_mutex_unlock(&agent->session_lock);
            } else {
                enqueue_trigger(triggers[i].agent_name, triggers[i].type,
                                triggers[i].data, triggers[i].thread_id);
                continue;
            }

            /* Create session */
            char title[128];
            snprintf(title, sizeof(title), "%s trigger for %s",
                     triggers[i].type == TRIG_IRC ? "IRC" : "internal",
                     triggers[i].agent_name);
            cJSON *thread = session_create(&d->sessions,
                triggers[i].type == TRIG_IRC ? "irc" : "internal",
                title, "owner");
            const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(thread, "id"));
            char sid_copy[12] = "";
            if (sid) snprintf(sid_copy, sizeof(sid_copy), "%s", sid);
            cJSON_Delete(thread);

            if (triggers[i].type == TRIG_IRC) {
                session_add_message(&d->sessions, sid_copy, "owner",
                                    triggers[i].agent_name,
                                    triggers[i].data, MSG_TEXT);
            }

            dispatch_session(d, agent, triggers[i].type, triggers[i].data, sid_copy);
        }

        /* Scheduler check (every 5s) */
        if (now - last_sched >= 5000) {
            for (int i = 0; i < d->n_agents; i++) {
                cJSON *due = NULL;
                int count = sched_get_due(&d->agents[i].scheduler, &due);
                if (count > 0) {
                    char *due_json = cJSON_PrintUnformatted(due);
                    cJSON *thread = session_create(&d->sessions, "schedule",
                                                    "Scheduled task", d->agents[i].name);
                    const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(thread, "id"));
                    char sid_copy[12] = "";
                    if (sid) snprintf(sid_copy, sizeof(sid_copy), "%s", sid);
                    cJSON_Delete(thread);

                    dispatch_session(d, &d->agents[i], TRIG_SCHEDULE, due_json, sid_copy);

                    /* Mark done */
                    cJSON *ids = cJSON_CreateArray();
                    cJSON *task;
                    cJSON_ArrayForEach(task, due) {
                        const char *tid = cJSON_GetStringValue(cJSON_GetObjectItem(task, "id"));
                        if (tid) cJSON_AddItemToArray(ids, cJSON_CreateString(tid));
                    }
                    sched_mark_done(&d->agents[i].scheduler, ids);
                    cJSON_Delete(ids);
                    free(due_json);
                }
                cJSON_Delete(due);
            }
            last_sched = now;
        }

        /* Inter-agent inbox check */
        if (now - last_inbox >= 5000) {
            for (int i = 0; i < d->n_agents; i++) {
                cJSON *msgs = NULL;
                int count = messenger_receive(&d->messenger, d->agents[i].name, &msgs);
                if (count > 0) {
                    char *msgs_json = cJSON_PrintUnformatted(msgs);
                    cJSON *first = cJSON_GetArrayItem(msgs, 0);
                    const char *from = cJSON_GetStringValue(cJSON_GetObjectItem(first, "from"));
                    char title[128];
                    snprintf(title, sizeof(title), "From %s", from ? from : "?");
                    cJSON *thread = session_create(&d->sessions, "agent_message",
                                                    title, from ? from : "unknown");
                    const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(thread, "id"));
                    char sid_copy[12] = "";
                    if (sid) snprintf(sid_copy, sizeof(sid_copy), "%s", sid);
                    cJSON_Delete(thread);

                    dispatch_session(d, &d->agents[i], TRIG_AGENT_MSG, msgs_json, sid_copy);
                    free(msgs_json);
                }
                cJSON_Delete(msgs);
            }
            last_inbox = now;
        }

        /* Plugin scan */
        if (now - last_plugin >= 5000) {
            plugin_scan(&d->plugins);
            last_plugin = now;
        }

        (void)nready;
    }

    /* Shutdown */
    log_info("Shutting down...");
    irc_disconnect(&d->irc);

    /* Close attached TUI clients */
    pthread_mutex_lock(&d->tui_lock);
    for (int i = 0; i < d->n_tui_clients; i++) {
        sock_send_event(d->tui_clients[i], SOCK_EVT_GOODBYE, "shutdown", 8);
        close(d->tui_clients[i]);
    }
    d->n_tui_clients = 0;
    pthread_mutex_unlock(&d->tui_lock);

    if (d->sock_fd >= 0) {
        close(d->sock_fd);
        unlink(d->socket_path);
    }
    ca_cleanup();
    log_info("Goodbye.");
    log_close();
}

void daemon_tui_broadcast(daemon_t *d, const char *agent, const char *text) {
    if (!d || !text || !text[0]) return;

    char line[512];
    int n = snprintf(line, sizeof(line), "%s: %s", agent ? agent : "system", text);

    pthread_mutex_lock(&d->tui_lock);
    for (int i = 0; i < d->n_tui_clients; i++) {
        if (sock_send_event(d->tui_clients[i], SOCK_EVT_LINE, line, (uint32_t)n) != 0) {
            close(d->tui_clients[i]);
            d->tui_clients[i] = d->tui_clients[--d->n_tui_clients];
            i--;
            continue;
        }
    }
    pthread_mutex_unlock(&d->tui_lock);
}
