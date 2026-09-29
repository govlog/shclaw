/*
 * check.c — behaviour checks for the parts that parse untrusted input
 * (HTTP, IRC, model tool calls). Run with `make check`.
 */

#include "../include/tc.h"
#include <netinet/in.h>

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

/* Stubs for daemon.c symbols used by irc.c */
void daemon_tui_broadcast(daemon_t *d, const char *agent, const char *text) {
    (void)d; (void)agent; (void)text;
}

static void check_dechunk(void) {
    char body[] = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    size_t n = http_dechunk(body, strlen(body));
    CHECK(n == 9 && strcmp(body, "Wikipedia") == 0, "dechunk got '%s'", body);

    /* A chunk size past the data (or near SIZE_MAX) must be clamped */
    char evil[] = "ffffffffffffffff\r\nabc";
    n = http_dechunk(evil, strlen(evil));
    CHECK(n == 3 && strcmp(evil, "abc") == 0, "huge chunk not clamped: %zu", n);
}

/* Listening socket on a free loopback port */
static int listen_local(int *port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t alen = sizeof(a);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 8) != 0 ||
        getsockname(s, (struct sockaddr *)&a, &alen) != 0) {
        close(s);
        return -1;
    }
    *port = ntohs(a.sin_port);
    return s;
}

/* Local HTTP server: /a redirects to "b?x=1", anything else echoes its
 * request line in a chunked body. Serves n requests, then exits. */
static pid_t http_server(int n, int *port) {
    int s = listen_local(port);
    if (s < 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0; i < n; i++) {
            int c = accept(s, NULL, NULL);
            char req[2048], resp[4096];
            ssize_t len = read(c, req, sizeof(req) - 1);
            req[len > 0 ? len : 0] = '\0';
            req[strcspn(req, "\r")] = '\0';
            if (!strncmp(req, "GET /a ", 7))
                snprintf(resp, sizeof(resp), "HTTP/1.1 301 Moved\r\nLocation: b?x=1\r\n"
                         "Content-Length: 0\r\n\r\n");
            else
                snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked"
                         "\r\n\r\n%zx\r\n%s\r\n0\r\n\r\n", strlen(req), req);
            write_all(c, resp, strlen(resp));
            close(c);
        }
        _exit(0);
    }
    close(s);
    return pid;
}

static void check_http(void) {
    int port;
    pid_t srv = http_server(4, &port);
    CHECK(srv > 0, "cannot start the local HTTP server");
    if (srv <= 0) return;

    const char *cases[][2] = {
        { "/a", "GET /b?x=1 HTTP/1.1" },                     /* relative redirect */
        { "?q=1", "GET /?q=1 HTTP/1.1" },                     /* no path */
        { "/city/New York#top", "GET /city/New%20York HTTP/1.1" },
    };
    for (int i = 0; i < 3; i++) {
        char url[256];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d%s", port, cases[i][0]);
        http_response_t r = http_get(url);
        CHECK(r.status == 200 && r.body && !strcmp(r.body, cases[i][1]),
              "GET %s: status %d, body '%s'", url, r.status, r.body ? r.body : "");
        http_response_free(&r);
    }
    waitpid(srv, NULL, 0);
}

/* Fake OpenAI server: saves request i to <dir>/req<i>.json and answers
 * "ok". Serves n requests, then exits. */
static pid_t llm_server(int n, const char *dir, int *port) {
    int s = listen_local(port);
    if (s < 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        static char req[4 * TC_BUF_HUGE];
        const char *reply = "{\"choices\":[{\"message\":{\"role\":\"assistant\","
                            "\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}";
        for (int i = 0; i < n; i++) {
            int c = accept(s, NULL, NULL);
            size_t len = 0, want = 0;
            char *body = NULL;
            ssize_t r;
            while ((!body || len - (size_t)(body - req) < want) &&
                   (r = read(c, req + len, sizeof(req) - 1 - len)) > 0) {
                req[len += (size_t)r] = '\0';
                if (!body && (body = strstr(req, "\r\n\r\n")) != NULL) {
                    const char *cl = strstr(req, "Content-Length: ");
                    want = cl ? strtoul(cl + 16, NULL, 10) : 0;
                    body += 4;
                }
            }
            char path[4200], resp[512];
            snprintf(path, sizeof(path), "%s/req%d.json", dir, i);
            if (body) atomic_write(path, body, len - (size_t)(body - req), 0600);
            snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s",
                     strlen(reply), reply);
            write_all(c, resp, strlen(resp));
            close(c);
        }
        _exit(0);
    }
    close(s);
    return pid;
}

