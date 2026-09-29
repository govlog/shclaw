#include "tc_plugin.h"

/*
 * Template plugin for the builder agent.
 * Files prefixed with '_' are ignored by the plugin scanner.
 *
 * == Every function available (no libc, no other header) ==
 *
 * HTTP (https works; returns the HTTP status, or < 0 on network error):
 *   int  tc_http_get(const char *url, char *buf, size_t buf_sz);
 *   int  tc_http_post(const char *url, const char *content_type,
 *                     const char *body, size_t body_len,
 *                     char *resp, size_t resp_sz);
 *   int  tc_http_post_json(const char *url, const char *json,
 *                          char *resp, size_t resp_sz);
 *   void tc_http_header(const char *name, const char *value); (next call only)
 *
 * JSON (handles are void *; strings point INTO the parsed tree):
 *   void *tc_json_parse(const char *json);
 *   void  tc_json_free(void *json);           optional, see below
 *   void *tc_json_get(void *json, const char *key);   NULL if missing
 *   void *tc_json_index(void *json, int index);       array element
 *   int   tc_json_array_size(void *json);
 *   const char *tc_json_string(void *node);           NULL if not a string
 *   int    tc_json_int(void *node);
 *   double tc_json_double(void *node);
 *   char  *tc_json_print(void *json);
 *
 * Strings and memory:
 *   int   tc_snprintf(char *buf, size_t sz, const char *fmt, ...);
 *   int   tc_strlen(const char *s);
 *   int   tc_strcmp(const char *a, const char *b);
 *   int   tc_strncmp(const char *a, const char *b, size_t n);
 *   char *tc_strcpy(char *dst, const char *src);
 *   char *tc_strncpy(char *dst, const char *src, size_t n);
 *   char *tc_strcat(char *dst, const char *src);
 *   char *tc_strncat(char *dst, const char *src, size_t n);
 *   char *tc_strstr(const char *haystack, const char *needle);
 *   char *tc_strchr(const char *s, int c);
 *   char *tc_strrchr(const char *s, int c);
 *   char *tc_strdup(const char *s);
 *   int   tc_memcmp(const void *a, const void *b, size_t n);
 *   int   tc_atoi(const char *s);
 *   int   tc_isdigit(int c), tc_isalpha(int c), tc_isspace(int c);
 *   int   tc_tolower(int c), tc_toupper(int c);
 *   void *tc_memcpy(void *dst, const void *src, size_t n);
 *   void *tc_memset(void *s, int c, size_t n);
 *   void *tc_malloc(size_t sz);  void tc_free(void *ptr);
 *   (strlen, snprintf, strcmp... also work as aliases;
 *    printf, sprintf and fopen do NOT exist)
 *
 * System:
 *   int  tc_read_file(const char *path, char *buf, size_t buf_sz);
 *   int  tc_write_file(const char *path, const char *data, size_t len);
 *   int  tc_gethostname(char *buf, size_t sz);
 *   void tc_log(const char *fmt, ...);
 *
 * Return a static buffer (not a local array) from tc_execute.
 * Each call runs in its own short-lived process: memory is released when
 * it ends, so tc_free() and tc_json_free() are optional (and harmless).
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

/* ── Example 2: HTTP GET built from a parameter, JSON answer ──
 *
 * const char *TC_PLUGIN_NAME = "wiki_summary";
 * const char *TC_PLUGIN_DESC = "Short Wikipedia summary of a topic";
 * const char *TC_PLUGIN_SCHEMA =
 *     "{\"type\":\"object\",\"properties\":{\"topic\":{\"type\":\"string\",\"description\":\"Topic\"}},\"required\":[\"topic\"]}";
 *
 * static char buf[16384];
 * static char result2[1024];
 *
 * const char *tc_execute(const char *input_json) {
 *     void *json = tc_json_parse(input_json);
 *     const char *topic = tc_json_string(tc_json_get(json, "topic"));
 *     char url[512];
 *
 *     if (!topic) {
 *         tc_json_free(json);
 *         return "error: missing topic";
 *     }
 *     // Build the URL before tc_json_free(): topic points into json.
 *     // Spaces and accents in the URL are encoded for you.
 *     tc_snprintf(url, sizeof(url),
 *                 "https://en.wikipedia.org/api/rest_v1/page/summary/%s", topic);
 *     tc_json_free(json);
 *
 *     int status = tc_http_get(url, buf, sizeof(buf));
 *     if (status != 200) {
 *         tc_snprintf(result2, sizeof(result2), "error: HTTP %d", status);
 *         return result2;
 *     }
 *     // This API answers JSON. Plain-text APIs (e.g. wttr.in with
 *     // ?format=3): return buf as it is, never tc_json_parse it.
 *     void *answer = tc_json_parse(buf);
 *     const char *extract = tc_json_string(tc_json_get(answer, "extract"));
 *     tc_snprintf(result2, sizeof(result2), "%s", extract ? extract : "no summary");
 *     tc_json_free(answer);
 *     return result2;
 * }
 */
