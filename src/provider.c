/*
 * provider.c — LLM API calls (Anthropic + OpenAI)
 */

#include "../include/tc.h"

static char *json_string_or_empty(cJSON *item) {
    if (!item) return strdup("{}");
    return cJSON_PrintUnformatted(item);
}

static void append_text_block(char *buf, size_t buf_sz, const char *text) {
    if (!text || !text[0] || buf_sz == 0) return;
    if (buf[0])
        strncat(buf, "\n", buf_sz - strlen(buf) - 1);
    strncat(buf, text, buf_sz - strlen(buf) - 1);
}

static void append_openai_message(cJSON *oai_messages, cJSON *msg) {
    const char *role = j_str(msg, "role");
    cJSON *content = cJSON_GetObjectItem(msg, "content");

    if (!content || !cJSON_IsArray(content)) {
        cJSON_AddItemToArray(oai_messages, cJSON_Duplicate(msg, 1));
        return;
    }

    if (role && strcmp(role, "assistant") == 0) {
        cJSON *converted = cJSON_CreateObject();
        cJSON *tool_calls = cJSON_CreateArray();
        char text_buf[TC_BUF_HUGE] = "";
        int n_tool_calls = 0;

        cJSON *block;
        cJSON_ArrayForEach(block, content) {
            const char *btype = j_str(block, "type");
            if (!btype) continue;

            if (strcmp(btype, "text") == 0 || strcmp(btype, "thinking") == 0) {
                append_text_block(text_buf, sizeof(text_buf),
                                  j_str(block, "text"));
                continue;
            }

            if (strcmp(btype, "tool_use") == 0) {
                const char *id = j_str(block, "id");
                const char *name = j_str(block, "name");
                char *args = json_string_or_empty(cJSON_GetObjectItem(block, "input"));
                if (!name || !args) {
                    free(args);
                    continue;
                }

                cJSON *tool_call = cJSON_CreateObject();
                cJSON *func = cJSON_CreateObject();
                cJSON_AddStringToObject(tool_call, "id", id ? id : "");
                cJSON_AddStringToObject(tool_call, "type", "function");
                cJSON_AddStringToObject(func, "name", name);
                cJSON_AddStringToObject(func, "arguments", args);
                cJSON_AddItemToObject(tool_call, "function", func);
                cJSON_AddItemToArray(tool_calls, tool_call);
                n_tool_calls++;
                free(args);
            }
        }

        cJSON_AddStringToObject(converted, "role", "assistant");
        if (text_buf[0])
            cJSON_AddStringToObject(converted, "content", text_buf);
        else
            cJSON_AddNullToObject(converted, "content");
        if (n_tool_calls > 0)
            cJSON_AddItemToObject(converted, "tool_calls", tool_calls);
        else
            cJSON_Delete(tool_calls);
        cJSON_AddItemToArray(oai_messages, converted);
        return;
    }

    if (role && strcmp(role, "user") == 0) {
        char text_buf[TC_BUF_HUGE] = "";
        cJSON *block;

        cJSON_ArrayForEach(block, content) {
            const char *btype = j_str(block, "type");
            if (!btype) continue;

            if (strcmp(btype, "tool_result") == 0) {
                const char *tool_call_id = j_str(block, "tool_use_id");
                const char *text = j_str(block, "content");
                cJSON *tool_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(tool_msg, "role", "tool");
                cJSON_AddStringToObject(tool_msg, "tool_call_id", tool_call_id ? tool_call_id : "");
                cJSON_AddStringToObject(tool_msg, "content", text ? text : "");
                cJSON_AddItemToArray(oai_messages, tool_msg);
            } else if (strcmp(btype, "text") == 0) {
                append_text_block(text_buf, sizeof(text_buf),
                                  j_str(block, "text"));
            }
        }

        if (text_buf[0]) {
            cJSON *converted = cJSON_CreateObject();
            cJSON_AddStringToObject(converted, "role", "user");
            cJSON_AddStringToObject(converted, "content", text_buf);
            cJSON_AddItemToArray(oai_messages, converted);
        }
        return;
    }

    cJSON_AddItemToArray(oai_messages, cJSON_Duplicate(msg, 1));
}