/* Tools + system prompt open every request: they must stay byte-identical
 * from one session to the next, or no provider can reuse its cache. */
static void check_prompt_cache(const char *tmp) {
    int port;
    pid_t srv = llm_server(2, tmp, &port);
    CHECK(srv > 0, "cannot start the fake LLM server");
    if (srv <= 0) return;

    static agent_ctx_t a;              /* large: not on the stack */
    static session_store_t sessions;
    char path[4200], out[256];
    snprintf(a.name, sizeof(a.name), "tester");
    snprintf(a.provider.provider_type, sizeof(a.provider.provider_type), "openai");
    snprintf(a.provider.base_url, sizeof(a.provider.base_url), "http://127.0.0.1:%d", port);
    snprintf(a.provider.model, sizeof(a.provider.model), "test");
    snprintf(path, sizeof(path), "%s/tester-memory", tmp);
    memory_init(&a.memory, path);
    snprintf(path, sizeof(path), "%s/tester-schedule.json", tmp);
    scheduler_init(&a.scheduler, path);
    snprintf(path, sizeof(path), "%s/tester-sessions", tmp);
    session_store_init(&sessions, path);
    a.sessions = &sessions;

    agent_run_session(&a, TRIG_IRC, "hello", "t1");
    memory_add(&a.memory, "Owner likes tea", "person", 5, NULL, out, sizeof(out));
    agent_run_session(&a, TRIG_IRC, "bye", "t2");
    waitpid(srv, NULL, 0);

    char *prefix[2], *turn[2];
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/req%d.json", tmp, i);
        char *body = file_slurp(path, NULL);
        cJSON *req = cJSON_Parse(body ? body : "");
        cJSON *msgs = cJSON_GetObjectItem(req, "messages");
        cJSON *head = cJSON_CreateArray();
        cJSON_AddItemReferenceToArray(head, cJSON_GetObjectItem(req, "tools"));
        cJSON_AddItemReferenceToArray(head, cJSON_GetArrayItem(msgs, 0));
        prefix[i] = cJSON_PrintUnformatted(head);
        const char *content = j_str(cJSON_GetArrayItem(msgs, 1), "content");
        turn[i] = strdup(content ? content : "");
        cJSON_Delete(head);
        cJSON_Delete(req);
        free(body);
    }
    CHECK(prefix[0] && prefix[1] && strlen(prefix[0]) > 1000 && !strcmp(prefix[0], prefix[1]),
          "tools + system prompt changed between sessions");
    CHECK(strstr(turn[1], "likes tea") && strstr(turn[1], "bye") && !strstr(turn[0], "likes tea"),
          "memories and trigger not in the first user turn:\n%s", turn[1]);
    for (int i = 0; i < 2; i++) { free(prefix[i]); free(turn[i]); }
}

static const char AGENTS[3][32] = { "jarvis", "Oracle", "builder" };

static int mentions(const char *msg, mention_t *out) {
    return parse_mentions(msg, AGENTS, 3, "jarvis", out, 8);
}

