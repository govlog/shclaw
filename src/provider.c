/*
 * provider.c — LLM API calls (Anthropic + OpenAI-compatible)
 *
 * The conversation is kept in Anthropic's block format; the OpenAI path
 * converts it on the fly and converts responses back.
 */

#include "../include/tc.h"

typedef void (*auth_fn)(const provider_ref_t *prov);

/* Official endpoints get their own parameters; anything else is a local
 * or third-party server speaking the same protocol. */
static int is_official(const provider_ref_t *prov, const char *host) {
    return !prov->base_url[0] || strstr(prov->base_url, host) != NULL;
}

/* base_url may end with '/' or already include "/v1" */
static void api_url(const provider_ref_t *prov, const char *def_base,
                    const char *path, char *url, size_t sz) {
    const char *base = prov->base_url[0] ? prov->base_url : def_base;
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') n--;
    if (n >= 3 && strncmp(base + n - 3, "/v1", 3) == 0 &&
        strncmp(path, "/v1/", 4) == 0)
        path += 3;
    snprintf(url, sz, "%.*s%s", (int)n, base, path);
}

static void anthropic_auth(const provider_ref_t *prov) {
    http_set_header("x-api-key", prov->api_key);
    http_set_header("anthropic-version", "2023-06-01");
}

static void openai_auth(const provider_ref_t *prov) {
    char auth[300];
    snprintf(auth, sizeof(auth), "Bearer %s",
             prov->api_key[0] ? prov->api_key : "none");
    http_set_header("Authorization", auth);
}

static void describe_failure(const http_response_t *r, char *err, size_t sz) {
    if (r->status < 0) {
        snprintf(err, sz, "%s", http_strerror(r->status));
        return;
    }
    cJSON *root = cJSON_Parse(r->body ? r->body : "");
    cJSON *e = cJSON_GetObjectItem(root, "error");
    const char *msg = cJSON_IsString(e) ? e->valuestring : j_str(e, "message");
    snprintf(err, sz, "HTTP %d: %.200s", r->status,
             msg ? msg : (r->body ? r->body : ""));
    cJSON_Delete(root);
}

static int retryable(int status) {
    return status == HTTP_ERR_CONNECT || status == HTTP_ERR_PROTO ||
           status == 408 || status == 409 || status == 429 || status >= 500;
}

/* Same policy as the official SDKs: two retries on connection errors,
 * 408/409/429 and 5xx, honouring Retry-After. */
static http_response_t post_retry(const provider_ref_t *prov, const char *url,
                                  const char *json, auth_fn auth,
                                  char *err, size_t err_sz) {
    for (int attempt = 0;; attempt++) {
        auth(prov);
        http_set_timeout(prov->timeout > 0 ? prov->timeout : TC_LLM_TIMEOUT);
        http_response_t r = http_post_json(url, json);
        if (r.status == 200)
            return r;
        describe_failure(&r, err, err_sz);
        if (attempt >= 2 || !retryable(r.status))
            return r;
        int wait = r.retry_after > 0 ? r.retry_after : 2 << (attempt * 2);
        if (wait > 60) wait = 60;
        log_warn("LLM %s: %s, retry %d/2 in %ds",
                 prov->provider_name, err, attempt + 1, wait);
        http_response_free(&r);
        sleep((unsigned)wait);
    }
}

static int add_text(llm_response_t *out, const char *type, const char *text,
                    size_t len) {
    if (!text || len == 0) return 0;
    text_block_t *b = &out->text_blocks[out->n_text];
    b->text = strndup(text, len);
    if (!b->text) return -1;
    snprintf(b->type, sizeof(b->type), "%s", type);
    out->n_text++;
    return 0;
}

static void add_tool_call(llm_response_t *out, const char *id,
                          const char *name, char *input_json) {
    tool_call_t *tc = &out->tool_calls[out->n_tools++];
    snprintf(tc->id, sizeof(tc->id), "%s", id);
    snprintf(tc->name, sizeof(tc->name), "%s", name);
    tc->input_json = input_json;
}

/* ── Anthropic ──────────────────────────────────────────── */

