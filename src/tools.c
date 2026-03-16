/*
 * tools.c — built-in tool definitions + JSON schema + dispatch
 */

#include "../include/tc.h"
#ifdef __COSMOPOLITAN__
#include <cosmo.h>
#endif

static const tool_def_t TOOLS[] = {
    [TOOL_EXEC] = {
        .name = "exec",
        .desc = "Execute a shell command. Returns stdout+stderr (max 10KB). Timeout 30s.",
        .params = {
            {"command", TC_STRING, "Shell command to execute", 1},
            {"timeout", TC_INT,    "Timeout in seconds (default 30)", 0},
            {0}
        }
    },
    [TOOL_READ_FILE] = {
        .name = "read_file",
        .desc = "Read a file's contents. Returns up to 32KB.",
        .params = {
            {"path",   TC_STRING, "File path", 1},
            {"offset", TC_INT,    "Start at byte offset (default 0)", 0},
            {"limit",  TC_INT,    "Max bytes to read (default 32768)", 0},
            {0}
        }
    },
    [TOOL_WRITE_FILE] = {
        .name = "write_file",
        .desc = "Write or append to a file. Creates parent dirs if needed.",
        .params = {
            {"path",    TC_STRING, "File path", 1},
            {"content", TC_STRING, "Content to write", 1},
            {"append",  TC_BOOL,   "Append instead of overwrite (default false)", 0},
            {0}
        }
    },
    [TOOL_SCHEDULE_TASK] = {
        .name = "schedule_task",
        .desc = "Schedule a one-shot task at a specific time.",
        .params = {
            {"run_at",      TC_STRING, "ISO 8601 datetime. Use local time (no Z suffix) unless user specifies UTC.", 1},
            {"description", TC_STRING, "What to do when it fires", 1},
            {"prompt",      TC_STRING, "Detailed prompt for the session", 0},
            {0}
        }
    },
    [TOOL_SCHEDULE_RECURRING] = {
        .name = "schedule_recurring",
        .desc = "Schedule a recurring task. Intervals: every_30min, hourly, every_6h, every_12h, daily, weekly.",
        .params = {
            {"run_at",      TC_STRING, "First run: ISO 8601 datetime. Use local time (no Z suffix) unless user specifies UTC.", 1},
            {"interval",    TC_STRING, "Recurrence interval", 1},
            {"description", TC_STRING, "What to do each time", 1},
            {"prompt",      TC_STRING, "Detailed prompt for each session", 0},
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
            {"task_id",     TC_STRING, "Task ID to update", 1},
            {"run_at",      TC_STRING, "New run time (ISO 8601)", 0},
            {"description", TC_STRING, "New description", 0},
            {"prompt",      TC_STRING, "New prompt", 0},
            {"interval",    TC_STRING, "New interval (empty = one-shot)", 0},
            {0}
        }
    },
    [TOOL_CANCEL_TASK] = {
        .name = "cancel_task",
        .desc = "Cancel a scheduled task by ID.",
        .params = {
            {"task_id", TC_STRING, "Task ID to cancel", 1},
            {0}
        }
    },
    [TOOL_REMEMBER] = {
        .name = "remember",
        .desc = "Save a memory. Auto-tagged for later retrieval.",
        .params = {
            {"content",    TC_STRING, "What to remember", 1},
            {"category",   TC_STRING, "Category: general, project, person, event, error", 0},
            {"importance", TC_INT,    "1-10, default 5. 8+ = critical.", 0},
            {"tags",       TC_STRING, "Comma-separated extra tags", 0},
            {0}
        }
    },
    [TOOL_RECALL] = {
        .name = "recall",
        .desc = "Search memories. Empty query = recent.",
        .params = {
            {"query", TC_STRING, "Search query", 0},
            {"n",     TC_INT,    "Max results (default 20)", 0},
            {0}
        }
    },
    [TOOL_SET_FACT] = {
        .name = "set_fact",
        .desc = "Store a permanent key-value fact. Survives memory pruning.",
        .params = {
            {"key",   TC_STRING, "Fact key", 1},
            {"value", TC_STRING, "Fact value", 1},
            {0}
        }
    },
    [TOOL_GET_FACT] = {
        .name = "get_fact",
        .desc = "Retrieve a stored fact by key. Empty key = list all.",
        .params = {
            {"key", TC_STRING, "Fact key (empty = list all)", 0},
            {0}
        }
    },
    [TOOL_SEND_MESSAGE] = {
        .name = "send_message",
        .desc = "Send a message to another agent, 'owner' (IRC), or 'all' (broadcast).",
        .params = {
            {"to",      TC_STRING, "Recipient: agent name, 'owner', or 'all'", 1},
            {"content", TC_STRING, "Message content", 1},
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
                "Read include/tc_plugin.h or plugins/_template.c first if "
                "needed. You can pass test_input_json and expected_output "
                "to self-test the plugin immediately. Compilation and "
                "self-test errors are returned for retry.",
        .params = {
            {"name",        TC_STRING, "Plugin name (e.g. 'weather')", 1},
            {"code",        TC_STRING, "Complete C source code", 1},
            {"test_input_json", TC_STRING, "Optional JSON object string for an immediate self-test", 0},
            {"expected_output", TC_STRING, "Optional exact expected output for the self-test", 0},
            {0}
        }
    },
    [TOOL_CLEAR_MEMORY] = {
        .name = "clear_memory",
        .desc = "Clear an agent's memories and/or facts. Use agent='all' to clear every agent.",
        .params = {
            {"agent",  TC_STRING, "Agent name, or 'all' for every agent", 1},
            {"what",   TC_STRING, "What to clear: 'memory', 'facts', or 'both' (default 'both')", 0},
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

        const tool_def_t *t = &TOOLS[i];
        if (!t->name) continue;

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

/* Helper to get string from cJSON input */
static const char *j_str(cJSON *input, const char *key) {
    if (!input) return NULL;
    cJSON *item = cJSON_GetObjectItem(input, key);
    return (item && cJSON_IsString(item)) ? item->valuestring : NULL;
}

static int j_int(cJSON *input, const char *key, int def) {
    if (!input) return def;
    cJSON *item = cJSON_GetObjectItem(input, key);
    return (item && cJSON_IsNumber(item)) ? item->valueint : def;
}

static int j_bool(cJSON *input, const char *key, int def) {
    if (!input) return def;
    cJSON *item = cJSON_GetObjectItem(input, key);
    if (!item) return def;
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    return def;
}

static int tool_is_empty_output(const char *result) {
    return !result || !result[0] || strcmp(result, TC_EMPTY_OUTPUT_MARKER) == 0;
}

static int tool_is_error_output(const char *result) {
    return result &&
           (strncmp(result, "error:", 6) == 0 ||
            strncmp(result, "Error:", 6) == 0 ||
            strncmp(result, "Plugin returned", 15) == 0);
}

const char *execute_tool(int tool_id, cJSON *input, agent_ctx_t *ctx,
                         char *out, size_t out_sz) {
    switch (tool_id) {

    case TOOL_EXEC: {
        const char *cmd = j_str(input, "command");
        int timeout = j_int(input, "timeout", TC_TOOL_TIMEOUT);
        return tool_exec_cmd(cmd, timeout, out, out_sz);
    }

    case TOOL_READ_FILE: {
        const char *path = j_str(input, "path");
        int offset = j_int(input, "offset", 0);
        int limit = j_int(input, "limit", TC_BUF_XL);
        if (!path) { snprintf(out, out_sz, "Error: path required"); return out; }
        if (offset < 0) { snprintf(out, out_sz, "Error: offset must be >= 0"); return out; }
        if (limit <= 0) { snprintf(out, out_sz, "Error: limit must be > 0"); return out; }

        FILE *f = fopen(path, "r");
        if (!f) { snprintf(out, out_sz, "Error: cannot open %s", path); return out; }
        if (offset > 0 && fseek(f, offset, SEEK_SET) != 0) {
            fclose(f);
            snprintf(out, out_sz, "Error: cannot seek %s", path);
            return out;
        }
        if (limit > TC_BUF_XL) limit = TC_BUF_XL;
        if (limit > (int)out_sz - 1) limit = (int)out_sz - 1;
        size_t rd = fread(out, 1, limit, f);
        out[rd] = '\0';
        fclose(f);
        return out;
    }

    case TOOL_WRITE_FILE: {
        const char *path = j_str(input, "path");
        const char *content = j_str(input, "content");
        int append = j_bool(input, "append", 0);
        if (!path || !content) { snprintf(out, out_sz, "Error: path and content required"); return out; }

        /* Ensure parent dir */
        char dir[4096];
        snprintf(dir, sizeof(dir), "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = '\0'; mkdirs(dir); }

        if (append) {
            FILE *f = fopen(path, "a");
            if (!f) { snprintf(out, out_sz, "Error: cannot open %s", path); return out; }
            if (fputs(content, f) == EOF || fclose(f) != 0) {
                snprintf(out, out_sz, "Error: cannot write %s", path);
                return out;
            }
        } else {
            if (atomic_write(path, content, strlen(content)) != 0) {
                snprintf(out, out_sz, "Error: cannot write %s", path);
                return out;
            }
        }
        snprintf(out, out_sz, "Written %zu bytes to %s", strlen(content), path);
        return out;
    }

    case TOOL_SCHEDULE_TASK: {
        const char *run_at = j_str(input, "run_at");
        const char *desc = j_str(input, "description");
        const char *prompt = j_str(input, "prompt");
        if (!run_at || !desc) { snprintf(out, out_sz, "Error: run_at and description required"); return out; }
        return sched_add(&ctx->scheduler, run_at, desc, prompt, NULL, out, out_sz);
    }

    case TOOL_SCHEDULE_RECURRING: {
        const char *run_at = j_str(input, "run_at");
        const char *interval = j_str(input, "interval");
        const char *desc = j_str(input, "description");
        const char *prompt = j_str(input, "prompt");
        if (!run_at || !interval || !desc) {
            snprintf(out, out_sz, "Error: run_at, interval, and description required");
            return out;
        }
        return sched_add(&ctx->scheduler, run_at, desc, prompt, interval, out, out_sz);
    }

    case TOOL_LIST_TASKS:
        return sched_list(&ctx->scheduler, out, out_sz);

    case TOOL_UPDATE_TASK: {
        const char *id = j_str(input, "task_id");
        if (!id || !id[0]) { snprintf(out, out_sz, "Error: task_id required"); return out; }
        return sched_update(&ctx->scheduler, id,
                            j_str(input, "run_at"), j_str(input, "description"),
                            j_str(input, "prompt"), j_str(input, "interval"),
                            out, out_sz);
    }

    case TOOL_CANCEL_TASK: {
        const char *id = j_str(input, "task_id");
        if (!id || !id[0]) { snprintf(out, out_sz, "Error: task_id required"); return out; }
        return sched_cancel(&ctx->scheduler, id, out, out_sz);
    }

    case TOOL_REMEMBER: {
        const char *content = j_str(input, "content");
        const char *category = j_str(input, "category");
        int importance = j_int(input, "importance", 5);
        const char *tags = j_str(input, "tags");
        if (!content || !content[0]) { snprintf(out, out_sz, "Error: content required"); return out; }
        return memory_add(&ctx->memory, content, category, importance, tags, out, out_sz);
    }

    case TOOL_RECALL: {
        const char *query = j_str(input, "query");
        int n = j_int(input, "n", 20);
        return memory_search(&ctx->memory, query, n, out, out_sz);
    }

    case TOOL_SET_FACT: {
        const char *key = j_str(input, "key");
        const char *val = j_str(input, "value");
        if (!key || !key[0] || !val) { snprintf(out, out_sz, "Error: key and value required"); return out; }
        return facts_set(&ctx->memory, key, val, out, out_sz);
    }

    case TOOL_GET_FACT: {
        const char *key = j_str(input, "key");
        return facts_get(&ctx->memory, key, out, out_sz);
    }

    case TOOL_SEND_MESSAGE: {
        const char *to = j_str(input, "to");
        const char *content = j_str(input, "content");
        if (!to || !content) { snprintf(out, out_sz, "Error: to and content required"); return out; }

        if (strcmp(to, "owner") == 0) {
            /* Send via IRC */
            if (ctx->irc)
                irc_reply(ctx->irc, ctx->name, content);
            snprintf(out, out_sz, "Sent to owner via IRC.");
            return out;
        }

        /* Log delegation to session */
        if (ctx->sessions) {
            char delegation_msg[TC_BUF_MD];
            snprintf(delegation_msg, sizeof(delegation_msg),
                     "→ %s: %.200s", to, content);
            session_add_message(ctx->sessions, "", ctx->name, to,
                                delegation_msg, MSG_DELEGATION);
        }

        /* Show inter-agent message on IRC/TUI */
        if (ctx->irc) {
            char prefix[96];
            snprintf(prefix, sizeof(prefix), "%s => %s", ctx->name, to);
            irc_reply(ctx->irc, prefix, content);
        }

        return messenger_send(ctx->messenger, ctx->name, to, content, NULL, out, out_sz);
    }

    case TOOL_LIST_AGENTS: {
        int off = 0;
        off += snprintf(out + off, out_sz - off, "Agents:\n");
        /* This is filled in by daemon with actual agent data */
        for (int i = 0; i < ctx->messenger->n_agents; i++) {
            if (strcmp(ctx->messenger->agents[i], "owner") == 0) continue;
            off += snprintf(out + off, out_sz - off, "- %s\n",
                            ctx->messenger->agents[i]);
        }
        return out;
    }

    case TOOL_CLEAR_MEMORY: {
        const char *agent = j_str(input, "agent");
        const char *what = j_str(input, "what");
        if (!agent || !agent[0]) { snprintf(out, out_sz, "Error: agent required"); return out; }
        if (!what || !what[0]) what = "both";

        int do_mem = (strcmp(what, "memory") == 0 || strcmp(what, "both") == 0);
        int do_facts = (strcmp(what, "facts") == 0 || strcmp(what, "both") == 0);

        if (strcmp(agent, "all") == 0) {
            /* Clear all agents */
            int cleared = 0;
            for (int i = 0; i < ctx->messenger->n_agents; i++) {
                const char *aname = ctx->messenger->agents[i];
                if (strcmp(aname, "owner") == 0) continue;
                char mem_dir[4200];
                snprintf(mem_dir, sizeof(mem_dir), "%s/%s/memory", ctx->data_dir, aname);
                memory_t tmp;
                memory_init(&tmp, mem_dir);
                if (do_mem) memory_clear(&tmp);
                if (do_facts) facts_clear(&tmp);
                cleared++;
            }
            /* Also clear own in-memory cache */
            if (do_mem) memory_clear(&ctx->memory);
            if (do_facts) facts_clear(&ctx->memory);
            snprintf(out, out_sz, "Cleared %s for %d agents.", what, cleared);
        } else {
            if (strcmp(agent, ctx->name) == 0) {
                /* Clear own memory */
                if (do_mem) memory_clear(&ctx->memory);
                if (do_facts) facts_clear(&ctx->memory);
            } else {
                /* Clear another agent's memory via filesystem */
                char mem_dir[4200];
                snprintf(mem_dir, sizeof(mem_dir), "%s/%s/memory", ctx->data_dir, agent);
                memory_t tmp;
                memory_init(&tmp, mem_dir);
                if (do_mem) memory_clear(&tmp);
                if (do_facts) facts_clear(&tmp);
            }
            snprintf(out, out_sz, "Cleared %s for %s.", what, agent);
        }
        return out;
    }

    case TOOL_CREATE_PLUGIN: {
        const char *name = j_str(input, "name");
        const char *code = j_str(input, "code");
        const char *test_input_json = j_str(input, "test_input_json");
        const char *expected_output = j_str(input, "expected_output");
        if (!name || !code) { snprintf(out, out_sz, "Error: name and code required"); return out; }

        /* Write source */
        char src_path[4096];
        snprintf(src_path, sizeof(src_path), "%s/%s.c", ctx->plugins->dir, name);
        atomic_write(src_path, code, strlen(code));

        /* Compile in-memory and load */
        struct stat st;
        time_t mtime = (stat(src_path, &st) == 0) ? st.st_mtime : time(NULL);
        if (plugin_compile(ctx->plugins, src_path, mtime) != 0) {
            if (ctx->plugins->last_error[0])
                snprintf(out, out_sz,
                         "Compilation failed for %s.c:\n%s\n"
                         "Hint: include only \"tc_plugin.h\" and export "
                         "TC_PLUGIN_NAME, TC_PLUGIN_DESC, TC_PLUGIN_SCHEMA, "
                         "and tc_execute(const char *input_json).",
                         name, ctx->plugins->last_error);
            else
                snprintf(out, out_sz,
                         "Compilation failed for %s.c.\n"
                         "Hint: include only \"tc_plugin.h\" and export "
                         "TC_PLUGIN_NAME, TC_PLUGIN_DESC, TC_PLUGIN_SCHEMA, "
                         "and tc_execute(const char *input_json).",
                         name);
            return out;
        }

        if (test_input_json && test_input_json[0]) {
            cJSON *test_input = cJSON_Parse(test_input_json);
            char test_result[TC_BUF_XL];
            const char *result;

            if (!test_input || !cJSON_IsObject(test_input)) {
                cJSON_Delete(test_input);
                snprintf(out, out_sz,
                         "Plugin '%s' compiled and loaded, but self-test "
                         "input is invalid JSON object: %s",
                         name, test_input_json);
                return out;
            }

            result = plugin_execute(ctx->plugins, name, test_input,
                                    test_result, sizeof(test_result));
            cJSON_Delete(test_input);

            if (!result) {
                snprintf(out, out_sz,
                         "Plugin '%s' compiled and loaded, but self-test "
                         "execution failed.", name);
                return out;
            }

            if (tool_is_empty_output(result)) {
                snprintf(out, out_sz,
                         "Plugin '%s' compiled, but self-test failed: "
                         "tool returned %s for input %s",
                         name, TC_EMPTY_OUTPUT_MARKER, test_input_json);
                return out;
            }

            if (expected_output && strcmp(result, expected_output) != 0) {
                snprintf(out, out_sz,
                         "Plugin '%s' compiled, but self-test failed.\n"
                         "Input: %s\nExpected: %s\nActual: %s",
                         name, test_input_json, expected_output, result);
                return out;
            }

            if (!expected_output && tool_is_error_output(result)) {
                snprintf(out, out_sz,
                         "Plugin '%s' compiled, but self-test failed.\n"
                         "Input: %s\nActual: %s",
                         name, test_input_json, result);
                return out;
            }

            snprintf(out, out_sz,
                     "Plugin '%s' compiled, loaded, and self-test passed.\n"
                     "Input: %s\nOutput: %s",
                     name, test_input_json, result);
            return out;
        }

        snprintf(out, out_sz, "Plugin '%s' compiled and loaded.", name);
        return out;
    }

    default:
        snprintf(out, out_sz, "Unknown tool %d", tool_id);
        return out;
    }
}