static void check_mentions(void) {
    mention_t m[8];
    int n = mentions("hello @all", m);
    CHECK(n == 1 && !strcmp(m[0].agent, "all") && !strcmp(m[0].text, "hello"),
          "@all at end: %d '%s'", n, n ? m[0].text : "");

    n = mentions("@oracle check GPU prices", m);
    CHECK(n == 1 && !strcmp(m[0].agent, "Oracle") && !strcmp(m[0].text, "check GPU prices"),
          "case-insensitive mention keeps the configured name: '%s'", n ? m[0].agent : "");

    n = mentions("mail bob@oracle.com please", m);
    CHECK(n == 1 && !strcmp(m[0].agent, "jarvis"), "email taken as mention");

    n = mentions("@oracle @builder do X", m);
    CHECK(n == 2 && !strcmp(m[0].text, "do X") && !strcmp(m[1].text, "do X"),
          "empty segment should take the next text");

    n = mentions("hi @oracle X @jarvis Y", m);
    CHECK(n == 3 && !strcmp(m[0].agent, "jarvis") && !strcmp(m[0].text, "hi") &&
          !strcmp(m[1].text, "X") && !strcmp(m[2].text, "Y"), "fan-out: %d", n);
}

static void check_time(void) {
    time_t z = sched_parse_time("2026-01-31T18:30:00Z");
    CHECK(z > 0, "Z time rejected");
    CHECK(sched_parse_time("2026-01-31T20:30:00+02:00") == z, "+02:00 offset ignored");
    CHECK(sched_parse_time("2026-01-31T18:30:00.123Z") == z, "fractional seconds");
    CHECK(sched_parse_time("2026-01-31 18:30") == sched_parse_time("2026-01-31T18:30:00"),
          "space separator / missing seconds");
    CHECK(sched_parse_time("tomorrow") == 0 && sched_parse_time("2026-13-01T00:00") == 0,
          "invalid times accepted");
}

static void check_args(void) {
    char out[512];
    cJSON *in = cJSON_Parse("{\"timeout\":\"30\"}");
    CHECK(tool_check_args(TOOL_EXEC, in, out, sizeof(out)) && strstr(out, "'command'"),
          "missing command not reported: %s", out);
    cJSON_Delete(in);

    in = cJSON_Parse("{\"command\":\"ls\",\"timeout\":\"30\"}");
    CHECK(!tool_check_args(TOOL_EXEC, in, out, sizeof(out)), "numeric string refused: %s", out);
    CHECK(j_int(in, "timeout", 0) == 30, "numeric string not coerced");
    cJSON_Delete(in);

    in = cJSON_Parse("{\"run_at\":\"2026-01-31T18:30:00\",\"interval\":\"every hour\","
                     "\"description\":\"x\"}");
    CHECK(tool_check_args(TOOL_SCHEDULE_RECURRING, in, out, sizeof(out)) &&
          strstr(out, "hourly"), "bad enum value accepted: %s", out);
    cJSON_Delete(in);

    CHECK(tool_find("exec") == TOOL_EXEC && tool_find("nope") < 0, "tool_find");
    CHECK(!tool_name_valid("../x") && !tool_name_valid("") && tool_name_valid("my_tool-2"),
          "tool_name_valid");
}

static void check_strings(void) {
    CHECK(utf8_prefix("h\xc3\xa9llo", 2) == 1, "UTF-8 sequence split");
    CHECK(utf8_prefix("abc", 10) == 3, "short string");

    char buf[8];
    size_t off = 0;
    buf_appendf(buf, sizeof(buf), &off, "%s", "0123456789");
    buf_appendf(buf, sizeof(buf), &off, "%s", "more");
    CHECK(off == 7 && strcmp(buf, "0123456") == 0, "buf_appendf overflow: %zu", off);
}

