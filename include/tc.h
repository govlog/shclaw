/*
 * tc.h — shclaw master internal header
 */

#ifndef TC_H
#define TC_H

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <termios.h>

#include "../vendor/cjson/cJSON.h"

/* ── Constants ──────────────────────────────────────────── */

#define TC_VERSION          "0.1.0"
#define TC_MAX_AGENTS       16
#define TC_MAX_TOOLS        32
#define TC_MAX_PLUGINS      32
#define TC_MAX_PARAMS       16
#define TC_MAX_TURNS        100
#define TC_BUF_SM           256
#define TC_BUF_MD           1024
#define TC_BUF_LG           4096
#define TC_BUF_XL           32768
#define TC_BUF_HUGE         65536
#define TC_SESSION_GAP      5        /* seconds between agent sessions */
#define TC_POLL_INTERVAL    5000     /* ms */
#define TC_TOOL_TIMEOUT     30       /* seconds */
#define TC_TOOL_OUTPUT_MAX  10240    /* bytes */
#define TC_HTTP_TIMEOUT     120      /* seconds */
#define TC_MAX_SCROLLBACK   200
#define TC_IRC_LINE_MAX     480
#define TC_EMPTY_OUTPUT_MARKER "[empty output]"

/* ── Enums ──────────────────────────────────────────────── */

typedef enum {
    TRIG_STARTUP,
    TRIG_IRC,
    TRIG_SCHEDULE,
    TRIG_AGENT_MSG,
    TRIG_SOCKET,        /* from TUI client */
} trigger_type_t;

typedef enum {
    MSG_TEXT,
    MSG_SYSTEM,
    MSG_THINKING,
    MSG_TOOL_CALL,
    MSG_TOOL_RESULT,
    MSG_DELEGATION,
} msg_type_t;

typedef enum {
    SESS_ACTIVE,
    SESS_CLOSED,
    SESS_ABORTED,
    SESS_FAILED,
} session_status_t;

typedef enum {
    TOOL_EXEC,
    TOOL_READ_FILE,
    TOOL_WRITE_FILE,
    TOOL_SCHEDULE_TASK,
    TOOL_SCHEDULE_RECURRING,
    TOOL_LIST_TASKS,
    TOOL_UPDATE_TASK,
    TOOL_CANCEL_TASK,
    TOOL_REMEMBER,
    TOOL_RECALL,
    TOOL_SET_FACT,
    TOOL_GET_FACT,
    TOOL_SEND_MESSAGE,
    TOOL_LIST_AGENTS,
    TOOL_CREATE_PLUGIN,
    TOOL_CLEAR_MEMORY,
    TOOL_COUNT,
} tool_id_t;

/* ── INI config ─────────────────────────────────────────── */

typedef struct {
    char section[64];
    char key[64];
    char *value;
} ini_entry_t;

typedef struct {
    ini_entry_t *entries;
    int          count;
    int          capacity;
} ini_t;

ini_t      *ini_load(const char *path);
void        ini_free(ini_t *ini);
const char *ini_get(ini_t *ini, const char *section, const char *key);
int         ini_get_int(ini_t *ini, const char *section, const char *key, int def);
int         ini_get_bool(ini_t *ini, const char *section, const char *key, int def);
int         ini_section_foreach(ini_t *ini, const char *section,
                void (*cb)(const char *key, const char *val, void *ctx), void *ctx);

/* ── Logging ────────────────────────────────────────────── */

void log_init(const char *log_dir);
void log_close(void);
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);
void log_debug(const char *fmt, ...);

/* ── Utilities ──────────────────────────────────────────── */

void    uuid_short(char *out, int len);
int     atomic_write(const char *path, const char *data, size_t len);
char   *file_slurp(const char *path, size_t *out_len);
int     file_exists(const char *path);
int     mkdirs(const char *path);
void    now_iso(char *buf, size_t sz);
int64_t now_ms(void);
void    sha256_hex(const void *data, size_t len, char out[65]);

/* ── Provider ───────────────────────────────────────────── */

