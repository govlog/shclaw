/*
 * agent.c — LLM conversation loop
 */

#include "../include/tc.h"
#include "../include/prompt.h"

static const char *trigger_type_str(trigger_type_t t) {
    switch (t) {
    case TRIG_STARTUP:   return "startup";
    case TRIG_IRC:       return "irc";
    case TRIG_SCHEDULE:  return "schedule";
    case TRIG_AGENT_MSG: return "agent_message";
    case TRIG_SOCKET:    return "socket";
    }
    return "unknown";
}

/* Find tool ID by name */
static int find_tool(const char *name) {
    static const char *tool_names[] = {
        [TOOL_EXEC] = "exec",
        [TOOL_READ_FILE] = "read_file",
        [TOOL_WRITE_FILE] = "write_file",
        [TOOL_SCHEDULE_TASK] = "schedule_task",
        [TOOL_SCHEDULE_RECURRING] = "schedule_recurring",
        [TOOL_LIST_TASKS] = "list_tasks",
        [TOOL_UPDATE_TASK] = "update_task",
        [TOOL_CANCEL_TASK] = "cancel_task",
        [TOOL_REMEMBER] = "remember",
        [TOOL_RECALL] = "recall",
        [TOOL_SET_FACT] = "set_fact",
        [TOOL_GET_FACT] = "get_fact",
        [TOOL_SEND_MESSAGE] = "send_message",
        [TOOL_LIST_AGENTS] = "list_agents",
        [TOOL_CREATE_PLUGIN] = "create_plugin",
        [TOOL_CLEAR_MEMORY] = "clear_memory",
    };

    for (int i = 0; i < TOOL_COUNT; i++)
        if (tool_names[i] && strcmp(tool_names[i], name) == 0)
            return i;
    return -1;
}

static int builder_create_plugin_succeeded(const char *result) {
    if (!result) return 0;
    return strstr(result, "compiled and loaded.") != NULL;
}

static void extract_agent_message_request(const char *trig_data,
                                          char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!trig_data || !trig_data[0]) return;

    cJSON *msgs = cJSON_Parse(trig_data);
    if (!msgs || !cJSON_IsArray(msgs)) {
        cJSON_Delete(msgs);
        return;
    }

    cJSON *first = cJSON_GetArrayItem(msgs, 0);
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(first, "content"));
    if (content)
        snprintf(out, out_sz, "%s", content);
    cJSON_Delete(msgs);
}

static void builder_notify_stall(agent_ctx_t *agent, const char *thread_id,
                                 const char *trig_data,
                                 const char *last_text) {
    if (!agent || !agent->sessions || !agent->messenger ||
        !thread_id || !thread_id[0])
        return;

    cJSON *session = session_get(agent->sessions, thread_id);
    if (!session) return;

    const char *initiator =
        cJSON_GetStringValue(cJSON_GetObjectItem(session, "initiator"));

    if (initiator && initiator[0]) {
        char msg[TC_BUF_LG];
        char out[TC_BUF_MD];
        char request[TC_BUF_MD];

        extract_agent_message_request(trig_data, request, sizeof(request));

        snprintf(msg, sizeof(msg),
                 "Builder stalled: plugin request ended without "
                 "create_plugin or send_message.\n"
                 "Original request: %s\n"
                 "Last model reply: %s",
                 request[0] ? request : "(unknown)",
                 (last_text && last_text[0]) ? last_text : "(empty)");

        messenger_send(agent->messenger, agent->name, initiator,
                       msg, thread_id, out, sizeof(out));
        session_add_message(agent->sessions, thread_id,
                            agent->name, initiator, msg, MSG_DELEGATION);
    } else if (agent->irc) {
        irc_reply(agent->irc, agent->name,
                  "Builder stalled: plugin request ended without action.");
    }

    cJSON_Delete(session);
}