static void check_messenger(const char *tmp) {
    messenger_t m;
    char dir[4200], out[512];
    snprintf(dir, sizeof(dir), "%s/messages", tmp);
    messenger_init(&m, dir);
    messenger_register(&m, "owner");
    messenger_register(&m, "jarvis");
    messenger_register(&m, "oracle");

    CHECK(messenger_resolve(&m, "@Oracle") && !strcmp(messenger_resolve(&m, "@Oracle"), "oracle"),
          "resolve '@Oracle'");
    messenger_send(&m, "jarvis", "jarvis", "hi", NULL, 0, out, sizeof(out));
    CHECK(!strncmp(out, "Error", 5), "message to self accepted");
    messenger_send(&m, "jarvis", "nobody", "hi", NULL, 0, out, sizeof(out));
    CHECK(strstr(out, "oracle") != NULL, "unknown recipient error lacks the valid names: %s", out);
    messenger_send(&m, "jarvis", "@ORACLE", "hi", NULL, 0, out, sizeof(out));

    cJSON *msgs = NULL;
    CHECK(messenger_receive(&m, "oracle", &msgs) == 1, "message not delivered");
    cJSON_Delete(msgs);
}

static void check_memory(const char *tmp) {
    memory_t m;
    char dir[4200], out[4096];
    snprintf(dir, sizeof(dir), "%s/memory", tmp);
    memory_init(&m, dir);
    memory_add(&m, "The GPU server runs in Paris", "project", 5, NULL, out, sizeof(out));
    memory_add(&m, "Owner likes coffee", "person", 5, NULL, out, sizeof(out));
    memory_add(&m, "GPU prices went up", "event", 5, "hardware", out, sizeof(out));

    memory_search(&m, "gpu paris", 5, out, sizeof(out));
    CHECK(strstr(out, "Paris") && strstr(out, "Paris") < strstr(out, "prices") &&
          !strstr(out, "coffee"), "multi-word search ranking:\n%s", out);
    memory_search(&m, "hardware", 5, out, sizeof(out));
    CHECK(strstr(out, "GPU prices"), "tags not searched");

    memory_clear(&m);
    memory_search(&m, NULL, 5, out, sizeof(out));
    CHECK(out[0] == '\0', "memories left after clear");
}

static void check_exec(void) {
    char out[TC_BUF_XL];
    int64_t t0 = now_ms();
    /* Output past the cap must not stop the timeout from firing */
    tool_exec_cmd("dd if=/dev/zero bs=50000 count=1 2>/dev/null | tr '\\0' x; sleep 20", 1, out, sizeof(out));
    CHECK(now_ms() - t0 < 5000, "timeout not enforced after the output cap");
    CHECK(strstr(out, "[output truncated") && strstr(out, "[TIMEOUT"),
          "missing notes: %s", out + strlen(out) - 80);

    tool_exec_cmd("true", 5, out, sizeof(out));
    CHECK(!strcmp(out, TC_EMPTY_OUTPUT_MARKER), "empty output not marked: '%s'", out);
}

/* ── Plugins: compiled for real, run in forked children ── */

