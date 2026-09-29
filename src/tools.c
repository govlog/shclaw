/*
 * tools.c — built-in tool definitions + JSON schema + dispatch
 */

#include "../include/tc.h"
#ifdef __COSMOPOLITAN__
#include <cosmo.h>
#endif

#define INTERVALS "every_30min,hourly,every_6h,every_12h,daily,weekly"
#define RUN_AT_DESC "Or a local time, YYYY-MM-DDTHH:MM:SS (e.g. 2026-01-31T18:30:00). " \
                    "Append Z or +HH:MM only for another time zone."
#define IN_MINUTES_DESC "Minutes from now: use this for 'in 5 minutes', 'in 2 hours' (120)"

static const tool_def_t TOOLS[] = {
    [TOOL_EXEC] = {
        .name = "exec",
        .desc = "Run a shell command (/bin/sh -c). Returns stdout+stderr (max 10KB). Timeout 30s.",
        .params = {
            {"command", TC_STRING, "Shell command to execute", 1, NULL},
            {"timeout", TC_INT,    "Timeout in seconds (default 30)", 0, NULL},
            {0}
        }
    },
    [TOOL_READ_FILE] = {
        .name = "read_file",
        .desc = "Read a file's contents. Returns up to 32KB; use offset to read more.",
        .params = {
            {"path",   TC_STRING, "File path", 1, NULL},
            {"offset", TC_INT,    "Start at byte offset (default 0)", 0, NULL},
            {"limit",  TC_INT,    "Max bytes to read (default 32768)", 0, NULL},
            {0}
        }
    },
    [TOOL_WRITE_FILE] = {
        .name = "write_file",
        .desc = "Write or append to a file. Creates parent dirs if needed.",
        .params = {
            {"path",    TC_STRING, "File path", 1, NULL},
            {"content", TC_STRING, "Content to write", 1, NULL},
            {"append",  TC_BOOL,   "Append instead of overwrite (default false)", 0, NULL},
            {0}
        }
    },
    [TOOL_SCHEDULE_TASK] = {
        .name = "schedule_task",
        .desc = "Schedule a one-shot task, in N minutes or at a given time.",
        .params = {
            {"in_minutes",  TC_INT,    IN_MINUTES_DESC, 0, NULL},
            {"run_at",      TC_STRING, RUN_AT_DESC, 0, NULL},
            {"description", TC_STRING, "What to do when it fires", 1, NULL},
            {"prompt",      TC_STRING, "Detailed prompt for the session", 0, NULL},
            {0}
        }
    },
    [TOOL_SCHEDULE_RECURRING] = {
        .name = "schedule_recurring",
        .desc = "Schedule a recurring task.",
        .params = {
            {"in_minutes",  TC_INT,    "First run. " IN_MINUTES_DESC, 0, NULL},
            {"run_at",      TC_STRING, "First run. " RUN_AT_DESC, 0, NULL},
            {"interval",    TC_STRING, "Recurrence interval", 1, INTERVALS},
            {"description", TC_STRING, "What to do each time", 1, NULL},
            {"prompt",      TC_STRING, "Detailed prompt for each session", 0, NULL},
            {0}
        }
    },
    [TOOL_LIST_TASKS] = {
        .name = "list_tasks",
        .desc = "List all scheduled tasks (upcoming first).",
        .params = {{0}}
    },
    [TOOL_UPDATE_TASK] = {
        .name = "update_task",
        .desc = "Update an existing scheduled task.",
        .params = {
            {"task_id",     TC_STRING, "Task ID to update", 1, NULL},
            {"run_at",      TC_STRING, "New run time. " RUN_AT_DESC, 0, NULL},
            {"description", TC_STRING, "New description", 0, NULL},
            {"prompt",      TC_STRING, "New prompt", 0, NULL},
            {"interval",    TC_STRING, "New interval (\"\" = one-shot)", 0, NULL},
            {0}
        }
    },
    [TOOL_CANCEL_TASK] = {
        .name = "cancel_task",
        .desc = "Cancel a scheduled task by ID.",
        .params = {
            {"task_id", TC_STRING, "Task ID to cancel", 1, NULL},
            {0}
        }
    },
    [TOOL_REMEMBER] = {
        .name = "remember",
        .desc = "Save a memory. Auto-tagged for later retrieval.",
        .params = {
            {"content",    TC_STRING, "What to remember", 1, NULL},
            {"category",   TC_STRING, "Category", 0, "general,project,person,event,error"},
            {"importance", TC_INT,    "1-10, default 5. 8+ = critical.", 0, NULL},
            {"tags",       TC_STRING, "Comma-separated extra tags", 0, NULL},
            {0}
        }
    },
    [TOOL_RECALL] = {
        .name = "recall",
        .desc = "Search memories by keywords. Empty query = most recent.",
        .params = {
            {"query", TC_STRING, "Keywords", 0, NULL},
            {"n",     TC_INT,    "Max results (default 20)", 0, NULL},
            {0}
        }
    },
    [TOOL_SET_FACT] = {
        .name = "set_fact",
        .desc = "Store a permanent key-value fact, always shown in your prompt.",
        .params = {
            {"key",   TC_STRING, "Fact key", 1, NULL},
            {"value", TC_STRING, "Fact value", 1, NULL},
            {0}
        }
    },
    [TOOL_GET_FACT] = {
        .name = "get_fact",
        .desc = "Retrieve a stored fact by key. Empty key = list all.",
        .params = {
            {"key", TC_STRING, "Fact key (empty = list all)", 0, NULL},
            {0}
        }
    },
    [TOOL_SEND_MESSAGE] = {
        .name = "send_message",
        .desc = "Send a message to another agent, 'owner' (IRC), or 'all' (broadcast).",
        .params = {
            {"to",      TC_STRING, "Recipient: agent name, 'owner', or 'all'", 1, NULL},
            {"content", TC_STRING, "Message content", 1, NULL},
            {0}
        }
    },
    [TOOL_LIST_AGENTS] = {
        .name = "list_agents",
        .desc = "List all agents with their status and specialty.",
        .params = {{0}}
    },
    [TOOL_CREATE_PLUGIN] = {
        .name = "create_plugin",
        .desc = "Create a single-file C plugin. Source must include only "
                "\"tc_plugin.h\" and export TC_PLUGIN_NAME, TC_PLUGIN_DESC, "
                "TC_PLUGIN_SCHEMA, and tc_execute(const char *input_json). "
                "Compilation errors are returned for retry.",
        .params = {
            {"name",        TC_STRING, "Plugin name, letters/digits/_ (e.g. 'weather')", 1, NULL},
            {"code",        TC_STRING, "Complete C source code", 1, NULL},
            {"test_input",  TC_STRING, "Optional JSON object to run the plugin with "
                                       "once compiled, e.g. {\"city\":\"Paris\"}", 0, NULL},
            {0}
        }
    },
    [TOOL_CLEAR_MEMORY] = {
        .name = "clear_memory",
        .desc = "Clear an agent's memories and/or facts. Use agent='all' to clear every agent.",
        .params = {
            {"agent",  TC_STRING, "Agent name, or 'all' for every agent", 1, NULL},
            {"what",   TC_STRING, "What to clear (default 'both')", 0, "memory,facts,both"},
            {0}
        }
    },
};

