/*
 * agent.c — LLM conversation loop
 */

#include "../include/tc.h"
#include "../include/prompt.h"

static const char *trigger_type_str(trigger_type_t t) {
    switch (t) {
    case TRIG_IRC:       return "irc";
    case TRIG_SCHEDULE:  return "schedule";
    case TRIG_AGENT_MSG: return "agent_message";
    }
    return "unknown";
}

static int is_error(const char *result) {
    return !result || strncmp(result, "Error", 5) == 0;
}

/* Who the session answers to */
typedef struct {
    char requesters[TC_MAX_AGENTS][32];  /* agents waiting for an answer */
    int  n_requesters;
    int  reply_batch;   /* only answers to our earlier requests */
    int  relay;         /* text replies go to the owner (IRC/TUI) */
} trigger_info_t;

/* Per-session harness state */
typedef struct {
    int builder_retry_used;
    int hub_retry_used;
    int empty_retry_used;
    int plugin_done;
    int plugin_failed;            /* last create_plugin returned an error */
    int sent_message;
    int listed_agents;
    int blocked_repeats;
    char sent_to[8][32];          /* agents already messaged */
    int  n_sent_to;
    char notice[TC_BUF_LG];       /* harness notice for the requester */
    uint64_t call_hash[64];       /* tool calls already run (name + args) */
    int      call_count[64];
    int      n_calls;
} session_state_t;

/* ── Trigger ────────────────────────────────────────────── */

static void parse_trigger(trigger_type_t type, const char *data, trigger_info_t *t) {
    memset(t, 0, sizeof(*t));
    if (type != TRIG_AGENT_MSG) {
        t->relay = 1;
        return;
    }
    cJSON *msgs = cJSON_Parse(data);
    cJSON *m;
    cJSON_ArrayForEach(m, msgs) {
        const char *from = j_str(m, "from");
        if (!from || cJSON_IsTrue(cJSON_GetObjectItem(m, "reply"))) continue;
        int known = 0;
        for (int i = 0; i < t->n_requesters; i++)
            known |= strcmp(t->requesters[i], from) == 0;
        if (!known && t->n_requesters < TC_MAX_AGENTS)
            snprintf(t->requesters[t->n_requesters++], sizeof(t->requesters[0]), "%s", from);
    }
    cJSON_Delete(msgs);
    t->reply_batch = t->n_requesters == 0;
    t->relay = t->reply_batch;
}

/* The user turn as plain text: small models read prose better than JSON */
static char *render_trigger(trigger_type_t type, const char *data) {
    if (!data || !data[0])
        return strdup(PROMPT_EMPTY_TRIGGER);
    cJSON *arr = type == TRIG_IRC ? NULL : cJSON_Parse(data);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return strdup(data);
    }

    size_t cap = strlen(data) + 256 * (size_t)(cJSON_GetArraySize(arr) + 1);
    char *out = malloc(cap);
    size_t off = 0;
    cJSON *it;
    if (out) out[0] = '\0';
    cJSON_ArrayForEach(it, arr) {
        if (!out) break;
        if (off) buf_appendf(out, cap, &off, "\n\n");
        if (type == TRIG_SCHEDULE) {
            const char *desc = j_str(it, "description");
            const char *prompt = j_str(it, "prompt");
            buf_appendf(out, cap, &off, PROMPT_TASK_DUE, desc ? desc : "");
            if (prompt && prompt[0])
                buf_appendf(out, cap, &off, "\nInstructions: %s", prompt);
        } else {
            const char *from = j_str(it, "from");
            const char *content = j_str(it, "content");
            buf_appendf(out, cap, &off,
                        cJSON_IsTrue(cJSON_GetObjectItem(it, "reply"))
                            ? PROMPT_REPLY_FROM : PROMPT_MESSAGE_FROM,
                        from ? from : "?", content ? content : "");
        }
    }
    cJSON_Delete(arr);
    return out ? out : strdup(data);
}

/* Extract plugin C code from a text reply (markdown block or raw).
 * Returns 1 if valid plugin code was found, 0 otherwise.
 * Sets name[] and code[] on success. */