typedef struct {
    char provider_name[32];
    char provider_type[16];    /* "anthropic" or "openai" */
    char api_key[256];
    char base_url[256];
    char model[64];
} provider_ref_t;

typedef struct {
    char type[16];    /* "text" or "thinking" */
    char *text;
} text_block_t;

typedef struct {
    char id[64];
    char name[64];
    char *input_json;
} tool_call_t;

typedef struct {
    text_block_t *text_blocks;
    int           n_text;
    tool_call_t  *tool_calls;
    int           n_tools;
    char          stop_reason[16];
} llm_response_t;

int  llm_call(provider_ref_t *prov, const char *system_prompt,
              cJSON *messages, cJSON *tools, llm_response_t *out);
void llm_response_free(llm_response_t *r);

/* ── HTTP ───────────────────────────────────────────────── */

typedef struct {
    int    status;
    char  *body;
    size_t body_len;
    char   content_type[128];
} http_response_t;

http_response_t http_get(const char *url);
http_response_t http_post(const char *url, const char *content_type,
                          const char *body, size_t body_len);
http_response_t http_post_json(const char *url, const char *json);
void http_set_header(const char *name, const char *value);
void http_response_free(http_response_t *r);

/* Minimal TLS client init (only modern ciphers, TLS 1.2 only) */
struct br_ssl_client_context_;
struct br_x509_minimal_context_;
void ssl_client_init_minimal(void *sc, void *xc,
                             const void *anchors, size_t anchor_count);

/* Also exposed to plugins as tc_http_* */
int  tc_http_get(const char *url, char *buf, size_t buf_sz);
int  tc_http_post(const char *url, const char *content_type,
                  const char *body, size_t body_len,
                  char *resp, size_t resp_sz);
int  tc_http_post_json(const char *url, const char *json,
                       char *resp, size_t resp_sz);
void tc_http_header(const char *name, const char *value);

/* ── TLS / CA ───────────────────────────────────────────── */

int  ca_init(void);
void ca_cleanup(void);

/* ── Sessions ───────────────────────────────────────────── */

typedef struct {
    char sessions_dir[4096];
    pthread_mutex_t lock;
} session_store_t;

void session_store_init(session_store_t *s, const char *dir);
cJSON *session_create(session_store_t *s, const char *type,
                      const char *title, const char *initiator);
int   session_add_message(session_store_t *s, const char *sid,
                          const char *sender, const char *recipient,
                          const char *content, msg_type_t msg_type);
int   session_close(session_store_t *s, const char *sid);
int   session_set_status(session_store_t *s, const char *sid, session_status_t status);
cJSON *session_get(session_store_t *s, const char *sid);
cJSON *session_list_recent(session_store_t *s, int n);

/* ── Memory ─────────────────────────────────────────────── */

typedef struct {
    char memory_dir[4096];
    pthread_mutex_t lock;
    cJSON *cache;       /* in-memory entries cache */
    int    dirty;
} memory_t;

void        memory_init(memory_t *m, const char *dir);
const char *memory_add(memory_t *m, const char *content, const char *category,
                       int importance, const char *tags_csv,
                       char *out, size_t out_sz);
const char *memory_search(memory_t *m, const char *query, int n,
                          char *out, size_t out_sz);
const char *memory_get_relevant(memory_t *m, cJSON *hints, int max_results,
                                char *out, size_t out_sz);
const char *facts_set(memory_t *m, const char *key, const char *value,
                      char *out, size_t out_sz);
const char *facts_get(memory_t *m, const char *key,
                      char *out, size_t out_sz);
void        memory_clear(memory_t *m);
void        facts_clear(memory_t *m);

/* ── Scheduler ──────────────────────────────────────────── */

typedef struct {
    char path[4096];
    pthread_mutex_t lock;
} scheduler_t;

void        scheduler_init(scheduler_t *s, const char *json_path);
const char *sched_add(scheduler_t *s, const char *run_at, const char *desc,
                      const char *prompt, const char *interval,
                      char *out, size_t out_sz);
