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
#include <sys/time.h>
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
#define TC_MAX_PLUGINS      32
#define TC_MAX_PARAMS       16
#define TC_MAX_TURNS        100
#define TC_BUF_SM           256
#define TC_BUF_MD           1024
#define TC_BUF_LG           4096
#define TC_BUF_XL           32768
#define TC_BUF_HUGE         65536
#define TC_SESSION_GAP      5        /* seconds between agent sessions */
#define TC_TICK_MS          1000     /* main loop tick */
#define TC_TOOL_TIMEOUT     30       /* seconds, exec default */
#define TC_TOOL_TIMEOUT_MAX 3600     /* seconds, exec upper bound */
#define TC_PLUGIN_TIMEOUT   180      /* seconds per plugin call */
#define TC_TOOL_OUTPUT_MAX  10240    /* bytes kept from exec output */
#define TC_HTTP_TIMEOUT     120      /* seconds of silence (plugin HTTP) */
#define TC_LLM_TIMEOUT      600      /* seconds of silence (LLM calls) */
#define TC_CONNECT_TIMEOUT  10       /* seconds */
#define TC_IRC_LINE_MAX     480
#define TC_EMPTY_OUTPUT_MARKER "[empty output]"

/* ── Enums ──────────────────────────────────────────────── */

typedef enum {
    TRIG_IRC,           /* owner message: IRC, TUI or CLI */
    TRIG_SCHEDULE,
    TRIG_AGENT_MSG,
} trigger_type_t;

typedef enum {
    MSG_TEXT,
    MSG_THINKING,
    MSG_TOOL_CALL,
    MSG_TOOL_RESULT,
    MSG_DELEGATION,
} msg_type_t;