static int builder_extract_code(const char *text,
                                char *name, size_t name_sz,
                                char *code, size_t code_sz) {
    if (!text || !text[0]) return 0;

    const char *cs = NULL, *ce = NULL;

    /* Try fenced code block: ``` ... ``` */
    const char *fence = strstr(text, "```");
    if (fence) {
        cs = strchr(fence + 3, '\n');
        if (cs) {
            cs++;
            ce = strstr(cs, "\n```");
        }
        if (cs && ce && ce > cs) {
            size_t blen = (size_t)(ce - cs);
            if (blen >= code_sz ||
                !memmem(cs, blen, "tc_plugin.h", 11) ||
                !memmem(cs, blen, "tc_execute", 10))
                cs = ce = NULL;
        } else {
            cs = ce = NULL;
        }
    }

    /* Fallback: raw C code starting with #include "tc_plugin.h" */
    if (!cs) {
        cs = strstr(text, "#include \"tc_plugin.h\"");
        if (cs && strstr(cs, "tc_execute")) {
            ce = NULL;
            for (const char *p = text + strlen(text) - 1; p > cs; p--)
                if (*p == '}') { ce = p + 1; break; }
        }
        if (!ce) cs = NULL;
    }

    if (!cs || !ce || ce <= cs) return 0;

    size_t len = (size_t)(ce - cs);
    if (len >= code_sz) return 0;
    memcpy(code, cs, len);
    code[len] = '\0';

    /* Extract plugin name from TC_PLUGIN_NAME = "..." */
    const char *np = strstr(code, "TC_PLUGIN_NAME");
    if (!np) return 0;
    const char *q1 = strchr(np, '"');
    if (!q1) return 0;
    q1++;
    const char *q2 = strchr(q1, '"');
    if (!q2 || q2 == q1 || (size_t)(q2 - q1) >= name_sz) return 0;

    memcpy(name, q1, (size_t)(q2 - q1));
    name[q2 - q1] = '\0';
    return 1;
}

/* ── Prompt ─────────────────────────────────────────────── */

/* Fixed for the agent (see PROMPT_SYSTEM_FMT): no time, memory or trigger
 * in here, or every session misses the provider's prompt cache. */
static void build_system_prompt(agent_ctx_t *agent, char *out, size_t out_sz) {
    /* Other agents, with their specialty */
    char agents_text[TC_BUF_LG] = "";
    size_t off = 0;
    for (int i = 0; i < agent->n_peers; i++) {
        agent_ctx_t *p = &agent->peers[i];
        if (p == agent) continue;
        buf_appendf(agents_text, sizeof(agents_text), &off, "- %s%s%s%s\n", p->name,
                    p->specialty[0] ? " — " : "", p->specialty,
                    p->is_builder ? PROMPT_BUILDER_PEER : "");
    }

    char objectives[TC_BUF_LG] = "";
    off = 0;
    for (int i = 0; i < agent->n_objectives; i++)
        buf_appendf(objectives, sizeof(objectives), &off, "- %s\n", agent->objectives[i]);

    char builder_rules[TC_BUF_LG * 2] = "";
    if (agent->is_builder) {
        char *tmpl = file_slurp("plugins/_template.c", NULL);
        snprintf(builder_rules, sizeof(builder_rules), PROMPT_BUILDER_RULES,
                 tmpl ? tmpl : "/* template unavailable */");
        free(tmpl);
    }

    snprintf(out, out_sz, PROMPT_SYSTEM_FMT,
        agent->name, agent->personality, agent->system_prompt_extra,
        agent->is_hub ? PROMPT_HUB_ROLE : "",
        builder_rules,
        objectives[0] ? objectives : PROMPT_NONE,
        agents_text[0] ? agents_text : PROMPT_NONE
    );
}

static void format_now(char *now, size_t sz) {
    char utc_str[32], loc_str[32];
    time_t t = time(NULL);
    format_time(t, 0, utc_str, sizeof(utc_str));
    format_time(t, 1, loc_str, sizeof(loc_str));
    /* Local time first: tools take local times */
    if (strncmp(utc_str, loc_str, strlen(loc_str)) == 0)
        snprintf(now, sz, "%s", utc_str);
    else
        snprintf(now, sz, "%s local time (UTC: %s)", loc_str, utc_str);
}