static int call_anthropic(provider_ref_t *prov, const char *system_prompt,
                          cJSON *messages, cJSON *tools, llm_response_t *out) {
    int official = is_official(prov, "api.anthropic.com");
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "model", prov->model);
    cJSON_AddNumberToObject(payload, "max_tokens",
                            prov->max_tokens > 0 ? prov->max_tokens : 16000);
    cJSON_AddStringToObject(payload, "system", system_prompt);
    cJSON_AddItemReferenceToObject(payload, "messages", messages);
    if (tools && cJSON_GetArraySize(tools) > 0)
        cJSON_AddItemReferenceToObject(payload, "tools", tools);
    /* Tools and system prompt are fixed for a session: cache the prefix so
     * each tool round only pays full price for what it appends. */
    if (official)
        cJSON_AddStringToObject(cJSON_AddObjectToObject(payload, "cache_control"),
                                "type", "ephemeral");

    char *json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (json)
        utf8_scrub(json);   /* capped tool output may end mid-character */
    if (!json) {
        snprintf(out->error, sizeof(out->error), "cannot serialize request");
        return -1;
    }

    char url[512];
    api_url(prov, "https://api.anthropic.com", "/v1/messages", url, sizeof(url));
    http_response_t resp = post_retry(prov, url, json, anthropic_auth,
                                      out->error, sizeof(out->error));
    free(json);
    if (resp.status != 200) {
        http_response_free(&resp);
        return -1;
    }

    cJSON *root = cJSON_Parse(resp.body);
    http_response_free(&resp);
    if (!root) {
        snprintf(out->error, sizeof(out->error), "invalid JSON response");
        return -1;
    }

    const char *stop = j_str(root, "stop_reason");
    snprintf(out->stop_reason, sizeof(out->stop_reason), "%s",
             stop ? stop : "end_turn");

    /* Replay the content exactly as received: thinking blocks carry
     * signatures that must come back unchanged on the next request. */
    cJSON *content = cJSON_DetachItemFromObject(root, "content");
    cJSON_Delete(root);
    if (!cJSON_IsArray(content)) {
        cJSON_Delete(content);
        content = cJSON_CreateArray();
    }
    out->content = content;

    int n_blocks = cJSON_GetArraySize(content);
    out->text_blocks = calloc(n_blocks + 1, sizeof(text_block_t));
    out->tool_calls = calloc(n_blocks + 1, sizeof(tool_call_t));
    if (!out->text_blocks || !out->tool_calls) {
        snprintf(out->error, sizeof(out->error), "out of memory");
        return -1;
    }

    cJSON *block;
    cJSON_ArrayForEach(block, content) {
        const char *type = j_str(block, "type");
        if (!type) continue;

        if (strcmp(type, "text") == 0) {
            const char *text = j_str(block, "text");
            add_text(out, "text", text, text ? strlen(text) : 0);
        } else if (strcmp(type, "thinking") == 0) {
            const char *text = j_str(block, "thinking");
            add_text(out, "thinking", text, text ? strlen(text) : 0);
        } else if (strcmp(type, "tool_use") == 0) {
            const char *id = j_str(block, "id");
            const char *name = j_str(block, "name");
            cJSON *input = cJSON_GetObjectItem(block, "input");
            char *input_json = input ? cJSON_PrintUnformatted(input) : strdup("{}");
            if (id && name && input_json)
                add_tool_call(out, id, name, input_json);
            else
                free(input_json);
        }
    }
    return 0;
}

/* ── OpenAI-compatible ──────────────────────────────────── */

/* Text of all blocks of the given type, joined by newlines (malloc'd) */
static char *join_blocks(cJSON *content, const char *type) {
    size_t len = 0, cap = 256;
    char *s = malloc(cap);
    if (!s) return NULL;
    s[0] = '\0';
    cJSON *block;
    cJSON_ArrayForEach(block, content) {
        const char *btype = j_str(block, "type");
        const char *text = j_str(block, "text");
        if (!btype || strcmp(btype, type) != 0 || !text || !text[0]) continue;
        size_t tl = strlen(text);
        if (len + tl + 2 > cap) {
            while (len + tl + 2 > cap) cap *= 2;
            char *grown = realloc(s, cap);
            if (!grown) break;
            s = grown;
        }
        if (len) s[len++] = '\n';
        memcpy(s + len, text, tl + 1);
        len += tl;
    }
    return s;
}

static void add_string_or_empty(cJSON *obj, const char *key, char *s) {
    cJSON_AddStringToObject(obj, key, s ? s : "");
    free(s);
}

