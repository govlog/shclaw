#include "tc_plugin.h"

/*
 * Template plugin for the builder agent.
 * Files prefixed with '_' are ignored by the plugin scanner.
 *
 * == Quick reference (commonly used functions) ==
 *
 * HTTP:
 *   int tc_http_get(const char *url, char *buf, size_t buf_sz);
 *   int tc_http_post(const char *url, const char *content_type,
 *                    const char *body, size_t body_len,
 *                    char *resp, size_t resp_sz);
 *   int tc_http_post_json(const char *url, const char *json,
 *                         char *resp, size_t resp_sz);
 *   void tc_http_header(const char *name, const char *value);
 *
 * JSON:
 *   void *tc_json_parse(const char *json);
 *   void  tc_json_free(void *json);
 *   void *tc_json_get(void *json, const char *key);
 *   const char *tc_json_string(void *node);
 *   int   tc_json_int(void *node);
 *
 * Strings:
 *   int   tc_snprintf(char *buf, size_t sz, const char *fmt, ...);
 *   int   tc_strlen(const char *s);
 *   int   tc_strcmp(const char *a, const char *b);
 *   char *tc_strstr(const char *haystack, const char *needle);
 *   char *tc_strchr(const char *s, int c);
 *   int   tc_atoi(const char *s);
 *
 * Files:
 *   int tc_read_file(const char *path, char *buf, size_t buf_sz);
 *   int tc_write_file(const char *path, const char *data, size_t len);
 *
 * See include/tc_plugin.h for the full list.
 */

/* ── Example 1: echo text (JSON input) ── */

const char *TC_PLUGIN_NAME = "example";
const char *TC_PLUGIN_DESC = "Example plugin template";
const char *TC_PLUGIN_SCHEMA =
    "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Text to echo\"}},\"required\":[\"text\"]}";

static char result[1024];

const char *tc_execute(const char *input_json) {
    void *json = tc_json_parse(input_json);
    const char *text;

    if (!json)
        return "error: invalid json";

    text = tc_json_string(tc_json_get(json, "text"));
    if (!text) {
        tc_json_free(json);
        return "error: missing text";
    }

    tc_snprintf(result, sizeof(result), "%s", text);
    tc_json_free(json);
    return result;
}

/* ── Example 2: HTTP GET (uncomment to use as base) ──
 *
 * const char *TC_PLUGIN_NAME = "fetch_example";
 * const char *TC_PLUGIN_DESC = "Fetch a URL and return body";
 * const char *TC_PLUGIN_SCHEMA =
 *     "{\"type\":\"object\",\"properties\":{},\"required\":[]}";
 *
 * static char buf[4096];
 * static char result2[4200];
 *
 * const char *tc_execute(const char *input_json) {
 *     (void)input_json;
 *     int status = tc_http_get("http://example.com", buf, sizeof(buf));
 *     if (status < 0)
 *         return "error: HTTP request failed";
 *     tc_snprintf(result2, sizeof(result2), "%s", buf);
 *     return result2;
 * }
 */