static const char *comm_rules(trigger_type_t trig_type, const trigger_info_t *trig) {
    return trig_type != TRIG_AGENT_MSG ? PROMPT_COMM_DIRECT :
           trig->reply_batch ? PROMPT_COMM_AGENT_REPLY : PROMPT_COMM_AGENT_MSG;
}

/* The first user turn: what changes between sessions, then the trigger.
 * `channel` is the section of lines from the conversation, or "".
 * Returns a malloc'd string, NULL when out of memory. */
static char *build_first_turn(agent_ctx_t *agent, trigger_type_t trig_type,
                              const trigger_info_t *trig, const char *trig_data,
                              const char *channel) {
    char now[96];
    format_now(now, sizeof(now));
    char *message = render_trigger(trig_type, trig_data);
    char *memories = malloc(TC_BUF_XL);
    /* memories + schedule + facts + the fixed text all fit in this */
    size_t cap = (message ? strlen(message) : 0) + strlen(channel) + TC_BUF_XL + 3 * TC_BUF_LG;
    char *out = message && memories ? malloc(cap) : NULL;
    if (out) {
        char schedule[TC_BUF_LG], facts[TC_BUF_LG];
        memory_search(&agent->memory, NULL, 15, memories, TC_BUF_XL);
        sched_list(&agent->scheduler, schedule, sizeof(schedule));
        facts_get(&agent->memory, "", facts, sizeof(facts));
        snprintf(out, cap, PROMPT_FIRST_TURN_FMT,
            trigger_type_str(trig_type),
            comm_rules(trig_type, trig),
            trig->relay ? PROMPT_IRC_FORMAT : "",
            facts[0] ? facts : PROMPT_NONE,
            schedule[0] ? schedule : PROMPT_NONE,
            memories[0] ? memories : PROMPT_NONE,
            channel,
            now,
            message
        );
    }
    free(memories);
    free(message);
    return out;
}

/* A turn in an open conversation: the history already holds the context */
static char *build_next_turn(trigger_type_t trig_type, const trigger_info_t *trig,
                             const char *trig_data, const char *channel) {
    char now[96];
    format_now(now, sizeof(now));
    char *message = render_trigger(trig_type, trig_data);
    size_t cap = (message ? strlen(message) : 0) + strlen(channel) + TC_BUF_LG;
    char *out = message ? malloc(cap) : NULL;
    if (out)
        snprintf(out, cap, PROMPT_NEXT_TURN_FMT, trigger_type_str(trig_type),
                 comm_rules(trig_type, trig), channel, now, message);
    free(message);
    return out;
}

/* Keeps a conversation under max chars by dropping its oldest exchanges
 * (from a user text turn to the next one), never the first: it holds the
 * context. Returns 0 when only the first and the last exchanges are left
 * and they still do not fit. */
static int conv_fit(cJSON *messages, size_t max) {
    for (;;) {
        char *json = cJSON_PrintUnformatted(messages);
        size_t len = json ? strlen(json) : SIZE_MAX;
        free(json);
        if (len <= max) return 1;

        int n = cJSON_GetArraySize(messages), from = -1, to = -1;
        for (int i = 1; i < n && to < 0; i++) {
            cJSON *m = cJSON_GetArrayItem(messages, i);
            const char *role = j_str(m, "role");
            if (!role || strcmp(role, "user") != 0 ||
                !cJSON_IsString(cJSON_GetObjectItem(m, "content")))
                continue;   /* tool results are user turns too, as arrays */
            if (from < 0) from = i; else to = i;
        }
        if (to < 0) return 0;
        for (int i = from; i < to; i++)
            cJSON_DeleteItemFromArray(messages, from);
    }
}

/* ── Tool calls ─────────────────────────────────────────── */

static int tool_offered(cJSON *tools, const char *name) {
    cJSON *t;
    cJSON_ArrayForEach(t, tools) {
        const char *n = j_str(t, "name");
        if (n && strcmp(n, name) == 0) return 1;
    }
    return 0;
}