static int call_anthropic(provider_ref_t *prov, const char *system_prompt,
                          cJSON *messages, cJSON *tools, llm_response_t *out) {
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "model", prov->model);
    cJSON_AddNumberToObject(payload, "max_tokens", 4096);
    cJSON_AddStringToObject(payload, "system", system_prompt);
    cJSON_AddItemToObject(payload, "messages", cJSON_Duplicate(messages, 1));
    if (tools && cJSON_GetArraySize(tools) > 0)
        cJSON_AddItemToObject(payload, "tools", cJSON_Duplicate(tools, 1));

    char *json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);

    /* Build URL */
    char url[512];
    if (prov->base_url[0])
        snprintf(url, sizeof(url), "%s/v1/messages", prov->base_url);
    else
        memcpy(url, "https://api.anthropic.com/v1/messages", sizeof("https://api.anthropic.com/v1/messages"));

    /* Set auth headers */
    http_set_header(NULL, NULL); /* clear */
    http_set_header("x-api-key", prov->api_key);
    http_set_header("anthropic-version", "2023-06-01");

    http_response_t resp = http_post_json(url, json);
    free(json);

    if (resp.status != 200) {
        log_error("Anthropic API: HTTP %d: %.200s",
                  resp.status, resp.body ? resp.body : "(null)");
        http_response_free(&resp);
        return -1;
    }

    /* Parse response */
    cJSON *root = cJSON_Parse(resp.body);
    http_response_free(&resp);
    if (!root) { log_error("Anthropic: invalid JSON response"); return -1; }

    memset(out, 0, sizeof(*out));

    /* Stop reason */
    const char *stop = j_str(root, "stop_reason");
    snprintf(out->stop_reason, sizeof(out->stop_reason), "%s",
             stop ? stop : "end_turn");

    /* Parse content blocks */
    cJSON *content = cJSON_GetObjectItem(root, "content");
    int n_blocks = cJSON_GetArraySize(content);

    out->text_blocks = calloc(n_blocks + 1, sizeof(text_block_t));
    out->tool_calls = calloc(n_blocks + 1, sizeof(tool_call_t));

    cJSON *block;
    cJSON_ArrayForEach(block, content) {
        const char *type = j_str(block, "type");
        if (!type) continue;

        if (strcmp(type, "text") == 0 || strcmp(type, "thinking") == 0) {
            const char *text = j_str(block, "text");
            if (text) {
                out->text_blocks[out->n_text].text = strdup(text);
                snprintf(out->text_blocks[out->n_text].type,
                         sizeof(out->text_blocks[out->n_text].type), "%s", type);
                out->n_text++;
            }
        } else if (strcmp(type, "tool_use") == 0) {
            const char *id = j_str(block, "id");
            const char *name = j_str(block, "name");
            cJSON *input = cJSON_GetObjectItem(block, "input");

            if (id && name) {
                snprintf(out->tool_calls[out->n_tools].id, 64, "%s", id);
                snprintf(out->tool_calls[out->n_tools].name, 64, "%s", name);
                out->tool_calls[out->n_tools].input_json = json_string_or_empty(input);
                out->n_tools++;
            }
        }
    }

    cJSON_Delete(root);
    return 0;
}

static cJSON *convert_tools_to_openai(cJSON *tools) {
    cJSON *oai_tools = cJSON_CreateArray();
    cJSON *t;
    cJSON_ArrayForEach(t, tools) {
        cJSON *func = cJSON_CreateObject();
        cJSON_AddStringToObject(func, "name", j_str(t, "name"));
        cJSON_AddStringToObject(func, "description", j_str(t, "description"));
        cJSON *schema = cJSON_GetObjectItem(t, "input_schema");
        if (schema)
            cJSON_AddItemToObject(func, "parameters", cJSON_Duplicate(schema, 1));

        cJSON *wrapper = cJSON_CreateObject();
        cJSON_AddStringToObject(wrapper, "type", "function");
        cJSON_AddItemToObject(wrapper, "function", func);
        cJSON_AddItemToArray(oai_tools, wrapper);
    }
    return oai_tools;
}