static void builder_notify_success(agent_ctx_t *agent, const char *thread_id,
                                   const char *trig_data,
                                   const char *result_text) {
    if (!agent || !agent->sessions || !agent->messenger ||
        !thread_id || !thread_id[0])
        return;

    cJSON *session = session_get(agent->sessions, thread_id);
    if (!session) return;

    const char *initiator =
        cJSON_GetStringValue(cJSON_GetObjectItem(session, "initiator"));

    if (initiator && initiator[0]) {
        char msg[TC_BUF_LG];
        char out[TC_BUF_MD];
        char request[TC_BUF_MD];

        extract_agent_message_request(trig_data, request, sizeof(request));

        snprintf(msg, sizeof(msg),
                 "Builder completed the plugin request.\n"
                 "Original request: %s\n"
                 "Result: %s",
                 request[0] ? request : "(unknown)",
                 (result_text && result_text[0]) ? result_text : "(no result)");

        messenger_send(agent->messenger, agent->name, initiator,
                       msg, thread_id, out, sizeof(out));
        session_add_message(agent->sessions, thread_id,
                            agent->name, initiator, msg, MSG_DELEGATION);
    } else if (agent->irc) {
        irc_reply(agent->irc, agent->name,
                  "Builder completed the plugin request.");
    }

    cJSON_Delete(session);
}

static int builder_read_file_duplicate(const char *input_json,
                                       char seen[][TC_BUF_SM],
                                       int n_seen) {
    if (!input_json) return 0;
    for (int i = 0; i < n_seen; i++)
        if (strcmp(seen[i], input_json) == 0)
            return 1;
    return 0;
}