static void append_openai_message(cJSON *oai_messages, cJSON *msg) {
    const char *role = j_str(msg, "role");
    cJSON *content = cJSON_GetObjectItem(msg, "content");

    if (!role || !cJSON_IsArray(content)) {
        cJSON_AddItemToArray(oai_messages, cJSON_Duplicate(msg, 1));
        return;
    }

    if (strcmp(role, "assistant") == 0) {
        cJSON *converted = cJSON_CreateObject();
        cJSON *tool_calls = cJSON_CreateArray();
        cJSON_AddStringToObject(converted, "role", "assistant");
        add_string_or_empty(converted, "content", join_blocks(content, "text"));

        cJSON *block;
        cJSON_ArrayForEach(block, content) {
            const char *btype = j_str(block, "type");
            const char *name = j_str(block, "name");
            if (!btype || strcmp(btype, "tool_use") != 0 || !name) continue;
            cJSON *input = cJSON_GetObjectItem(block, "input");
            char *args = input ? cJSON_PrintUnformatted(input) : NULL;
            cJSON *tool_call = cJSON_CreateObject();
            cJSON *func = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_call, "id", j_str(block, "id") ? j_str(block, "id") : "");
            cJSON_AddStringToObject(tool_call, "type", "function");
            cJSON_AddStringToObject(func, "name", name);
            cJSON_AddStringToObject(func, "arguments", args ? args : "{}");
            cJSON_AddItemToObject(tool_call, "function", func);
            cJSON_AddItemToArray(tool_calls, tool_call);
            free(args);
        }
        if (cJSON_GetArraySize(tool_calls) > 0)
            cJSON_AddItemToObject(converted, "tool_calls", tool_calls);
        else
            cJSON_Delete(tool_calls);
        cJSON_AddItemToArray(oai_messages, converted);
        return;
    }

    /* user: tool results become "tool" messages, text stays a user message */
    cJSON *block;
    cJSON_ArrayForEach(block, content) {
        const char *btype = j_str(block, "type");
        if (!btype || strcmp(btype, "tool_result") != 0) continue;
        const char *id = j_str(block, "tool_use_id");
        const char *text = j_str(block, "content");
        cJSON *tool_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_msg, "role", "tool");
        cJSON_AddStringToObject(tool_msg, "tool_call_id", id ? id : "");
        cJSON_AddStringToObject(tool_msg, "content", text ? text : "");
        cJSON_AddItemToArray(oai_messages, tool_msg);
    }
    char *text = join_blocks(content, "text");
    if (text && text[0]) {
        cJSON *converted = cJSON_CreateObject();
        cJSON_AddStringToObject(converted, "role", role);
        cJSON_AddStringToObject(converted, "content", text);
        cJSON_AddItemToArray(oai_messages, converted);
    }
    free(text);
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
            cJSON_AddItemReferenceToObject(func, "parameters", schema);

        cJSON *wrapper = cJSON_CreateObject();
        cJSON_AddStringToObject(wrapper, "type", "function");
        cJSON_AddItemToObject(wrapper, "function", func);
        cJSON_AddItemToArray(oai_tools, wrapper);
    }
    return oai_tools;
}

/* Arguments normally arrive as a JSON string, but some servers send an
 * object and some small models double-encode the string. */
static char *tool_arguments(cJSON *args) {
    if (cJSON_IsObject(args))
        return cJSON_PrintUnformatted(args);
    if (!cJSON_IsString(args) || !args->valuestring[0])
        return strdup("{}");
    cJSON *parsed = cJSON_Parse(args->valuestring);
    if (cJSON_IsString(parsed)) {
        cJSON *inner = cJSON_Parse(parsed->valuestring);
        if (cJSON_IsObject(inner)) {
            char *s = cJSON_PrintUnformatted(inner);
            cJSON_Delete(inner);
            cJSON_Delete(parsed);
            return s;
        }
        cJSON_Delete(inner);
    }
    cJSON_Delete(parsed);
    return strdup(args->valuestring);   /* validated by the agent loop */
}