static int call_openai(provider_ref_t *prov, const char *system_prompt,
                       cJSON *messages, cJSON *tools, llm_response_t *out) {
    cJSON *oai_messages = cJSON_CreateArray();

    /* System message */
    if (system_prompt && system_prompt[0]) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", system_prompt);
        cJSON_AddItemToArray(oai_messages, sys);
    }

    /* Convert messages */
    cJSON *msg;
    cJSON_ArrayForEach(msg, messages)
        append_openai_message(oai_messages, msg);

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "model", prov->model);
    cJSON_AddItemToObject(payload, "messages", oai_messages);
    cJSON_AddNumberToObject(payload, "max_completion_tokens", 4096);

    if (tools && cJSON_GetArraySize(tools) > 0) {
        cJSON *oai_tools = convert_tools_to_openai(tools);
        cJSON_AddItemToObject(payload, "tools", oai_tools);
    }

    char *json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);

    char url[512];
    if (prov->base_url[0])
        snprintf(url, sizeof(url), "%s/v1/chat/completions", prov->base_url);
    else
        memcpy(url, "https://api.openai.com/v1/chat/completions", sizeof("https://api.openai.com/v1/chat/completions"));

    char auth[512];
    snprintf(auth, sizeof(auth), "Bearer %s",
             prov->api_key[0] ? prov->api_key : "ollama");
    http_set_header(NULL, NULL); /* clear */
    http_set_header("Authorization", auth);

    http_response_t resp = http_post_json(url, json);
    free(json);

    if (resp.status != 200) {
        log_error("OpenAI API: HTTP %d: %.200s",
                  resp.status, resp.body ? resp.body : "(null)");
        http_response_free(&resp);
        return -1;
    }

    cJSON *root = cJSON_Parse(resp.body);
    http_response_free(&resp);
    if (!root) { log_error("OpenAI: invalid JSON response"); return -1; }

    memset(out, 0, sizeof(*out));

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    const char *finish = j_str(choice, "finish_reason");
    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");

    /* Text */
    const char *content_text = j_str(message, "content");
    out->text_blocks = calloc(2, sizeof(text_block_t));
    out->tool_calls = calloc((tool_calls ? cJSON_GetArraySize(tool_calls) : 0) + 1,
                             sizeof(tool_call_t));

    if (content_text && content_text[0]) {
        out->text_blocks[0].text = strdup(content_text);
        snprintf(out->text_blocks[0].type, sizeof(out->text_blocks[0].type), "text");
        out->n_text = 1;
    }

    /* Tool calls */
    if (tool_calls) {
        cJSON *tc;
        cJSON_ArrayForEach(tc, tool_calls) {
            cJSON *func = cJSON_GetObjectItem(tc, "function");
            const char *tc_id = j_str(tc, "id");
            const char *tc_name = j_str(func, "name");
            const char *tc_args = j_str(func, "arguments");

            if (tc_id && tc_name) {
                snprintf(out->tool_calls[out->n_tools].id, 64, "%s", tc_id);
                snprintf(out->tool_calls[out->n_tools].name, 64, "%s", tc_name);
                out->tool_calls[out->n_tools].input_json = strdup(tc_args ? tc_args : "{}");
                out->n_tools++;
            }
        }
    }

    /* Stop reason */
    if (out->n_tools > 0 || (finish && strcmp(finish, "tool_calls") == 0))
        snprintf(out->stop_reason, sizeof(out->stop_reason), "tool_use");
    else
        snprintf(out->stop_reason, sizeof(out->stop_reason), "end_turn");

    cJSON_Delete(root);
    return 0;
}

int llm_call(provider_ref_t *prov, const char *system_prompt,
             cJSON *messages, cJSON *tools, llm_response_t *out) {
    if (strcmp(prov->provider_type, "anthropic") == 0)
        return call_anthropic(prov, system_prompt, messages, tools, out);
    else
        return call_openai(prov, system_prompt, messages, tools, out);
}

void llm_response_free(llm_response_t *r) {
    if (r->text_blocks) {
        for (int i = 0; i < r->n_text; i++)
            free(r->text_blocks[i].text);
        free(r->text_blocks);
    }
    if (r->tool_calls) {
        for (int i = 0; i < r->n_tools; i++)
            free(r->tool_calls[i].input_json);
        free(r->tool_calls);
    }
    memset(r, 0, sizeof(*r));
}