/* Compact "key=value, ..." summary of tool arguments for IRC/TUI */
static void summarize_args(cJSON *input, char *out, size_t out_sz) {
    size_t off = 0;
    out[0] = '\0';
    cJSON *item;
    cJSON_ArrayForEach(item, input) {
        char val[96];
        if (cJSON_IsString(item)) {
            const char *s = item->valuestring;
            if (strchr(s, '\n'))   /* multiline values (code): show the size */
                snprintf(val, sizeof(val), "<%zu chars>", strlen(s));
            else
                snprintf(val, sizeof(val), "%.*s%s", utf8_prefix(s, 60), s,
                         strlen(s) > 60 ? "..." : "");
        } else if (cJSON_IsNumber(item)) {
            snprintf(val, sizeof(val), "%g", item->valuedouble);
        } else if (cJSON_IsBool(item)) {
            snprintf(val, sizeof(val), "%s", cJSON_IsTrue(item) ? "true" : "false");
        } else {
            snprintf(val, sizeof(val), "{...}");
        }
        buf_appendf(out, out_sz, &off, "%s%s=%s", off ? ", " : "",
                    item->string ? item->string : "?", val);
    }
}

/* Count identical calls (same tool, same arguments) in this session.
 * Returns 1 when the call exceeds `limit` and must not run again. */