const char *sched_list(scheduler_t *s, char *out, size_t out_sz);
const char *sched_update(scheduler_t *s, const char *id, const char *run_at,
                         const char *desc, const char *prompt,
                         const char *interval, char *out, size_t out_sz);
const char *sched_cancel(scheduler_t *s, const char *id,
                         char *out, size_t out_sz);
int         sched_get_due(scheduler_t *s, cJSON **out);
void        sched_mark_done(scheduler_t *s, cJSON *ids);

/* ── Messenger ──────────────────────────────────────────── */

typedef struct {
    char dir[4096];
    pthread_mutex_t lock;
    char agents[TC_MAX_AGENTS][32];
    int  n_agents;
} messenger_t;

void        messenger_init(messenger_t *m, const char *dir);
void        messenger_register(messenger_t *m, const char *agent_name);
const char *messenger_send(messenger_t *m, const char *from, const char *to,
                           const char *content, const char *thread_id,
                           char *out, size_t out_sz);
int         messenger_receive(messenger_t *m, const char *agent_name, cJSON **out);

/* ── IRC ────────────────────────────────────────────────── */

typedef struct {
    int  fd;
    char nick[32];
    char channel[64];
    char channel_key[32];
    char owner[64];
    char hub[32];
    char agents[TC_MAX_AGENTS][32];
    int  n_agents;
    char readbuf[4096];
    int  readbuf_len;

    void (*on_trigger)(const char *agent, const char *from,
                       const char *text, void *ctx);
    void *ctx;
    /* BearSSL state stored as opaque bytes to avoid exposing bearssl headers */
    void *tls_state;
} irc_t;

int  irc_connect(irc_t *irc, const char *host, int port);
int  irc_poll(irc_t *irc);
void irc_reply(irc_t *irc, const char *agent_name, const char *text);
void irc_action(irc_t *irc, const char *agent_name, const char *text);
void irc_disconnect(irc_t *irc);
int  irc_fd(irc_t *irc);

/* ── IRC Parse ──────────────────────────────────────────── */

typedef struct {
    char agent[32];
    char text[TC_IRC_LINE_MAX];
} mention_t;

int parse_mentions(const char *msg,
                   const char agents[][32], int n_agents,
                   const char *hub,
                   mention_t *out, int max_out);

/* ── Plugins ────────────────────────────────────────────── */

typedef struct {
    char name[64];
    cJSON *schema;
    const char *(*execute)(const char *input);
    void *tcc_state;       /* kept alive so symbols remain valid */
    time_t mtime;
    char src_path[4096];
} plugin_entry_t;

typedef struct {
    char dir[4096];
    plugin_entry_t plugins[TC_MAX_PLUGINS];
    int count;
    pthread_mutex_t lock;
    time_t failed_mtime[TC_MAX_PLUGINS]; /* track failed compiles to avoid log spam */
    char   failed_src[TC_MAX_PLUGINS][4096];
    int    n_failed;
    char   last_error[TC_BUF_LG];       /* captured TCC error for create_plugin feedback */
} plugin_registry_t;

void        plugin_init(plugin_registry_t *r, const char *plugins_dir);
void        plugin_scan(plugin_registry_t *r);
int         plugin_compile(plugin_registry_t *r, const char *src_path, time_t mtime);
cJSON      *plugin_get_schemas(plugin_registry_t *r);
const char *plugin_execute(plugin_registry_t *r, const char *name, cJSON *input,
                           char *out, size_t out_sz);

/* ── Socket (TUI server) ────────────────────────────────── */

#define SOCK_CMD_ATTACH     1
#define SOCK_CMD_INPUT      2
#define SOCK_CMD_DETACH     3
#define SOCK_CMD_STATUS     4
#define SOCK_CMD_MSG        5
#define SOCK_CMD_STOP       6
#define SOCK_CMD_IRC_INFO   7

#define SOCK_EVT_LINE       1
#define SOCK_EVT_STATUS     2
#define SOCK_EVT_IRC_INFO   3
#define SOCK_EVT_SCROLLBACK 4
#define SOCK_EVT_GOODBYE    5

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t len;
} wire_header_t;