static int call_openai(provider_ref_t *prov, const char *system_prompt,
                       cJSON *messages, cJSON *tools, llm_response_t *out) {
    int official = is_official(prov, "api.openai.com");
    cJSON *oai_messages = cJSON_CreateArray();

    if (system_prompt && system_prompt[0]) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", system_prompt);
        cJSON_AddItemToArray(oai_messages, sys);
    }

    cJSON *msg;
    cJSON_ArrayForEach(msg, messages)
        append_openai_message(oai_messages, msg);

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "model", prov->model);
    cJSON_AddItemToObject(payload, "messages", oai_messages);
    /* OpenAI's reasoning models only take max_completion_tokens, while
     * most compatible servers (Ollama, llama.cpp, vLLM) only know max_tokens */
    cJSON_AddNumberToObject(payload,
                            official ? "max_completion_tokens" : "max_tokens",
                            prov->max_tokens > 0 ? prov->max_tokens
                                                 : (official ? 16000 : 4096));
    if (tools && cJSON_GetArraySize(tools) > 0)
        cJSON_AddItemToObject(payload, "tools", convert_tools_to_openai(tools));

    char *json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (json)
        utf8_scrub(json);   /* capped tool output may end mid-character */
    if (!json) {
        snprintf(out->error, sizeof(out->error), "cannot serialize request");
        return -1;
    }

    char url[512];
    api_url(prov, "https://api.openai.com", "/v1/chat/completions", url, sizeof(url));
    http_response_t resp = post_retry(prov, url, json, openai_auth,
                                      out->error, sizeof(out->error));
    free(json);
    if (resp.status != 200) {
        http_response_free(&resp);
        return -1;
    }

    cJSON *root = cJSON_Parse(resp.body);
    http_response_free(&resp);
    cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0);
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    if (!message) {
        snprintf(out->error, sizeof(out->error), "invalid response (no message)");
        cJSON_Delete(root);
        return -1;
    }
    const char *finish = j_str(choice, "finish_reason");
    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    int n_calls = cJSON_GetArraySize(tool_calls);

    out->text_blocks = calloc(3, sizeof(text_block_t));
    out->tool_calls = calloc(n_calls + 1, sizeof(tool_call_t));
    out->content = cJSON_CreateArray();
    if (!out->text_blocks || !out->tool_calls) {
        snprintf(out->error, sizeof(out->error), "out of memory");
        cJSON_Delete(root);
        return -1;
    }

    /* Reasoning: a separate field on some servers, or <think>...</think>
     * inlined at the start of the content. Never shown to the owner. */
    const char *reasoning = j_str(message, "reasoning_content");
    if (!reasoning) reasoning = j_str(message, "reasoning");
    add_text(out, "thinking", reasoning, reasoning ? strlen(reasoning) : 0);

    const char *text = j_str(message, "content");
    if (text) {
        while (*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t') text++;
        if (strncmp(text, "<think>", 7) == 0) {
            const char *end = strstr(text, "</think>");
            const char *th = text + 7;
            add_text(out, "thinking", th, end ? (size_t)(end - th) : strlen(th));
            text = end ? end + 8 : "";
            while (*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t') text++;
        }
        if (text[0]) {
            add_text(out, "text", text, strlen(text));
            cJSON *tb = cJSON_CreateObject();
            cJSON_AddStringToObject(tb, "type", "text");
            cJSON_AddStringToObject(tb, "text", text);
            cJSON_AddItemToArray(out->content, tb);
        }
    }

    cJSON *tc;
    cJSON_ArrayForEach(tc, tool_calls) {
        cJSON *func = cJSON_GetObjectItem(tc, "function");
        const char *name = j_str(func, "name");
        if (!name || !name[0]) continue;
        char id[64];
        const char *tc_id = j_str(tc, "id");
        if (tc_id && tc_id[0]) {
            snprintf(id, sizeof(id), "%s", tc_id);
        } else {   /* some servers omit ids: the tool results still need one */
            char rnd[10];
            uuid_short(rnd, 8);
            snprintf(id, sizeof(id), "call_%s", rnd);
        }
        char *args = tool_arguments(cJSON_GetObjectItem(func, "arguments"));
        if (!args) continue;

        cJSON *tu = cJSON_CreateObject();
        cJSON *input = cJSON_Parse(args);
        cJSON_AddStringToObject(tu, "type", "tool_use");
        cJSON_AddStringToObject(tu, "id", id);
        cJSON_AddStringToObject(tu, "name", name);
        cJSON_AddItemToObject(tu, "input", cJSON_IsObject(input) ? input : cJSON_CreateObject());
        if (!cJSON_IsObject(input)) cJSON_Delete(input);
        cJSON_AddItemToArray(out->content, tu);
        add_tool_call(out, id, name, args);
    }

    const char *stop = out->n_tools > 0 ? "tool_use" :
                       (finish && strcmp(finish, "length") == 0) ? "max_tokens" :
                       "end_turn";
    snprintf(out->stop_reason, sizeof(out->stop_reason), "%s", stop);

    cJSON_Delete(root);
    return 0;
}

int llm_call(provider_ref_t *prov, const char *system_prompt,
             cJSON *messages, cJSON *tools, llm_response_t *out) {
    memset(out, 0, sizeof(*out));
    int rc = strcmp(prov->provider_type, "anthropic") == 0
        ? call_anthropic(prov, system_prompt, messages, tools, out)
        : call_openai(prov, system_prompt, messages, tools, out);
    if (rc != 0)
        log_error("LLM %s/%s: %s", prov->provider_name, prov->model, out->error);
    return rc;
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
    cJSON_Delete(r->content);
    memset(r, 0, sizeof(*r));
}