static const char *type_to_json_str(int type) {
    switch (type) {
    case TC_INT:   return "integer";
    case TC_BOOL:  return "boolean";
    case TC_FLOAT: return "number";
    default:       return "string";
    }
}

int tool_find(const char *name) {
    for (int i = 0; name && i < TOOL_COUNT; i++)
        if (TOOLS[i].name && strcmp(TOOLS[i].name, name) == 0)
            return i;
    return -1;
}

int tool_name_valid(const char *name) {
    size_t n = 0;
    for (; name[n]; n++) {
        char c = name[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return n > 0 && n <= 64;
}

cJSON *tools_to_json(int is_builder) {
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < TOOL_COUNT; i++) {
        /* create_plugin: skip in no-plugin builds, or on unsupported platforms */
        if (i == TOOL_CREATE_PLUGIN) {
#ifdef TC_NO_PLUGINS
            continue;
#elif defined(__COSMOPOLITAN__)
            if (!is_builder || IsWindows()) continue;
#else
            if (!is_builder) continue;
#endif
        }

        /* Builder agents: only essential tools */
        if (is_builder && i != TOOL_READ_FILE && i != TOOL_EXEC &&
            i != TOOL_CREATE_PLUGIN && i != TOOL_SEND_MESSAGE)
            continue;

        const tool_def_t *t = &TOOLS[i];
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", t->name);
        cJSON_AddStringToObject(tool, "description", t->desc);

        cJSON *schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");

        cJSON *props = cJSON_CreateObject();
        cJSON *required = cJSON_CreateArray();

        for (int j = 0; j < TC_MAX_PARAMS && t->params[j].name; j++) {
            const tc_param_t *p = &t->params[j];
            cJSON *prop = cJSON_CreateObject();
            cJSON_AddStringToObject(prop, "type", type_to_json_str(p->type));
            cJSON_AddStringToObject(prop, "description", p->description);
            if (p->choices) {
                /* An enum is the surest way to get a valid value */
                cJSON *choices = cJSON_AddArrayToObject(prop, "enum");
                char buf[TC_BUF_SM];
                snprintf(buf, sizeof(buf), "%s", p->choices);
                char *save = NULL;
                for (char *c = strtok_r(buf, ",", &save); c; c = strtok_r(NULL, ",", &save))
                    cJSON_AddItemToArray(choices, cJSON_CreateString(c));
            }
            cJSON_AddItemToObject(props, p->name, prop);
            if (p->required)
                cJSON_AddItemToArray(required, cJSON_CreateString(p->name));
        }

        cJSON_AddItemToObject(schema, "properties", props);
        cJSON_AddItemToObject(schema, "required", required);
        cJSON_AddItemToObject(tool, "input_schema", schema);

        cJSON_AddItemToArray(arr, tool);
    }

    return arr;
}

/* Small models often get arguments wrong: say exactly what is expected */
const char *tool_check_args(int tool_id, cJSON *input, char *out, size_t out_sz) {
    const tool_def_t *t = &TOOLS[tool_id];
    char problem[TC_BUF_SM] = "";

    for (int j = 0; j < TC_MAX_PARAMS && t->params[j].name && !problem[0]; j++) {
        const tc_param_t *p = &t->params[j];
        cJSON *v = cJSON_GetObjectItem(input, p->name);
        int ok;
        if (!v || cJSON_IsNull(v)) {
            ok = !p->required;
        } else if (p->type == TC_STRING) {
            ok = cJSON_IsString(v);
            if (ok && p->choices && v->valuestring[0]) {
                char pat[TC_BUF_SM + 2], val[TC_BUF_SM];
                snprintf(pat, sizeof(pat), ",%s,", p->choices);
                snprintf(val, sizeof(val), ",%s,", v->valuestring);
                ok = strstr(pat, val) != NULL;
            }
        } else if (p->type == TC_INT) {
            ok = j_int(input, p->name, INT32_MIN) != INT32_MIN;
        } else {
            ok = j_bool(input, p->name, -1) != -1;
        }
        if (!ok)
            snprintf(problem, sizeof(problem), "%s '%s'",
                     v ? "invalid value for" : "missing required parameter", p->name);
    }
    if (!problem[0])
        return NULL;

    size_t off = 0;
    buf_appendf(out, out_sz, &off, "Error: %s. Usage: %s(", problem, t->name);
    for (int j = 0; j < TC_MAX_PARAMS && t->params[j].name; j++) {
        const tc_param_t *p = &t->params[j];
        buf_appendf(out, out_sz, &off, "%s%s: %s%s%s%s", j ? ", " : "", p->name,
                    type_to_json_str(p->type),
                    p->choices ? " one of " : "", p->choices ? p->choices : "",
                    p->required ? ", required" : "");
    }
    buf_appendf(out, out_sz, &off, ")");
    return out;
}

const char *agent_message(agent_ctx_t *ctx, const char *to, const char *content,
                          const char *thread_id, int reply, char *out, size_t out_sz) {
    const char *name = messenger_resolve(ctx->messenger, to);
    if (name && strcmp(name, "owner") == 0) {
        irc_reply(ctx->irc, ctx->name, content);
        snprintf(out, out_sz, "Sent to owner.");
        return out;
    }
    messenger_send(ctx->messenger, ctx->name, to, content, thread_id, reply, out, out_sz);
    if (strncmp(out, "Error", 5) != 0) {
        /* Inter-agent traffic is visible on IRC/TUI */
        char prefix[96];
        snprintf(prefix, sizeof(prefix), "%s => %s", ctx->name, name ? name : to);
        irc_reply(ctx->irc, prefix, content);
    }
    return out;
}

/* The agent's own context, or another agent's by name */
static agent_ctx_t *find_peer(agent_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->n_peers; i++)
        if (strcasecmp(ctx->peers[i].name, name) == 0)
            return &ctx->peers[i];
    return NULL;
}

