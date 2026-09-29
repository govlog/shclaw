# Plugin API

A plugin is a single `.c` file, usually written by an agent at runtime. TinyCC compiles it in memory: no `.so` ever reaches the disk.

<img src="images/irc-builder.png" alt="The builder creates a weather plugin on IRC" width="820">

## Structure

Every plugin includes `tc_plugin.h` and exports four things:

```c
#include "tc_plugin.h"

const char *TC_PLUGIN_NAME = "weather";
const char *TC_PLUGIN_DESC = "Get current weather for a city";
const char *TC_PLUGIN_SCHEMA =
    "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\",\"description\":\"City name\"}},\"required\":[\"city\"]}";

const char *tc_execute(const char *input_json) {
    void *json = tc_json_parse(input_json);
    const char *city = tc_json_string(tc_json_get(json, "city"));
    if (!city)
        return "error: missing city";

    char url[256];
    tc_snprintf(url, sizeof(url), "https://wttr.in/%s?format=3", city);

    static char result[512];
    int status = tc_http_get(url, result, sizeof(result));
    return (status == 200) ? result : "error: weather service unavailable";
}
```

The builder's template is [`plugins/_template.c`](../plugins/_template.c). The plugin scanner ignores files whose name starts with `_`.

## Names

`TC_PLUGIN_NAME` becomes the tool name: letters, digits, `_` and `-`, at most 64 characters. It must not be a built-in tool name or the name of another plugin file. shclaw refuses a plugin that breaks these rules, since one bad tool name would make every API call fail.

## Runtime

- **No libc.** Plugins are compiled with `-nostdlib`, without system headers. The daemon injects the `tc_*` functions with `tcc_add_symbol()`. Common libc names (`strlen`, `snprintf`, `strcat`, `malloc`...) are macros for their `tc_*` versions; `printf`, `sprintf` and `fopen` do not exist. Zeroed arrays, struct copies, divisions and 64-bit arithmetic work on every platform.
- **One forked child per call,** with a 180-second timeout. A crash or an endless loop fails the call, not the daemon. Memory is released when the call ends, so `tc_free()` and `tc_json_free()` are optional.
- **HTTP** goes through the daemon's BearSSL stack, so plugins get HTTPS for free. `tc_http_get()` follows redirects and percent-encodes spaces and non-ASCII bytes in the URL.
- **Not a sandbox:** a plugin is native code with the daemon's rights.

## Testing

`create_plugin` takes an optional `test_input` (a JSON object, sent as text or as an object). After compiling, the daemon runs the plugin once with it and returns the output with a trace of each HTTP call: URL, status, start of the body, and a note when the body did not fit the buffer. A crash, a timeout, an empty output or an output starting with `error` makes `create_plugin` fail, so the builder fixes the code. Compile errors come back with the source line of each error.

## Available functions

| Category | Functions |
|----------|-----------|
| Memory | `tc_malloc`, `tc_free` |
| Strings | `tc_strlen`, `tc_strcmp`, `tc_strncmp`, `tc_strcpy`, `tc_strncpy`, `tc_strcat`, `tc_strncat`, `tc_strdup`, `tc_snprintf`, `tc_memcpy`, `tc_memset`, `tc_memcmp`, `tc_strstr`, `tc_strchr`, `tc_strrchr`, `tc_atoi` |
| Characters | `tc_isdigit`, `tc_isalpha`, `tc_isspace`, `tc_tolower`, `tc_toupper` (ASCII) |
| Files | `tc_read_file`, `tc_write_file` |
| HTTP | `tc_http_get`, `tc_http_post`, `tc_http_post_json`, `tc_http_header` |
| JSON | `tc_json_parse`, `tc_json_free`, `tc_json_print`, `tc_json_get`, `tc_json_index`, `tc_json_array_size`, `tc_json_string`, `tc_json_int`, `tc_json_double` |
| System | `tc_gethostname` |
| Logging | `tc_log` |

Full declarations: [`include/tc_plugin.h`](../include/tc_plugin.h).