static void builder_mark_read_file(const char *input_json,
                                   char seen[][TC_BUF_SM],
                                   int *n_seen) {
    if (!input_json || !n_seen || *n_seen >= 16) return;
    snprintf(seen[*n_seen], TC_BUF_SM, "%s", input_json);
    (*n_seen)++;
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

void agent_build_system_prompt(agent_ctx_t *agent, trigger_type_t trig_type,
                               const char *trig_data, const char *thread_id,
                               const char **all_agents,
                               const char **all_specialties, int n_agents,
                               char *out, size_t out_sz) {
    char now[96], memories[TC_BUF_XL], schedule[TC_BUF_LG];
    {
        time_t t = time(NULL);
        struct tm utc, loc;
        gmtime_r(&t, &utc);
        localtime_r(&t, &loc);
        char utc_str[32], loc_str[32];
        snprintf(utc_str, sizeof(utc_str), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                 utc.tm_hour, utc.tm_min, utc.tm_sec);
        snprintf(loc_str, sizeof(loc_str), "%04d-%02d-%02dT%02d:%02d:%02d",
                 loc.tm_year + 1900, loc.tm_mon + 1, loc.tm_mday,
                 loc.tm_hour, loc.tm_min, loc.tm_sec);
        if (strcmp(utc_str, loc_str) == 0)
            snprintf(now, sizeof(now), "%s", utc_str);
        else
            snprintf(now, sizeof(now), "%s (local: %s)", utc_str, loc_str);
    }
    memory_search(&agent->memory, NULL, 15, memories, sizeof(memories));
    sched_list(&agent->scheduler, schedule, sizeof(schedule));

    /* Build agents roster (with specialties) */
    char agents_text[TC_BUF_MD] = "";
    int off = 0;
    for (int i = 0; i < n_agents; i++) {
        if (strcmp(all_agents[i], agent->name) == 0) continue;
        if (all_specialties && all_specialties[i] && all_specialties[i][0])
            off += snprintf(agents_text + off, sizeof(agents_text) - off,
                            "- %s — %s\n", all_agents[i], all_specialties[i]);
        else
            off += snprintf(agents_text + off, sizeof(agents_text) - off,
                            "- %s\n", all_agents[i]);
    }

    /* Objectives */
    char objectives[TC_BUF_MD] = "";
    off = 0;
    for (int i = 0; i < agent->n_objectives; i++)
        off += snprintf(objectives + off, sizeof(objectives) - off,
                        "- %s\n", agent->objectives[i]);

    char builder_rules_buf[TC_BUF_LG * 2] = "";
    if (agent->is_builder) {
        char *tmpl = file_slurp("plugins/_template.c", NULL);
        snprintf(builder_rules_buf, sizeof(builder_rules_buf),
            PROMPT_BUILDER_RULES,
            tmpl ? tmpl : "/* template unavailable */");
        free(tmpl);
    }
    const char *builder_rules = builder_rules_buf;

    const char *comm_rules = (trig_type == TRIG_AGENT_MSG)
        ? PROMPT_COMM_AGENT_MSG : PROMPT_COMM_DIRECT;

    snprintf(out, out_sz, PROMPT_SYSTEM_FMT,
        agent->name, agent->personality, agent->system_prompt_extra,
        agent->is_hub ? PROMPT_HUB_ROLE : "",
        builder_rules,
        trigger_type_str(trig_type),
        trig_data ? trig_data : PROMPT_NO_DATA,
        thread_id ? thread_id : "(none)",
        comm_rules,
        objectives[0] ? objectives : PROMPT_NONE,
        agents_text[0] ? agents_text : PROMPT_NONE,
        schedule,
        memories,
        now
    );
}

int agent_run_session(agent_ctx_t *agent, trigger_type_t trig_type,
                      const char *trig_data, const char *thread_id,
                      const char **all_agents, const char **all_specialties,
                      int n_agents) {
    char *system_prompt = malloc(TC_BUF_HUGE);
    agent_build_system_prompt(agent, trig_type, trig_data, thread_id,
                              all_agents, all_specialties, n_agents,
                              system_prompt, TC_BUF_HUGE);

    cJSON *tools = tools_to_json(agent->is_builder);

    /* Add plugin tools */
    if (agent->plugins) {
        cJSON *plugin_tools = plugin_get_schemas(agent->plugins);
        cJSON *pt;
        cJSON_ArrayForEach(pt, plugin_tools)
            cJSON_AddItemToArray(tools, cJSON_Duplicate(pt, 1));
        cJSON_Delete(plugin_tools);
    }

    /* Build initial messages */
    cJSON *messages = cJSON_CreateArray();
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content",
        trig_data ? trig_data : PROMPT_STARTUP_FALLBACK);
    cJSON_AddItemToArray(messages, user_msg);

    int max_turns = agent->max_turns > 0 ? agent->max_turns : TC_MAX_TURNS;
    int builder_retry_used = 0;
    int builder_plugin_completed = 0;
    int builder_sent_message = 0;
    int hub_called_list_agents = 0;
    int hub_retry_used = 0;
    char builder_success_result[TC_BUF_LG] = "";
    char builder_read_file_seen[16][TC_BUF_SM];
    int n_builder_read_file_seen = 0;

    log_info("[%s] === SESSION (trigger: %s, thread: %s, model: %s) ===",
             agent->name, trigger_type_str(trig_type),
             thread_id ? thread_id : "-", agent->provider.model);

    int outcome = SESSION_COMPLETED;

    for (int turn = 0; turn < max_turns; turn++) {
        if (agent->abort_flag) {
            log_info("[%s] Aborted", agent->name);
            outcome = SESSION_ABORTED;
            break;
        }

        /* Call LLM */
        llm_response_t resp;
        if (llm_call(&agent->provider, system_prompt, messages, tools, &resp) != 0) {
            log_error("[%s] LLM call failed", agent->name);
            outcome = SESSION_FAILED;
            break;
        }

        /* Append assistant message for conversation continuity */
        cJSON *assistant = cJSON_CreateObject();
        cJSON_AddStringToObject(assistant, "role", "assistant");

        /* Collect text */
        char *text_combined = calloc(1, TC_BUF_HUGE);
        for (int i = 0; i < resp.n_text; i++) {
            if (resp.text_blocks[i].text) {
                if (text_combined[0]) strcat(text_combined, "\n");
                strncat(text_combined, resp.text_blocks[i].text,
                        TC_BUF_HUGE - strlen(text_combined) - 1);
            }
        }

        /* Log text blocks to session */
        for (int i = 0; i < resp.n_text; i++) {
            if (!resp.text_blocks[i].text || !resp.text_blocks[i].text[0]) continue;
            msg_type_t mt = (strcmp(resp.text_blocks[i].type, "thinking") == 0)
                            ? MSG_THINKING : MSG_TEXT;
            if (thread_id && agent->sessions)
                session_add_message(agent->sessions, thread_id,
                                    agent->name, "", resp.text_blocks[i].text, mt);

            /* Echo text responses to IRC/TUI (not agent_msg -- those are
               shown via send_message in tools.c to avoid duplicates) */
            if (mt == MSG_TEXT && agent->irc &&
                (trig_type == TRIG_IRC || trig_type == TRIG_SCHEDULE ||
                 trig_type == TRIG_SOCKET))
                irc_reply(agent->irc, agent->is_hub ? NULL : agent->name,
                           resp.text_blocks[i].text);
        }

        if (resp.n_tools > 0) {
            /* Build Anthropic-style assistant content for the conversation */
            cJSON *content_arr = cJSON_CreateArray();

            if (text_combined[0]) {
                cJSON *tb = cJSON_CreateObject();
                cJSON_AddStringToObject(tb, "type", "text");
                cJSON_AddStringToObject(tb, "text", text_combined);
                cJSON_AddItemToArray(content_arr, tb);
            }

            for (int i = 0; i < resp.n_tools; i++) {
                cJSON *tu = cJSON_CreateObject();
                cJSON_AddStringToObject(tu, "type", "tool_use");
                cJSON_AddStringToObject(tu, "id", resp.tool_calls[i].id);
                cJSON_AddStringToObject(tu, "name", resp.tool_calls[i].name);
                cJSON *input = cJSON_Parse(resp.tool_calls[i].input_json);
                cJSON_AddItemToObject(tu, "input", input ? input : cJSON_CreateObject());
                cJSON_AddItemToArray(content_arr, tu);
            }

            cJSON_AddItemToObject(assistant, "content", content_arr);
            cJSON_AddItemToArray(messages, assistant);

            /* Execute tools and collect results */
            cJSON *tool_results_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_results_msg, "role", "user");
            cJSON *results_arr = cJSON_CreateArray();

            for (int i = 0; i < resp.n_tools; i++) {
                log_info("[%s] Tool: %s", agent->name, resp.tool_calls[i].name);

                /* Show tool call on IRC/TUI as ACTION */
                if (agent->irc) {
                    char action[384];
                    /* Build compact param summary from input JSON */
                    char params[256] = "";
                    cJSON *inp = cJSON_Parse(resp.tool_calls[i].input_json);
                    if (inp) {
                        int poff = 0;
                        cJSON *item;
                        cJSON_ArrayForEach(item, inp) {
                            const char *k = item->string;
                            char val[80];
                            if (cJSON_IsString(item)) {
                                /* Skip multiline values (e.g. code), show length */
                                if (strchr(item->valuestring, '\n')) {
                                    snprintf(val, sizeof(val), "<%d chars>",
                                             (int)strlen(item->valuestring));
                                } else {
                                    snprintf(val, sizeof(val), "%.60s%s",
                                             item->valuestring,
                                             strlen(item->valuestring) > 60 ? "..." : "");
                                }
                            } else if (cJSON_IsNumber(item)) {
                                snprintf(val, sizeof(val), "%g", item->valuedouble);
                            } else if (cJSON_IsBool(item)) {
                                snprintf(val, sizeof(val), "%s",
                                         cJSON_IsTrue(item) ? "true" : "false");
                            } else {
                                snprintf(val, sizeof(val), "{...}");
                            }
                            int wrote = snprintf(params + poff, sizeof(params) - poff,
                                                 "%s%s=%s", poff ? ", " : "", k, val);
                            poff += wrote;
                            if (poff >= (int)sizeof(params) - 1) break;
                        }
                        cJSON_Delete(inp);
                    }
                    snprintf(action, sizeof(action), "calls %s(%s)",
                             resp.tool_calls[i].name, params);
                    irc_action(agent->irc, agent->name, action);
                }

                /* Log tool call to session */
                if (thread_id && agent->sessions) {
                    char call_summary[TC_BUF_SM];
                    snprintf(call_summary, sizeof(call_summary), "%s(%.150s)",
                             resp.tool_calls[i].name, resp.tool_calls[i].input_json);
                    session_add_message(agent->sessions, thread_id,
                                        agent->name, "", call_summary, MSG_TOOL_CALL);
                }

                cJSON *input = cJSON_Parse(resp.tool_calls[i].input_json);
                char result_buf[TC_BUF_XL];

                int tid = find_tool(resp.tool_calls[i].name);
                const char *result;

                if (agent->is_builder &&
                    strcmp(resp.tool_calls[i].name, "read_file") == 0 &&
                    builder_read_file_duplicate(resp.tool_calls[i].input_json,
                                                builder_read_file_seen,
                                                n_builder_read_file_seen)) {
                    snprintf(result_buf, sizeof(result_buf),
                             "Already read this file in the current builder "
                             "session. Reuse the earlier content instead of "
                             "calling read_file again.");
                    result = result_buf;
                } else if (tid >= 0) {
                    result = execute_tool(tid, input, agent, result_buf, sizeof(result_buf));
                } else if (agent->plugins) {
                    /* Try plugins */
                    result = plugin_execute(agent->plugins, resp.tool_calls[i].name,
                                            input, result_buf, sizeof(result_buf));
                    if (!result)
                        snprintf(result_buf, sizeof(result_buf), "Unknown tool: %s",
                                 resp.tool_calls[i].name);
                    result = result ? result : result_buf;
                } else {
                    snprintf(result_buf, sizeof(result_buf), "Unknown tool: %s",
                             resp.tool_calls[i].name);
                    result = result_buf;
                }

                cJSON_Delete(input);

                if (agent->is_builder &&
                    strcmp(resp.tool_calls[i].name, "read_file") == 0 &&
                    !builder_read_file_duplicate(resp.tool_calls[i].input_json,
                                                 builder_read_file_seen,
                                                 n_builder_read_file_seen))
                    builder_mark_read_file(resp.tool_calls[i].input_json,
                                           builder_read_file_seen,
                                           &n_builder_read_file_seen);

                /* Log result to session */
                if (thread_id && agent->sessions)
                    session_add_message(agent->sessions, thread_id,
                                        agent->name, "", result, MSG_TOOL_RESULT);

                if (strcmp(resp.tool_calls[i].name, "create_plugin") == 0 &&
                    builder_create_plugin_succeeded(result)) {
                    builder_plugin_completed = 1;
                    snprintf(builder_success_result, sizeof(builder_success_result),
                             "%s", result);
                }
                if (strcmp(resp.tool_calls[i].name, "send_message") == 0)
                    builder_sent_message = 1;
                if (strcmp(resp.tool_calls[i].name, "list_agents") == 0)
                    hub_called_list_agents = 1;

                cJSON *tr = cJSON_CreateObject();
                cJSON_AddStringToObject(tr, "type", "tool_result");
                cJSON_AddStringToObject(tr, "tool_use_id", resp.tool_calls[i].id);
                cJSON_AddStringToObject(tr, "content", result);
                cJSON_AddItemToArray(results_arr, tr);
            }

            cJSON_AddItemToObject(tool_results_msg, "content", results_arr);
            cJSON_AddItemToArray(messages, tool_results_msg);

        } else if (strcmp(resp.stop_reason, "end_turn") == 0) {
            cJSON_AddStringToObject(assistant, "content", text_combined);
            cJSON_AddItemToArray(messages, assistant);

            if (agent->is_builder && trig_type == TRIG_AGENT_MSG) {
                if (builder_plugin_completed && !builder_sent_message) {
                    builder_notify_success(agent, thread_id, trig_data,
                                           builder_success_result);
                } else if (!builder_plugin_completed && !builder_sent_message) {
                    /* Try auto-extracting plugin code from text */
                    char ext_name[64], ext_code[TC_BUF_XL];
                    int extracted = builder_extract_code(text_combined,
                                        ext_name, sizeof(ext_name),
                                        ext_code, sizeof(ext_code));

                    if (extracted) {
                        log_info("[%s] Auto-extracted plugin '%s' from text",
                                 agent->name, ext_name);
                        if (agent->irc) {
                            char action[256];
                            snprintf(action, sizeof(action),
                                     "auto-compiles '%s' from text", ext_name);
                            irc_action(agent->irc, agent->name, action);
                        }
                        if (thread_id && agent->sessions) {
                            char cs[256];
                            snprintf(cs, sizeof(cs),
                                     "create_plugin(name=%s, auto-extracted)",
                                     ext_name);
                            session_add_message(agent->sessions, thread_id,
                                                agent->name, "", cs,
                                                MSG_TOOL_CALL);
                        }

                        cJSON *auto_input = cJSON_CreateObject();
                        cJSON_AddStringToObject(auto_input, "name", ext_name);
                        cJSON_AddStringToObject(auto_input, "code", ext_code);
                        char auto_result[TC_BUF_LG];
                        execute_tool(TOOL_CREATE_PLUGIN, auto_input, agent,
                                     auto_result, sizeof(auto_result));
                        cJSON_Delete(auto_input);

                        if (thread_id && agent->sessions)
                            session_add_message(agent->sessions, thread_id,
                                                agent->name, "",
                                                auto_result, MSG_TOOL_RESULT);

                        if (builder_create_plugin_succeeded(auto_result)) {
                            builder_plugin_completed = 1;
                            snprintf(builder_success_result,
                                     sizeof(builder_success_result),
                                     "%s", auto_result);
                            builder_notify_success(agent, thread_id,
                                                   trig_data,
                                                   builder_success_result);
                            free(text_combined);
                            llm_response_free(&resp);
                            break;
                        }

                        /* Compilation failed — inject error for retry */
                        if (!builder_retry_used) {
                            cJSON *retry = cJSON_CreateObject();
                            cJSON_AddStringToObject(retry, "role", "user");
                            char rbuf[TC_BUF_LG];
                            snprintf(rbuf, sizeof(rbuf),
                                PROMPT_BUILDER_AUTO_FAIL, auto_result);
                            cJSON_AddStringToObject(retry, "content", rbuf);
                            cJSON_AddItemToArray(messages, retry);
                            builder_retry_used = 1;
                            free(text_combined);
                            llm_response_free(&resp);
                            continue;
                        }
                    } else if (!builder_retry_used) {
                        cJSON *nudge = cJSON_CreateObject();
                        cJSON_AddStringToObject(nudge, "role", "user");
                        cJSON_AddStringToObject(nudge, "content",
                            PROMPT_BUILDER_NUDGE);
                        cJSON_AddItemToArray(messages, nudge);
                        builder_retry_used = 1;
                        free(text_combined);
                        llm_response_free(&resp);
                        continue;
                    }

                    builder_notify_stall(agent, thread_id, trig_data,
                                         text_combined);
                    log_error("[%s] Builder stalled: ended with text-only reply",
                              agent->name);
                    outcome = SESSION_FAILED;
                    free(text_combined);
                    llm_response_free(&resp);
                    break;
                }
            }

            /* Hub agent narrated delegation without calling send_message */
            if (agent->is_hub && hub_called_list_agents &&
                !builder_sent_message && !hub_retry_used) {
                cJSON *nudge = cJSON_CreateObject();
                cJSON_AddStringToObject(nudge, "role", "user");
                cJSON_AddStringToObject(nudge, "content",
                    PROMPT_HUB_NUDGE);
                cJSON_AddItemToArray(messages, nudge);
                hub_retry_used = 1;
                free(text_combined);
                llm_response_free(&resp);
                continue;
            }

            free(text_combined);
            llm_response_free(&resp);
            break;
        } else {
            cJSON_AddStringToObject(assistant, "content", text_combined);
            cJSON_AddItemToArray(messages, assistant);
            log_warn("[%s] Unexpected stop_reason: %s", agent->name, resp.stop_reason);
            free(text_combined);
            llm_response_free(&resp);
            break;
        }

        free(text_combined);
        llm_response_free(&resp);
    }

    cJSON_Delete(messages);
    cJSON_Delete(tools);
    free(system_prompt);

    log_info("[%s] === END ===", agent->name);
    return outcome;
}