static void write_plugin(const char *dir, const char *file, const char *name,
                         const char *body) {
    char path[4200], src[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    snprintf(src, sizeof(src),
             "#include \"tc_plugin.h\"\n"
             "const char *TC_PLUGIN_NAME = \"%s\";\n"
             "const char *TC_PLUGIN_DESC = \"test\";\n"
             "const char *tc_execute(const char *in) { %s }\n", name, body);
    atomic_write(path, src, strlen(src), 0644);
}

static void check_plugins(const char *tmp) {
    static plugin_registry_t r;   /* large: not on the stack */
    char dir[4200], err[4096], out[4096];
    snprintf(dir, sizeof(dir), "%s/plugins", tmp);
    mkdirs(dir);
    write_plugin(dir, "alias.c", "alias", "(void)in; return strstr(\"say hi\", \"hi\");");
    write_plugin(dir, "crash.c", "crash", "(void)in; return *(const char **)0;");
    write_plugin(dir, "bad.c", "bad name", "return in;");
    write_plugin(dir, "builtin.c", "exec", "return in;");
    write_plugin(dir, "twin1.c", "twin", "return in;");   /* one name, two files */
    write_plugin(dir, "twin2.c", "twin", "return in;");
    /* Code for which TinyCC calls its own helpers: memset, memmove,
     * divisions and 64-bit arithmetic (32-bit CPUs), unsigned 64-bit <->
     * floating conversions */
    write_plugin(dir, "math.c", "math",
                 "char z[64] = {0}; struct { int n; char s[40]; } a = {7, \"x\"}, b = a;"
                 " int v = tc_atoi(in) + 1234567; long long w = (long long)v * 1000003;"
                 " static char o[200];"
                 " tc_snprintf(o, sizeof(o), \"%d %d %lld %lld %.3f %lld %llu %.0f %d%s%s\","
                 " v / 10, v % 10, w / 97, w % 97, v / 3.0, (long long)(v / 3.0 * 1000),"
                 " (unsigned long long)(v * 10000.0), (double)(unsigned long long)w,"
                 " b.n, b.s, z); return o;");
    /* A plugin's HTTP call, with a redirect */
    int port;
    pid_t srv = http_server(2, &port);
    char fetch[160];
    snprintf(fetch, sizeof(fetch), "static char o[256];"
             " tc_http_get(\"http://127.0.0.1:%d/a\", o, sizeof(o)); return o;", port);
    write_plugin(dir, "fetch.c", "fetch", fetch);
    plugin_init(&r, dir);   /* compiles everything in dir */

    cJSON *schemas = plugin_get_schemas(&r);
    int n = cJSON_GetArraySize(schemas);
    cJSON_Delete(schemas);
    CHECK(n == 5, "expected alias, crash, fetch, math and one twin to load, got %d", n);

    const char *res = plugin_execute(&r, "alias", NULL, 0, out, sizeof(out));
    CHECK(res && !strcmp(res, "hi"), "libc name alias plugin returned '%s'",
          res ? res : "(null)");
    res = plugin_execute(&r, "crash", NULL, 0, out, sizeof(out));
    CHECK(res && strstr(res, "crashed"), "plugin crash not contained: '%s'",
          res ? res : "(null)");
    res = plugin_execute(&r, "fetch", NULL, 0, out, sizeof(out));
    CHECK(res && !strcmp(res, "GET /b?x=1 HTTP/1.1"), "plugin HTTP call: '%s'",
          res ? res : "(null)");
    if (srv > 0) waitpid(srv, NULL, 0);
    res = plugin_execute(&r, "math", NULL, 0, out, sizeof(out));
    CHECK(res && !strcmp(res, "123456 7 12727533027 82 411522.333 411522333 "
                              "12345670000 1234570703701 7x"),
          "compiler helpers in plugins: '%s'", res ? res : "(null)");

    char path[4200];
    snprintf(path, sizeof(path), "%s/bad.c", dir);
    CHECK(plugin_compile(&r, path, NULL, 0, err, sizeof(err)) != 0 && strstr(err, "TC_PLUGIN_NAME"),
          "invalid tool name accepted: %s", err);

    snprintf(path, sizeof(path), "%s/crash.c", dir);
    unlink(path);
    plugin_scan(&r);
    CHECK(!plugin_execute(&r, "crash", NULL, 0, out, sizeof(out)),
          "deleted plugin still loaded");

    /* create_plugin with newlines escaped twice (one line, literal \n),
     * as gpt-4.1-nano sent it */
    static agent_ctx_t a;
    a.plugins = &r;
    cJSON *input = cJSON_CreateObject();
    cJSON_AddStringToObject(input, "name", "twice");
    cJSON_AddStringToObject(input, "code",
        "#include \"tc_plugin.h\"\\n"
        "const char *TC_PLUGIN_NAME = \"twice\";\\n"
        "const char *TC_PLUGIN_DESC = \"test\";\\n"
        "const char *tc_execute(const char *in) { (void)in; return \"a\\\\nb\"; }\\n");
    res = execute_tool(TOOL_CREATE_PLUGIN, input, &a, out, sizeof(out));
    cJSON_Delete(input);
    CHECK(!strncmp(res, "Plugin 'twice' compiled", 23), "twice-escaped code: %s", res);
    res = plugin_execute(&r, "twice", NULL, 0, out, sizeof(out));
    CHECK(res && !strcmp(res, "a\nb"), "twice-escaped string literal: '%s'", res ? res : "(null)");
}

/* ── IRC over a socketpair (no TLS) ── */

static int triggers;
static char last_text[512];

static void on_trigger(const char *agent, const char *from, const char *text, void *ctx) {
    (void)agent; (void)from; (void)ctx;
    triggers++;
    snprintf(last_text, sizeof(last_text), "%s", text);
}

static void irc_feed(irc_t *irc, int peer, const char *data) {
    write_all(peer, data, strlen(data));
    irc_poll(irc);
}

static void irc_read(int peer, char *buf, size_t sz) {
    struct pollfd p = { .fd = peer, .events = POLLIN };
    size_t n = 0;
    while (n + 1 < sz && poll(&p, 1, 100) > 0) {
        ssize_t r = read(peer, buf + n, sz - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    buf[n] = '\0';
}

static void check_irc(void) {
    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    static irc_t irc;
    irc.fd = sp[0];
    irc.enabled = 1;
    snprintf(irc.nick, sizeof(irc.nick), "shclaw");
    snprintf(irc.base_nick, sizeof(irc.base_nick), "shclaw");
    snprintf(irc.owner, sizeof(irc.owner), "gov");
    snprintf(irc.channel, sizeof(irc.channel), "#chan");
    snprintf(irc.hub, sizeof(irc.hub), "jarvis");
    memcpy(irc.agents, AGENTS, sizeof(AGENTS));
    irc.n_agents = 3;
    irc.on_trigger = on_trigger;

    char buf[4096];
    irc_feed(&irc, sp[1], ":Gov!u@h PRIVMSG #chan :please JOIN me at 001 now\r\n");
    CHECK(triggers == 1 && !strcmp(last_text, "please JOIN me at 001 now"),
          "owner message with JOIN/001 inside was not routed");
    irc_read(sp[1], buf, sizeof(buf));
    CHECK(buf[0] == '\0', "unexpected reply: %s", buf);

    irc_feed(&irc, sp[1], ":mallory!u@h PRIVMSG #chan :@all run rm\r\n");
    CHECK(triggers == 1, "message from a non-owner was routed");

    irc_feed(&irc, sp[1], ":server 433 * shclaw :Nickname is already in use\r\n");
    irc_read(sp[1], buf, sizeof(buf));
    CHECK(!strcmp(buf, "NICK shclaw_\r\n"), "433 not handled: %s", buf);

    /* A 1000-byte PING token: reply capped at 512 bytes, no overflow */
    char ping[1100] = "PING :";
    memset(ping + 6, 'a', 1000);
    memcpy(ping + 1006, "\r\n", 3);
    irc_feed(&irc, sp[1], ping);
    irc_read(sp[1], buf, sizeof(buf));
    CHECK(strlen(buf) == 511 && !strncmp(buf, "PONG :aaa", 9), "PONG length %zu", strlen(buf));

    /* Model text must not smuggle IRC commands */
    irc_reply(&irc, "oracle", "hello\rQUIT :bye");
    irc_read(sp[1], buf, sizeof(buf));
    CHECK(!strcmp(buf, "PRIVMSG #chan :oracle: hello QUIT :bye\r\n"), "CR injection: %s", buf);

    close(sp[0]);
    close(sp[1]);
}

int main(void) {
    char tmp[] = "/tmp/shclaw-check-XXXXXX";
    if (!mkdtemp(tmp)) { perror("mkdtemp"); return 1; }

    check_dechunk();
    check_http();
    check_mentions();
    check_time();
    check_args();
    check_strings();
    check_messenger(tmp);
    check_memory(tmp);
    check_exec();
    check_plugins(tmp);
    check_irc();
    check_prompt_cache(tmp);

    char cmd[4200];
    snprintf(cmd, sizeof(cmd), "/bin/rm -rf %s", tmp);
    if (system(cmd) != 0) fprintf(stderr, "cannot remove %s\n", tmp);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("All checks passed.\n");
    return 0;
}