/* TinyCC reports "file:LINE: error: ..."; small models fix code much
 * better when they also see the line itself. */
static void annotate_errors(const char *code, const char *err,
                            char *out, size_t out_sz, size_t *off) {
    int shown = 0;
    for (const char *l = err; *l && shown < 12; shown++) {
        const char *eol = strchr(l, '\n');
        int len = eol ? (int)(eol - l) : (int)strlen(l);
        buf_appendf(out, out_sz, off, "%.*s\n", len, l);

        const char *colon = memchr(l, ':', (size_t)len);
        int line_no = colon ? atoi(colon + 1) : 0;
        const char *src = code;
        for (int n = 1; line_no > 0 && src && n < line_no; n++) {
            src = strchr(src, '\n');
            if (src) src++;
        }
        if (line_no > 0 && src && *src) {
            const char *send = strchr(src, '\n');
            int slen = send ? (int)(send - src) : (int)strlen(src);
            buf_appendf(out, out_sz, off, "    %d | %.*s\n", line_no,
                        utf8_prefix(src, slen < 200 ? slen : 200), src);
        }
        l = eol ? eol + 1 : l + len;
    }
}

/* Run a freshly built plugin once so the builder sees what it returns.
 * A crash, a hang, or an error or empty answer to a valid input is a
 * failed test: returns -1. */