static int repeated_call(session_state_t *st, const char *name, cJSON *input, int limit) {
    char *args = cJSON_PrintUnformatted(input);
    uint64_t h = 1469598103934665603ULL;   /* FNV-1a */
    for (const char *p = name; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ULL;
    h *= 1099511628211ULL;                 /* separator */
    for (const char *p = args ? args : ""; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ULL;
    free(args);

    for (int i = 0; i < st->n_calls; i++)
        if (st->call_hash[i] == h)
            return ++st->call_count[i] > limit;
    if (st->n_calls < 64) {
        st->call_hash[st->n_calls] = h;
        st->call_count[st->n_calls++] = 1;
    }
    return 0;
}

static int is_requester(const trigger_info_t *trig, const char *name) {
    for (int i = 0; name && i < trig->n_requesters; i++)
        if (strcmp(trig->requesters[i], name) == 0)
            return 1;
    return 0;
}

static const char *run_tool(agent_ctx_t *agent, cJSON *tools, tool_call_t *call,
                            session_state_t *st, const trigger_info_t *trig,
                            const char *thread_id, char *out, size_t out_sz) {
    cJSON *input = cJSON_Parse(call->input_json);
    int tid = tool_find(call->name);
    const char *result;

    if (!tool_offered(tools, call->name)) {
        size_t off = 0;
        buf_appendf(out, out_sz, &off, PROMPT_UNKNOWN_TOOL, call->name);
        cJSON *t;
        cJSON_ArrayForEach(t, tools)
            buf_appendf(out, out_sz, &off, " %s", j_str(t, "name"));
        result = out;
    } else if (!cJSON_IsObject(input)) {
        snprintf(out, out_sz, PROMPT_BAD_ARGS, call->name);
        result = out;
    } else if (repeated_call(st, call->name, input,
                             agent->is_builder && tid == TOOL_READ_FILE ? 1 : 2)) {
        snprintf(out, out_sz, PROMPT_REPEAT_CALL, call->name);
        st->blocked_repeats++;
        result = out;
    } else if (tid >= 0) {
        const char *to = tid == TOOL_SEND_MESSAGE
            ? messenger_resolve(agent->messenger, j_str(input, "to")) : NULL;
        result = tool_check_args(tid, input, out, out_sz);
        if (!result && is_requester(trig, to))
            /* Answering the requester: marked as an answer, so it cannot
             * start a new request (two agents would ping-pong) */
            result = agent_message(agent, to, j_str(input, "content"), thread_id, 1,
                                   out, out_sz);
        else if (!result)
            result = execute_tool(tid, input, agent, out, out_sz);
        if (tid == TOOL_SEND_MESSAGE && !is_error(result)) {
            st->sent_message = 1;
            if (to && st->n_sent_to < 8)
                snprintf(st->sent_to[st->n_sent_to++], sizeof(st->sent_to[0]), "%s", to);
        }
    } else {
        result = plugin_execute(agent->plugins, call->name, input, 0, out, out_sz);
        if (!result) {
            snprintf(out, out_sz, "Error: plugin '%s' is no longer loaded", call->name);
            result = out;
        }
    }
    cJSON_Delete(input);
    return result;
}

/* Small local models have small context windows: once the tool outputs in
 * the history exceed the agent's budget, shrink the oldest ones to half the
 * budget. A trim rewrites the history, so the server must process it again
 * instead of reusing its cache: trim rarely, and a lot at once. Only for
 * OpenAI-compatible providers — Anthropic histories stay append-only. */
static void trim_history(cJSON *messages, size_t budget) {
    size_t total = 0;
    cJSON *msg, *block;
    cJSON_ArrayForEach(msg, messages)
        cJSON_ArrayForEach(block, cJSON_GetObjectItem(msg, "content")) {
            const char *c = j_str(block, "content");
            if (c) total += strlen(c);
        }
    if (total <= budget) return;
    budget /= 2;

    cJSON *newest = cJSON_GetArrayItem(messages, cJSON_GetArraySize(messages) - 1);
    cJSON_ArrayForEach(msg, messages) {
        if (total <= budget || msg == newest) break;
        cJSON_ArrayForEach(block, cJSON_GetObjectItem(msg, "content")) {
            const char *c = j_str(block, "content");
            size_t len = c ? strlen(c) : 0;
            if (total <= budget || len <= 400) continue;
            char shorter[400];
            int keep = utf8_prefix(c, 200);
            snprintf(shorter, sizeof(shorter), "%.*s" PROMPT_ELIDED,
                     keep, c, len - (size_t)keep);
            total -= len - strlen(shorter);
            cJSON_ReplaceItemInObject(block, "content", cJSON_CreateString(shorter));
        }
    }
}

/* ── Session loop ───────────────────────────────────────── */

static void add_user_text(cJSON *messages, const char *text) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", text);
    cJSON_AddItemToArray(messages, msg);
}

/* After a reply without tool calls: 1 = the session goes on (a nudge was
 * queued), 0 = done, -1 = failed */
static int after_text_reply(agent_ctx_t *agent, trigger_type_t trig_type,
                            const trigger_info_t *trig, const char *thread_id,
                            const char *text, session_state_t *st, cJSON *messages) {
    if (agent->is_builder && !st->plugin_done) {
        /* Weak models often print the plugin instead of calling create_plugin */
        char name[64], *code = malloc(TC_BUF_XL);
        if (code && builder_extract_code(text, name, sizeof(name), code, TC_BUF_XL)) {
            log_info("[%s] Auto-extracted plugin '%s' from text", agent->name, name);
            char action[160], result[TC_BUF_LG];
            snprintf(action, sizeof(action), "auto-compiles '%s' from text", name);
            irc_action(agent->irc, agent->name, action);
            session_add_message(agent->sessions, thread_id, agent->name, "",
                                action, MSG_TOOL_CALL);

            cJSON *input = cJSON_CreateObject();
            cJSON_AddStringToObject(input, "name", name);
            cJSON_AddStringToObject(input, "code", code);
            execute_tool(TOOL_CREATE_PLUGIN, input, agent, result, sizeof(result));
            cJSON_Delete(input);
            session_add_message(agent->sessions, thread_id, agent->name, "",
                                result, MSG_TOOL_RESULT);
            free(code);

            if (!is_error(result)) {
                st->plugin_done = 1;
                snprintf(st->notice, sizeof(st->notice), "%s", result);
                if (trig->relay)
                    irc_reply(agent->irc, agent->name, result);
                return 0;
            }
            if (!st->builder_retry_used) {
                char retry[TC_BUF_LG];
                snprintf(retry, sizeof(retry), PROMPT_BUILDER_AUTO_FAIL, result);
                add_user_text(messages, retry);
                st->builder_retry_used = 1;
                return 1;
            }
        } else {
            free(code);
            /* Agents only write to the builder to get a plugin; the owner may
             * just be chatting with it, unless a build is under way. */
            if (trig_type != TRIG_AGENT_MSG && !st->plugin_failed)
                return 0;
            if (!st->builder_retry_used) {
                add_user_text(messages, PROMPT_BUILDER_NUDGE);
                st->builder_retry_used = 1;
                return 1;
            }
        }
        snprintf(st->notice, sizeof(st->notice),
                 "Builder stalled: the plugin request ended without a working plugin.");
        if (trig->relay)
            irc_reply(agent->irc, agent->name, st->notice);
        log_error("[%s] Builder stalled: ended with text-only reply", agent->name);
        return -1;
    }

    /* Hub narrated a delegation without calling send_message */
    if (agent->is_hub && st->listed_agents && !st->sent_message && !st->hub_retry_used) {
        add_user_text(messages, PROMPT_HUB_NUDGE);
        st->hub_retry_used = 1;
        return 1;
    }

    if (!text[0] && !st->empty_retry_used) {
        add_user_text(messages, PROMPT_EMPTY_NUDGE);
        st->empty_retry_used = 1;
        return 1;
    }
    return 0;
}

/* Requests from other agents always get an answer, even when a small
 * model forgets send_message or the session fails. */
static void deliver_answer(agent_ctx_t *agent, const trigger_info_t *trig,
                           session_state_t *st, const char *thread_id,
                           const char *answer, int failed) {
    char fallback[160];
    if (!st->notice[0] && (!answer || !answer[0])) {
        snprintf(fallback, sizeof(fallback), failed ? PROMPT_FAILED_ANSWER : PROMPT_NO_ANSWER,
                 agent->name);
        answer = fallback;
    }
    size_t len = strlen(st->notice) + (answer ? strlen(answer) : 0) + 2;
    char *msg = malloc(len);
    if (!msg) return;
    snprintf(msg, len, "%s%s%s", st->notice, st->notice[0] && answer ? "\n" : "",
             answer ? answer : "");

    for (int i = 0; i < trig->n_requesters; i++) {
        int done = 0;
        for (int j = 0; j < st->n_sent_to; j++)
            done |= strcmp(st->sent_to[j], trig->requesters[i]) == 0;
        if (done) continue;
        char out[TC_BUF_SM];
        agent_message(agent, trig->requesters[i], msg, thread_id, 1, out, sizeof(out));
        session_add_message(agent->sessions, thread_id, agent->name,
                            trig->requesters[i], msg, MSG_DELEGATION);
    }
    free(msg);
}

int agent_run_session(agent_ctx_t *agent, trigger_type_t trig_type,
                      const char *trig_data, const char *thread_id) {
    trigger_info_t trig;
    parse_trigger(trig_type, trig_data, &trig);

    /* Sessions that talk to the owner form the conversation: while it is
     * open, the agent's history goes on, with what the others said since */
    int talk = trig.relay && agent->chat;
    unsigned conv_id = talk ? chat_id(agent->chat) : 0, seq = 0;
    int resume = talk && agent->conv && agent->conv_id == conv_id;
    if (talk && !resume) {
        cJSON_Delete(agent->conv);
        agent->conv = NULL;
    }
    size_t chat_sz = TC_CHAT_LINES * TC_CHAT_LINE + 64;
    char *lines = malloc(chat_sz), *channel = malloc(chat_sz);
    if (lines && channel) {
        lines[0] = channel[0] = '\0';
        if (talk)
            seq = chat_render(agent->chat, resume ? agent->conv_seq : 0,
                              resume ? agent->name : NULL, lines, chat_sz);
        if (lines[0])
            snprintf(channel, chat_sz, PROMPT_CHAT_HEADER, lines);
    }

    char *system_prompt = malloc(TC_BUF_HUGE);
    char *result_buf = malloc(TC_BUF_XL);
    char *opening = !lines || !channel ? NULL
        : resume ? build_next_turn(trig_type, &trig, trig_data, channel)
                 : build_first_turn(agent, trig_type, &trig, trig_data, channel);
    session_state_t *st = calloc(1, sizeof(*st));
    free(lines);
    free(channel);
    if (!system_prompt || !result_buf || !opening || !st) {
        log_error("[%s] out of memory", agent->name);
        free(system_prompt); free(result_buf); free(opening); free(st);
        return SESSION_FAILED;
    }
    build_system_prompt(agent, system_prompt, TC_BUF_HUGE);

    cJSON *tools = tools_to_json(agent->is_builder);
    if (agent->plugins) {
        cJSON *plugin_tools = plugin_get_schemas(agent->plugins);
        cJSON *pt;
        while ((pt = cJSON_DetachItemFromArray(plugin_tools, 0)) != NULL)
            cJSON_AddItemToArray(tools, pt);
        cJSON_Delete(plugin_tools);
    }

    cJSON *messages = resume ? agent->conv : cJSON_CreateArray();
    if (resume)
        agent->conv = NULL;   /* back at the end, if the session goes well */
    add_user_text(messages, opening);
    free(opening);
    if (talk && trig_type == TRIG_IRC) {   /* after the turn: it already holds the message */
        char who[48];
        snprintf(who, sizeof(who), "owner → %s", agent->name);
        chat_add(agent->chat, agent->name, who, trig_data);
    }

    int max_turns = agent->max_turns > 0 ? agent->max_turns : TC_MAX_TURNS;
    int trim = agent->history_budget > 0 &&
               strcmp(agent->provider.provider_type, "anthropic") != 0;
    int outcome = SESSION_COMPLETED;
    char *answer = NULL;   /* last text reply */
    int turn;

    log_info("[%s] === SESSION (trigger: %s, thread: %s, model: %s) ===",
             agent->name, trigger_type_str(trig_type),
             thread_id && thread_id[0] ? thread_id : "-", agent->provider.model);

    for (turn = 0; turn < max_turns; turn++) {
        llm_response_t resp;
        if (llm_call(&agent->provider, system_prompt, messages, tools, &resp) != 0) {
            snprintf(st->notice, sizeof(st->notice), PROMPT_LLM_ERROR, resp.error);
            irc_reply(agent->irc, agent->name, st->notice);
            llm_response_free(&resp);
            outcome = SESSION_FAILED;
            break;
        }
        if (resp.usage.input)   /* some local servers do not count */
            log_info("[%s] Tokens: %d in (%d from cache, %d to cache), %d out",
                     agent->name, resp.usage.input, resp.usage.cached,
                     resp.usage.written, resp.usage.output);

        /* Log text blocks; relay replies (not thinking) to the owner */
        size_t text_len = 0;
        for (int i = 0; i < resp.n_text; i++) {
            text_block_t *b = &resp.text_blocks[i];
            int thinking = strcmp(b->type, "thinking") == 0;
            session_add_message(agent->sessions, thread_id, agent->name, "",
                                b->text, thinking ? MSG_THINKING : MSG_TEXT);
            if (thinking) continue;
            text_len += strlen(b->text) + 1;
            if (trig.relay) {
                irc_reply(agent->irc, agent->name, b->text);
                if (talk) chat_add(agent->chat, agent->name, agent->name, b->text);
            }
        }
        char *text = calloc(1, text_len + 1);
        for (int i = 0; text && i < resp.n_text; i++)
            if (strcmp(resp.text_blocks[i].type, "thinking") != 0) {
                if (text[0]) strcat(text, "\n");
                strcat(text, resp.text_blocks[i].text);
            }
        if (text && text[0]) {
            free(answer);
            answer = strdup(text);
        }

        /* Replay the assistant turn exactly as the provider returned it */
        if (cJSON_GetArraySize(resp.content) > 0) {
            cJSON *assistant = cJSON_CreateObject();
            cJSON_AddStringToObject(assistant, "role", "assistant");
            cJSON_AddItemToObject(assistant, "content", resp.content);
            resp.content = NULL;
            cJSON_AddItemToArray(messages, assistant);
        }

        int next = 0;   /* 1: call the model again */
        if (resp.n_tools > 0) {
            cJSON *results = cJSON_CreateArray();
            for (int i = 0; i < resp.n_tools; i++) {
                tool_call_t *call = &resp.tool_calls[i];
                log_info("[%s] Tool: %s", agent->name, call->name);

                cJSON *input = cJSON_Parse(call->input_json);
                char params[256], line[400];
                summarize_args(input, params, sizeof(params));
                cJSON_Delete(input);
                snprintf(line, sizeof(line), "calls %s(%s)", call->name, params);
                irc_action(agent->irc, agent->name, line);
                session_add_message(agent->sessions, thread_id, agent->name, "",
                                    line + 6, MSG_TOOL_CALL);

                const char *result = run_tool(agent, tools, call, st, &trig, thread_id,
                                              result_buf, TC_BUF_XL);
                session_add_message(agent->sessions, thread_id, agent->name, "",
                                    result, MSG_TOOL_RESULT);

                int ok = !is_error(result);
                if (strcmp(call->name, "create_plugin") == 0) {
                    st->plugin_failed = !ok;
                    if (ok) {
                        st->plugin_done = 1;
                        snprintf(st->notice, sizeof(st->notice), "%s", result);
                    }
                }
                if (strcmp(call->name, "list_agents") == 0)
                    st->listed_agents = 1;

                cJSON *tr = cJSON_CreateObject();
                cJSON_AddStringToObject(tr, "type", "tool_result");
                cJSON_AddStringToObject(tr, "tool_use_id", call->id);
                cJSON_AddStringToObject(tr, "content", result);
                if (!ok)
                    cJSON_AddBoolToObject(tr, "is_error", 1);
                cJSON_AddItemToArray(results, tr);
            }
            cJSON *tool_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_msg, "role", "user");
            cJSON_AddItemToObject(tool_msg, "content", results);
            cJSON_AddItemToArray(messages, tool_msg);
            if (trim)
                trim_history(messages, (size_t)agent->history_budget);

            next = 1;
            if (st->blocked_repeats >= 3) {
                snprintf(st->notice, sizeof(st->notice), "%s", PROMPT_STUCK);
                irc_reply(agent->irc, agent->name, PROMPT_STUCK);
                log_warn("[%s] stuck repeating tool calls, session stopped", agent->name);
                outcome = SESSION_FAILED;
                next = 0;
            }
        } else if (strcmp(resp.stop_reason, "refusal") == 0) {
            if (trig.relay) irc_reply(agent->irc, agent->name, PROMPT_REFUSED);
            snprintf(st->notice, sizeof(st->notice), "%s", PROMPT_REFUSED);
            log_warn("[%s] model refused", agent->name);
        } else {
            next = after_text_reply(agent, trig_type, &trig, thread_id,
                                    text ? text : "", st, messages);
            if (next < 0) {
                outcome = SESSION_FAILED;
                next = 0;
            }
        }

        free(text);
        llm_response_free(&resp);
        if (!next) break;
    }

    if (turn == max_turns) {
        snprintf(st->notice, sizeof(st->notice), PROMPT_MAX_TURNS, max_turns);
        irc_reply(agent->irc, agent->name, st->notice);
        log_warn("[%s] max_turns (%d) reached", agent->name, max_turns);
    }

    if (trig.n_requesters)
        deliver_answer(agent, &trig, st, thread_id, answer, outcome != SESSION_COMPLETED);

    /* Keep the conversation: complete exchanges only, within the limit */
    const char *last = j_str(cJSON_GetArrayItem(messages, cJSON_GetArraySize(messages) - 1), "role");
    if (talk && outcome == SESSION_COMPLETED && last && !strcmp(last, "assistant") &&
        conv_fit(messages, agent->history_budget > 0 ? (size_t)agent->history_budget
                                                     : TC_CONV_MAX)) {
        agent->conv = messages;
        agent->conv_id = conv_id;
        agent->conv_seq = seq;
        messages = NULL;
    }

    free(answer);
    cJSON_Delete(messages);
    cJSON_Delete(tools);
    free(system_prompt);
    free(result_buf);
    free(st);

    log_info("[%s] === END ===", agent->name);
    return outcome;
}
