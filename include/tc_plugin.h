/*
 * tc_plugin.h — Public API for shclaw plugins
 *
 * Plugins are .c files in the plugins/ directory. They are compiled at
 * runtime by TinyCC in-memory (no libc, no system headers).
 *
 * ONLY include this header. Do NOT include <stdio.h> or any system header.
 * All needed types and functions are provided below.
 *
 * Required exports:
 *   const char *TC_PLUGIN_NAME  — unique tool name
 *   const char *TC_PLUGIN_DESC  — one-line description
 *
 * Optional exports:
 *   const char *TC_PLUGIN_SCHEMA — JSON string defining input_schema
 *     Example: "{"type":"object","properties":{"a":{"type":"number","description":"First number"},"b":{"type":"number","description":"Second number"}},"required":["a","b"]}"
 *     If not provided, an empty properties object is used.
 *     Best practice: always export it, even for tools with no inputs.
 *
 * Required:
 *   const char *tc_execute(const char *input_json)
 *     input_json is a JSON object with the tool's parameters.
 *     Return a static or malloc'd string with the result.
 *
 * Recommended scaffold:
 *   #include "tc_plugin.h"
 *   const char *TC_PLUGIN_NAME = "example";
 *   const char *TC_PLUGIN_DESC = "Example plugin";
 *   const char *TC_PLUGIN_SCHEMA =
 *       "{\"type\":\"object\",\"properties\":{},\"required\":[]}";
 *   static char result[1024];
 *   const char *tc_execute(const char *input_json) { ... }
 *
 * Best practices:
 *   - Read parameters from input_json with tc_json_* helpers.
 *   - Use only tc_* functions declared in this header.
 *   - Prefer static result buffers for returned strings.
 */

#ifndef TC_PLUGIN_H
#define TC_PLUGIN_H

/* ── Basic types (no libc needed) ── */

typedef unsigned long size_t;
typedef long ssize_t;
#define NULL ((void *)0)

/* ── String/memory helpers (injected by daemon) ── */

void *tc_malloc(size_t sz);
void  tc_free(void *ptr);
int   tc_strlen(const char *s);
void *tc_memcpy(void *dst, const void *src, size_t n);
void *tc_memset(void *s, int c, size_t n);
int   tc_strcmp(const char *a, const char *b);
int   tc_strncmp(const char *a, const char *b, size_t n);
char *tc_strcpy(char *dst, const char *src);
char *tc_strncpy(char *dst, const char *src, size_t n);
int   tc_snprintf(char *buf, size_t sz, const char *fmt, ...);

char *tc_strcat(char *dst, const char *src);
char *tc_strncat(char *dst, const char *src, size_t n);
char *tc_strdup(const char *s);
int   tc_memcmp(const void *a, const void *b, size_t n);

/* Search */
char *tc_strstr(const char *haystack, const char *needle);
char *tc_strchr(const char *s, int c);
char *tc_strrchr(const char *s, int c);
int   tc_atoi(const char *s);

/* Characters (ASCII) */
int tc_isdigit(int c);
int tc_isalpha(int c);
int tc_isspace(int c);
int tc_tolower(int c);
int tc_toupper(int c);

/* ── Syscall helpers (injected by daemon) ── */

/* Read entire file into buf. Returns bytes read or -1. */
int tc_read_file(const char *path, char *buf, size_t buf_sz);

/* Write data to file (creates/overwrites). Returns 0 on success. */
int tc_write_file(const char *path, const char *data, size_t len);

/* Get hostname into buf. Returns 0 on success. */
int tc_gethostname(char *buf, size_t sz);

/* ── HTTP ── */

/* GET url, write response body into buf. Returns HTTP status or -1. */
int tc_http_get(const char *url, char *buf, size_t buf_sz);

/* POST with content-type + body. Returns HTTP status or -1. */
int tc_http_post(const char *url, const char *content_type,
                 const char *body, size_t body_len,
                 char *resp, size_t resp_sz);

/* POST JSON string. Returns HTTP status or -1. */
int tc_http_post_json(const char *url, const char *json,
                      char *resp, size_t resp_sz);

/* Set an extra header for the next HTTP call (e.g. Authorization). */
void tc_http_header(const char *name, const char *value);

/* ── JSON (cJSON wrappers) ── */

/* Parse a JSON string. Returns an opaque handle. */
void *tc_json_parse(const char *json);

/* Free a parsed JSON object. Optional: each plugin call runs in its own
 * process and all its memory is released when the call ends. */
void  tc_json_free(void *json);

/* Pretty-print JSON into a new string. */
char *tc_json_print(void *json);

/* Get object field by name. Returns NULL if not found. */
void *tc_json_get(void *json, const char *key);

/* Get array element by index. Returns NULL if out of range. */
void *tc_json_index(void *json, int index);

/* Get array length. */
int   tc_json_array_size(void *json);

/* Get string value from JSON node. Returns NULL if not a string. */
const char *tc_json_string(void *json);

/* Get integer value from JSON node. Returns 0 if not a number. */
int tc_json_int(void *json);

/* Get double value from JSON node. Returns 0.0 if not a number. */
double tc_json_double(void *json);

/* ── Logging ── */

/* Log a message (appears in daemon log). printf-style format. */
void tc_log(const char *fmt, ...);

/* ── libc names ── */

/* Code written out of habit with these libc names still compiles: they
 * map to the tc_* functions above. No other libc function exists here. */
#define malloc   tc_malloc
#define free     tc_free
#define strlen   tc_strlen
#define memcpy   tc_memcpy
#define memset   tc_memset
#define strcmp   tc_strcmp
#define strncmp  tc_strncmp
#define strcpy   tc_strcpy
#define strncpy  tc_strncpy
#define snprintf tc_snprintf
#define strstr   tc_strstr
#define strchr   tc_strchr
#define strrchr  tc_strrchr
#define strcat   tc_strcat
#define strncat  tc_strncat
#define strdup   tc_strdup
#define memcmp   tc_memcmp
#define atoi     tc_atoi

#ifndef __bool_true_false_are_defined
#define bool  _Bool
#define true  1
#define false 0
#define __bool_true_false_are_defined 1
#endif

#endif /* TC_PLUGIN_H */