static int test_plugin(agent_ctx_t *ctx, const char *tool, const char *test,
                       char *out, size_t out_sz, size_t *off) {
    cJSON *targs = cJSON_Parse(test);
    static __thread char run[TC_BUF_XL / 2];   /* HTTP trace + plugin output */
    const char *res = cJSON_IsObject(targs)
        ? plugin_execute(ctx->plugins, tool, targs, 1, run, sizeof(run))
        : "Error: test_input is not a JSON object";
    cJSON_Delete(targs);
    if (!res) res = "Error: plugin not loaded";

    /* The test child writes one trace line per HTTP call, then the output */
    const char *output = res;
    while (!strncmp(output, "[GET ", 5) || !strncmp(output, "[POST ", 6)) {
        const char *nl = strchr(output, '\n');
        output = nl ? nl + 1 : output + strlen(output);
    }
    int trace_len = (int)(output - res);
    while (*output == ' ' || *output == '\n' || *output == '\r')
        output++;
    int failed = !strncmp(res, "Error:", 6) || !strncasecmp(output, "error", 5) ||
                 !*output || !strcmp(output, TC_EMPTY_OUTPUT_MARKER);

    buf_appendf(out, out_sz, off, "\nTest run with %s", test);
    if (trace_len)
        buf_appendf(out, out_sz, off, "\nHTTP calls (debug trace, not part of the output):\n%.*s",
                    trace_len, res);
    buf_appendf(out, out_sz, off, "\nPlugin output:\n%s\n%s", *output ? output : "(empty)",
                failed ? "Fix the plugin and call create_plugin again."
                       : "If this is not what the plugin should answer, fix it "
                         "and call create_plugin again.");
    return failed ? -1 : 0;
}

/* Decode \n \t \r \" and \\ in place; other characters are kept */
static void unescape_once(char *s) {
    char *w = s;
    for (const char *r = s; *r; r++) {
        char c = *r;
        if (c == '\\') {
            switch (r[1]) {
            case 'n':  c = '\n'; r++; break;
            case 't':  c = '\t'; r++; break;
            case 'r':  c = '\r'; r++; break;
            case '"':  c = '"';  r++; break;
            case '\\': c = '\\'; r++; break;
            }
        }
        *w++ = c;
    }
    *w = '\0';
}

static void clear_agent(agent_ctx_t *a, int mem, int facts) {
    if (mem) memory_clear(&a->memory);
    if (facts) facts_clear(&a->memory);
}