typedef enum {
    SESS_ACTIVE,
    SESS_CLOSED,
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

/* ── Logging ────────────────────────────────────────────── */

void log_init(const char *log_dir);
void log_close(void);
void log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ── Utilities ──────────────────────────────────────────── */

void    uuid_short(char *out, int len);
int     write_all(int fd, const void *buf, size_t len);
int     atomic_write(const char *path, const char *data, size_t len, mode_t mode);
char   *file_slurp(const char *path, size_t *out_len);
int     file_exists(const char *path);
int     mkdirs(const char *path);
int     mkdirs_for(const char *path); /* mkdirs() of parent of given path */
void    format_time(time_t t, int local, char *buf, size_t sz); /* ISO 8601, Z if UTC */
void    now_iso(char *buf, size_t sz);                            /* now, UTC */
int64_t now_ms(void);
/* Append at *off; stops at sz - 1 (output is truncated, never overflows). */
void    buf_appendf(char *buf, size_t sz, size_t *off, const char *fmt, ...)
            __attribute__((format(printf, 4, 5)));
/* Longest prefix of s, at most max bytes, that keeps UTF-8 sequences whole
 * (for "%.*s": APIs reject JSON with broken UTF-8) */
int     utf8_prefix(const char *s, int max);
/* data_dir from etc/config.ini, for CLI/TUI clients */
void    cli_data_dir(char *out, size_t sz);

/* JSON helpers — NULL-safe; numbers and booleans also accept strings */
const char *j_str(cJSON *obj, const char *key);
int         j_int(cJSON *obj, const char *key, int def);
int         j_bool(cJSON *obj, const char *key, int def);

/* JSON file I/O. A missing file loads as an empty array/object; a file
 * that exists but does not parse returns NULL: never overwrite it, a
 * one-character hand edit would erase everything. */
cJSON *json_load(const char *path, int want_array);
int    json_save_atomic(const char *path, cJSON *obj, int formatted);
/* Replace invalid or truncated UTF-8 sequences with '?', in place */
void   utf8_scrub(char *s);

/* Child processes: read the output pipe until EOF or timeout, keep up to
 * cap bytes (out needs cap + 1), then reap. On timeout the process group
 * is killed. Returns the wait status, or -1 on timeout. */
int child_collect(pid_t pid, int fd, int timeout_s, char *out, size_t cap,
                  size_t *len, size_t *dropped);

/* ── Provider ───────────────────────────────────────────── */

typedef struct {
    char provider_name[32];
    char provider_type[16];    /* "anthropic" or "openai" */
    char api_key[256];
    char base_url[256];
    char model[64];
    int  max_tokens;
    int  timeout;              /* seconds */
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
    char          stop_reason[32];
    cJSON        *content;     /* assistant turn to replay (Anthropic blocks) */
    char          error[256];  /* set when llm_call() fails */
} llm_response_t;

int  llm_call(provider_ref_t *prov, const char *system_prompt,
              cJSON *messages, cJSON *tools, llm_response_t *out);
void llm_response_free(llm_response_t *r);

/* ── HTTP ───────────────────────────────────────────────── */

#define HTTP_ERR_CONNECT  -1
#define HTTP_ERR_TLS      -2
#define HTTP_ERR_PROTO    -3   /* no or malformed response */
#define HTTP_ERR_REQUEST  -4   /* bad URL or header */
#define HTTP_ERR_NOMEM    -5
#define HTTP_ERR_TIMEOUT  -6

typedef struct {
    int    status;       /* HTTP status, or HTTP_ERR_* */
    char  *body;
    size_t body_len;
    int    retry_after;  /* seconds from Retry-After, 0 if absent */
    char   location[1024];  /* Location of a redirect, "" otherwise */
} http_response_t;

http_response_t http_get(const char *url);
http_response_t http_post(const char *url, const char *content_type,
                          const char *body, size_t body_len);
http_response_t http_post_json(const char *url, const char *json);
/* Per-thread settings, reset after each request */
void http_set_header(const char *name, const char *value);
void http_set_timeout(int seconds);
const char *http_strerror(int status);
void http_response_free(http_response_t *r);
size_t http_dechunk(char *body, size_t len);  /* in place, returns new len */
int  net_connect(const char *host, int port, int timeout_s);

/* TLS 1.2 client over *fd (modern ciphers only); -1 without CA anchors.
 * sc, xc, ioc: BearSSL client, X.509 and sslio contexts */
int  tls_client_start(void *sc, void *xc, void *ioc,
                      unsigned char *iobuf, size_t iobuf_sz,
                      const char *host, int *fd,
                      int (*rd)(void *, unsigned char *, size_t),
                      int (*wr)(void *, const unsigned char *, size_t));
int  net_write(void *ctx, const unsigned char *buf, size_t len); /* ctx: int *fd */

/* Set in a plugin test-run child: tc_http_* calls are traced there */
extern int http_trace_fd;

/* Also exposed to plugins as tc_http_* */
int  tc_http_get(const char *url, char *buf, size_t buf_sz);
int  tc_http_post(const char *url, const char *content_type,
                  const char *body, size_t body_len,
                  char *resp, size_t resp_sz);
int  tc_http_post_json(const char *url, const char *json,
                       char *resp, size_t resp_sz);
void tc_http_header(const char *name, const char *value);

/* ── TLS / CA ───────────────────────────────────────────── */

int  ca_init(const char *data_dir);

/* ── Sessions ───────────────────────────────────────────── */

typedef struct {
    char sessions_dir[4096];
    pthread_mutex_t lock;
} session_store_t;

void session_store_init(session_store_t *s, const char *dir);
/* Returns the new session id (written to sid), or NULL */
const char *session_create(session_store_t *s, const char *type,
                           const char *title, const char *initiator, char sid[12]);
int   session_add_message(session_store_t *s, const char *sid,
                          const char *sender, const char *recipient,
                          const char *content, msg_type_t msg_type);
int   session_set_status(session_store_t *s, const char *sid, session_status_t status);

/* ── Memory ─────────────────────────────────────────────── */

typedef struct {
    char memory_dir[4096];
    pthread_mutex_t lock;
    cJSON *cache;       /* in-memory entries cache */
} memory_t;

void        memory_init(memory_t *m, const char *dir);
const char *memory_add(memory_t *m, const char *content, const char *category,
                       int importance, const char *tags_csv,
                       char *out, size_t out_sz);
const char *memory_search(memory_t *m, const char *query, int n,
                          char *out, size_t out_sz);
const char *facts_set(memory_t *m, const char *key, const char *value,
                      char *out, size_t out_sz);
const char *facts_get(memory_t *m, const char *key,  /* "" = all facts */
                      char *out, size_t out_sz);
void        memory_clear(memory_t *m);
void        facts_clear(memory_t *m);

/* ── Scheduler ──────────────────────────────────────────── */

typedef struct {
    char path[4096];
    pthread_mutex_t lock;
} scheduler_t;

void        scheduler_init(scheduler_t *s, const char *json_path);
time_t      sched_parse_time(const char *iso);  /* 0 if invalid */
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
    char agents[TC_MAX_AGENTS + 1][32];  /* agents + "owner" */
    int  n_agents;
} messenger_t;

void        messenger_init(messenger_t *m, const char *dir);
void        messenger_register(messenger_t *m, const char *agent_name);
/* Registered name matching `name` (case-insensitive, leading '@' ignored) */
const char *messenger_resolve(messenger_t *m, const char *name);
/* reply: the message answers an earlier request (no answer expected) */
const char *messenger_send(messenger_t *m, const char *from, const char *to,
                           const char *content, const char *thread_id, int reply,
                           char *out, size_t out_sz);
int         messenger_receive(messenger_t *m, const char *agent_name, cJSON **out);

/* ── IRC ────────────────────────────────────────────────── */

typedef struct {
    int  fd;
    int  enabled;
    int  registered;    /* 001 received on this connection */
    char base_nick[32]; /* configured nick, restored on each connect */
    char nick[32];
    char channel[64];
    char channel_key[32];
    char owner[64];
    char hub[32];
    char agents[TC_MAX_AGENTS][32];
    int  n_agents;
    char readbuf[4096];
    int  readbuf_len;
    int64_t last_rx;    /* ms, last byte from the server */
    int  pinged;        /* keepalive PING sent, waiting for data */

    void (*on_trigger)(const char *agent, const char *from,
                       const char *text, void *ctx);
    void *ctx;
    /* BearSSL state stored as opaque bytes to avoid exposing bearssl headers */
    void *tls_state;
} irc_t;