int  sock_server_create(const char *path);
void sock_server_accept(int listen_fd, struct pollfd *fds, int *n_clients, int max_clients);
int  sock_send_cmd(int fd, uint32_t type, const char *data, uint32_t len);
int  sock_send_event(int client_fd, uint32_t type, const char *data, uint32_t len);
int  sock_read_cmd(int fd, uint32_t *type, char *data, size_t max_len);
int  sock_client_connect(const char *path);

/* ── TUI ───────────────────────────────────────────────── */

int  tui_run(const char *target_agent);

/* ── Tools ──────────────────────────────────────────────── */

#define TC_STRING 1
#define TC_INT    2
#define TC_BOOL   3
#define TC_FLOAT  4

typedef struct {
    const char *name;
    int         type;
    const char *description;
    int         required;
} tc_param_t;

typedef struct {
    const char *name;
    const char *desc;
    tc_param_t  params[TC_MAX_PARAMS];
} tool_def_t;

cJSON *tools_to_json(int is_builder);

/* Forward declare agent context for tool execution */
typedef struct agent_ctx agent_ctx_t;

const char *execute_tool(int tool_id, cJSON *input, agent_ctx_t *ctx,
                         char *out, size_t out_sz);
const char *tool_exec_cmd(const char *cmd, int timeout,
                          char *out, size_t out_sz);

/* ── Agent ──────────────────────────────────────────────── */

struct agent_ctx {
    char         name[32];
    char         personality[TC_BUF_LG];
    char         system_prompt_extra[TC_BUF_XL];
    char         specialty[256];
    char         owner_email[128];
    int          max_turns;
    int          is_hub;
    int          is_builder;
    provider_ref_t provider;
    memory_t     memory;
    scheduler_t  scheduler;
    messenger_t *messenger;
    session_store_t *sessions;
    plugin_registry_t *plugins;
    irc_t       *irc;
    char        *data_dir;
    char         objectives[16][256];
    int          n_objectives;

    /* Runtime */
    pthread_mutex_t session_lock;
    int64_t         last_session_time;
    volatile int    abort_flag;
};

#define SESSION_COMPLETED 0
#define SESSION_ABORTED   1
#define SESSION_FAILED    2

int  agent_run_session(agent_ctx_t *agent, trigger_type_t trig_type,
                       const char *trig_data, const char *thread_id,
                       const char **all_agents, const char **all_specialties,
                       int n_agents);
void agent_build_system_prompt(agent_ctx_t *agent, trigger_type_t trig_type,
                               const char *trig_data, const char *thread_id,
                               const char **all_agents,
                               const char **all_specialties, int n_agents,
                               char *out, size_t out_sz);

/* ── Daemon ─────────────────────────────────────────────── */

typedef struct {
    /* Config */
    char        data_dir[4096];
    char        log_dir[4096];
    char        socket_path[4096];
    char        irc_host[256];
    int         irc_port;
    char        irc_nick[32];
    char        irc_owner[64];
    char        irc_channel[64];     /* may be overridden in config */
    char        irc_channel_key[32]; /* may be overridden in config */

    /* Provider registry */
    struct {
        char name[32];
        char type[16];
        char api_key[256];
        char base_url[256];
    } providers[8];
    int n_providers;

    /* Model tiers */
    struct {
        char tier[32];
        char model_ref[96];
    } tiers[8];
    int n_tiers;

    /* Runtime */
    session_store_t  sessions;
    messenger_t      messenger;
    plugin_registry_t plugins;
    irc_t            irc;
    int              sock_fd;
    int              tui_clients[8];  /* attached TUI client fds */
    int              n_tui_clients;
    pthread_mutex_t  tui_lock;
    agent_ctx_t      agents[TC_MAX_AGENTS];
    int              n_agents;
    volatile int     shutdown;
} daemon_t;

/* Push a line to all attached TUI clients */
void daemon_tui_broadcast(daemon_t *d, const char *agent, const char *text);

void daemon_run(daemon_t *d, ini_t *cfg);
int  daemon_resolve_provider(daemon_t *d, const char *model_ref, provider_ref_t *out);

#endif /* TC_H */