const char *execute_tool(int tool_id, cJSON *input, agent_ctx_t *ctx,
                         char *out, size_t out_sz) {
    switch (tool_id) {

    case TOOL_EXEC:
        return tool_exec_cmd(j_str(input, "command"),
                             j_int(input, "timeout", TC_TOOL_TIMEOUT), out, out_sz);

    case TOOL_READ_FILE: {
        const char *path = j_str(input, "path");
        long offset = j_int(input, "offset", 0);
        long limit = j_int(input, "limit", TC_BUF_XL);
        if (offset < 0) { snprintf(out, out_sz, "Error: offset must be >= 0"); return out; }
        if (limit <= 0) { snprintf(out, out_sz, "Error: limit must be > 0"); return out; }

        FILE *f = fopen(path, "r");
        if (!f) { snprintf(out, out_sz, "Error: cannot open %s: %s", path, strerror(errno)); return out; }
        if (offset > 0 && fseek(f, offset, SEEK_SET) != 0) {
            fclose(f);
            snprintf(out, out_sz, "Error: cannot seek %s", path);
            return out;
        }
        if (limit > TC_BUF_XL) limit = TC_BUF_XL;
        if ((size_t)limit > out_sz - 160) limit = (long)out_sz - 160;
        size_t rd = fread(out, 1, (size_t)limit, f);
        out[rd] = '\0';
        int more = fgetc(f) != EOF;
        fclose(f);
        if (strlen(out) < rd)
            snprintf(out, out_sz, "Error: %s is a binary file", path);
        else if (more)
            buf_appendf(out, out_sz, &rd, "\n[truncated: call read_file with offset=%ld for more]",
                        offset + (long)rd);
        else if (rd == 0)
            snprintf(out, out_sz, "%s", TC_EMPTY_OUTPUT_MARKER);
        return out;
    }

    case TOOL_WRITE_FILE: {
        const char *path = j_str(input, "path");
        const char *content = j_str(input, "content");
        int ok;

        mkdirs_for(path);
        if (j_bool(input, "append", 0)) {
            FILE *f = fopen(path, "a");
            ok = f && fputs(content, f) != EOF;
            if (f && fclose(f) != 0) ok = 0;
        } else {
            ok = atomic_write(path, content, strlen(content), 0644) == 0;
        }
        if (!ok)
            snprintf(out, out_sz, "Error: cannot write %s: %s", path, strerror(errno));
        else
            snprintf(out, out_sz, "Written %zu bytes to %s", strlen(content), path);
        return out;
    }

    case TOOL_SCHEDULE_TASK:
    case TOOL_SCHEDULE_RECURRING: {
        /* A delay is far less error-prone for small models than a date */
        const char *run_at = j_str(input, "run_at");
        char when[32];
        int minutes = j_int(input, "in_minutes", 0);
        if (minutes > 0) {
            format_time(time(NULL) + (time_t)minutes * 60, 0, when, sizeof(when));
            run_at = when;
        } else if (!run_at || !run_at[0]) {
            snprintf(out, out_sz, "Error: give in_minutes (e.g. 5) or run_at");
            return out;
        }
        return sched_add(&ctx->scheduler, run_at,
                         j_str(input, "description"), j_str(input, "prompt"),
                         tool_id == TOOL_SCHEDULE_RECURRING ? j_str(input, "interval") : NULL,
                         out, out_sz);
    }

    case TOOL_LIST_TASKS:
        return sched_list(&ctx->scheduler, out, out_sz);

    case TOOL_UPDATE_TASK:
        return sched_update(&ctx->scheduler, j_str(input, "task_id"),
                            j_str(input, "run_at"), j_str(input, "description"),
                            j_str(input, "prompt"), j_str(input, "interval"),
                            out, out_sz);

    case TOOL_CANCEL_TASK:
        return sched_cancel(&ctx->scheduler, j_str(input, "task_id"), out, out_sz);

    case TOOL_REMEMBER:
        return memory_add(&ctx->memory, j_str(input, "content"), j_str(input, "category"),
                          j_int(input, "importance", 5), j_str(input, "tags"),
                          out, out_sz);

    case TOOL_RECALL:
        return memory_search(&ctx->memory, j_str(input, "query"),
                             j_int(input, "n", 20), out, out_sz);

    case TOOL_SET_FACT:
        return facts_set(&ctx->memory, j_str(input, "key"), j_str(input, "value"),
                         out, out_sz);

    case TOOL_GET_FACT:
        return facts_get(&ctx->memory, j_str(input, "key"), out, out_sz);

    case TOOL_SEND_MESSAGE:
        return agent_message(ctx, j_str(input, "to"), j_str(input, "content"),
                             NULL, 0, out, out_sz);

    case TOOL_LIST_AGENTS: {
        size_t off = 0;
        buf_appendf(out, out_sz, &off, "Agents:\n");
        for (int i = 0; i < ctx->n_peers; i++) {
            agent_ctx_t *a = &ctx->peers[i];
            buf_appendf(out, out_sz, &off, "- %s%s%s%s%s\n", a->name,
                        a == ctx ? " (you)" : "",
                        a->specialty[0] ? " — " : "", a->specialty,
                        a != ctx && __atomic_load_n(&a->busy, __ATOMIC_RELAXED)
                            ? " [busy]" : "");
        }
        return out;
    }

    case TOOL_CLEAR_MEMORY: {
        const char *agent = j_str(input, "agent");
        const char *what = j_str(input, "what");
        if (!what || !what[0]) what = "both";
        int mem = strcmp(what, "facts") != 0;
        int facts = strcmp(what, "memory") != 0;

        if (strcmp(agent, "all") == 0) {
            for (int i = 0; i < ctx->n_peers; i++)
                clear_agent(&ctx->peers[i], mem, facts);
            snprintf(out, out_sz, "Cleared %s for %d agents.", what, ctx->n_peers);
            return out;
        }
        agent_ctx_t *a = find_peer(ctx, agent);
        if (!a) {
            snprintf(out, out_sz, "Error: unknown agent '%s'", agent);
            return out;
        }
        clear_agent(a, mem, facts);
        snprintf(out, out_sz, "Cleared %s for %s.", what, a->name);
        return out;
    }

    case TOOL_CREATE_PLUGIN: {
        char name[80];
        const char *code = j_str(input, "code");
        /* Weak models sometimes escape the code twice. One line with a
         * literal "\n" cannot be C: decode the escapes once more. */
        if (code && !strchr(code, '\n') && strstr(code, "\\n"))
            unescape_once(cJSON_GetObjectItem(input, "code")->valuestring);
        snprintf(name, sizeof(name), "%s", j_str(input, "name"));
        size_t nlen = strlen(name);
        if (nlen > 2 && strcmp(name + nlen - 2, ".c") == 0)
            name[nlen - 2] = '\0';   /* "weather.c" means "weather" */
        if (!tool_name_valid(name)) {
            snprintf(out, out_sz, "Error: invalid plugin name '%s' "
                     "(letters, digits, '_' and '-' only)", name);
            return out;
        }

        char src_path[4200];
        snprintf(src_path, sizeof(src_path), "%s/%s.c", ctx->plugins->dir, name);

        /* Build from memory first: a failed attempt must not replace the
         * source of a working plugin on disk */
        time_t mtime = time(NULL);
        char err[TC_BUF_LG];
        size_t off = 0;
        if (plugin_compile(ctx->plugins, src_path, code, mtime, err, sizeof(err)) != 0) {
            buf_appendf(out, out_sz, &off, "Error: compilation failed for %s.c:\n", name);
            annotate_errors(code, err, out, out_sz, &off);
            if (strstr(err, "unresolved reference") || strstr(err, "implicit declaration"))
                buf_appendf(out, out_sz, &off, "Only these functions exist: %s\n",
                            plugin_api_names());
            if (strstr(err, "defined twice") || strstr(err, "redefinition"))
                buf_appendf(out, out_sz, &off, "One of your functions has the name of a "
                            "provided one: rename it (e.g. my_isdigit).\n");
            buf_appendf(out, out_sz, &off,
                        "Hint: include only \"tc_plugin.h\", use only its tc_* functions, "
                        "and export TC_PLUGIN_NAME, TC_PLUGIN_DESC, TC_PLUGIN_SCHEMA "
                        "and tc_execute(const char *input_json).");
            return out;
        }

        /* Saved with the mtime the registry knows, so the scanner skips it */
        struct timeval tv[2] = { { .tv_sec = mtime }, { .tv_sec = mtime } };
        if (atomic_write(src_path, code, strlen(code), 0644) != 0 || utimes(src_path, tv) != 0) {
            snprintf(out, out_sz, "Error: plugin loaded, but %s cannot be saved: %s",
                     src_path, strerror(errno));
            return out;
        }

        /* err now holds the plugin (tool) name */
        const char *test = j_str(input, "test_input");
        char *report = calloc(1, TC_BUF_XL);
        size_t roff = 0;
        int failed = report && test && test[0] &&
                     test_plugin(ctx, err, test, report, TC_BUF_XL, &roff) != 0;
        if (failed)
            snprintf(out, out_sz, "Error: %s.c compiled, but its test run failed.%s",
                     name, report);
        else
            snprintf(out, out_sz, "Plugin '%s' compiled and loaded as tool '%s'.%s",
                     name, err, report ? report : "");
        free(report);
        return out;
    }

    default:
        snprintf(out, out_sz, "Error: unknown tool %d", tool_id);
        return out;
    }
}