int  irc_connect(irc_t *irc, const char *host, int port);
int  irc_poll(irc_t *irc);
int  irc_keepalive(irc_t *irc);   /* -1 when the link looks dead */
void irc_reply(irc_t *irc, const char *agent_name, const char *text);
void irc_action(irc_t *irc, const char *agent_name, const char *text);
void irc_disconnect(irc_t *irc);

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
} plugin_registry_t;

void        plugin_init(plugin_registry_t *r, const char *plugins_dir);
void        plugin_scan(plugin_registry_t *r);
/* Compiles code (or the file when code is NULL), registered as src_path.
 * err receives compiler diagnostics on failure, the plugin name on success */
int         plugin_compile(plugin_registry_t *r, const char *src_path, const char *code,
                           time_t mtime, char *err, size_t err_sz);
cJSON      *plugin_get_schemas(plugin_registry_t *r);
/* The tc_* functions plugins can call, comma-separated */
const char *plugin_api_names(void);
/* NULL if no such plugin. trace: prefix HTTP calls to the output */
const char *plugin_execute(plugin_registry_t *r, const char *name, cJSON *input,
                           int trace, char *out, size_t out_sz);

/* ── Socket (TUI server) ────────────────────────────────── */

#define SOCK_CMD_ATTACH     1
#define SOCK_CMD_DETACH     3
#define SOCK_CMD_STATUS     4
#define SOCK_CMD_MSG        5
#define SOCK_CMD_STOP       6
#define SOCK_CMD_IRC_INFO   7

#define SOCK_EVT_LINE       1
#define SOCK_EVT_STATUS     2
#define SOCK_EVT_IRC_INFO   3
#define SOCK_EVT_GOODBYE    5

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t len;
} wire_header_t;

int  sock_server_create(const char *path);
int  sock_send(int fd, uint32_t type, const char *data, uint32_t len);
int  sock_read_cmd(int fd, uint32_t *type, char *data, size_t max_len);
int  sock_client_connect(void);   /* daemon socket from etc/config.ini */

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
    const char *choices;      /* allowed values, comma-separated, or NULL */
} tc_param_t;

typedef struct {
    const char *name;
    const char *desc;
    tc_param_t  params[TC_MAX_PARAMS];
} tool_def_t;

cJSON *tools_to_json(int is_builder);
int    tool_find(const char *name);        /* built-in tool id or -1 */
int    tool_name_valid(const char *name);  /* [A-Za-z0-9_-]{1,64} */

/* Forward declare agent context for tool execution */
typedef struct agent_ctx agent_ctx_t;

/* NULL when input matches the tool schema, else an error message in out */
const char *tool_check_args(int tool_id, cJSON *input, char *out, size_t out_sz);
const char *execute_tool(int tool_id, cJSON *input, agent_ctx_t *ctx,
                         char *out, size_t out_sz);
const char *tool_exec_cmd(const char *cmd, int timeout,
                          char *out, size_t out_sz);
/* Message an agent or the owner, and show it on IRC/TUI */
const char *agent_message(agent_ctx_t *ctx, const char *to, const char *content,
                          const char *thread_id, int reply, char *out, size_t out_sz);

/* ── Agent ──────────────────────────────────────────────── */

struct agent_ctx {
    char         name[32];
    char         personality[TC_BUF_LG];
    char         system_prompt_extra[TC_BUF_XL];
    char         specialty[256];
    int          max_turns;
    int          history_budget;  /* chars of tool output kept, 0 = all */
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
    agent_ctx_t *peers;           /* every agent, this one included */
    int          n_peers;

    /* Runtime, shared with the main loop (atomic access) */
    int          busy;
    int          last_session_time;   /* monotonic seconds */
};

#define SESSION_COMPLETED 0
#define SESSION_FAILED    2

int  agent_run_session(agent_ctx_t *agent, trigger_type_t trig_type,
                       const char *trig_data, const char *thread_id);

/* ── Daemon ─────────────────────────────────────────────── */

typedef struct {
    /* Config */
    char        data_dir[4096];
    char        log_dir[4096];
    char        socket_path[4200];
    char        irc_host[256];
    int         irc_port;

    /* Provider registry (model left empty) and model tiers */
    provider_ref_t providers[8];
    int n_providers;
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
    int64_t          irc_retry_at;    /* ms, next reconnect attempt */
    int              irc_backoff;     /* seconds */
    int              sock_fd;
    int              tui_clients[8];  /* attached TUI client fds */
    int              n_tui_clients;
    pthread_mutex_t  tui_lock;
    agent_ctx_t      agents[TC_MAX_AGENTS];
    int              n_agents;
    volatile sig_atomic_t shutdown;
} daemon_t;

/* Push a line to all attached TUI clients */
void daemon_tui_broadcast(daemon_t *d, const char *agent, const char *text);

int  daemon_run(daemon_t *d, ini_t *cfg);   /* 0, or 1 when it cannot start */
int  daemon_resolve_provider(daemon_t *d, const char *model_ref, provider_ref_t *out);

#endif /* TC_H */
