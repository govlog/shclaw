# Plugin API

Plugins are single `.c` files that agents write at runtime. They get compiled in-memory by TCC -- no `.so` ever hits disk.

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

    char url[256];
    tc_snprintf(url, sizeof(url), "https://wttr.in/%s?format=3", city);

    static char result[512];
    int status = tc_http_get(url, result, sizeof(result));
    tc_json_free(json);

    return (status == 200) ? result : "error";
}
```

The builder template is in [`plugins/_template.c`](../plugins/_template.c). Files starting with `_` are ignored by the plugin scanner.

## Sandbox

Plugins run in `-nostdlib` mode: no libc, no system headers. The daemon injects a set of `tc_*` functions via `tcc_add_symbol()` before compilation. This keeps plugins sandboxed while giving them HTTP+TLS, JSON, and file I/O for free.

## Available functions

| Category | Functions |
|----------|-----------|
| Memory | `tc_malloc`, `tc_free` |
| Strings | `tc_strlen`, `tc_strcmp`, `tc_strncmp`, `tc_strcpy`, `tc_strncpy`, `tc_snprintf`, `tc_memcpy`, `tc_memset`, `tc_strstr`, `tc_strchr`, `tc_atoi` |
| Files | `tc_read_file`, `tc_write_file` |
| HTTP | `tc_http_get`, `tc_http_post`, `tc_http_post_json`, `tc_http_header` |
| JSON | `tc_json_parse`, `tc_json_free`, `tc_json_print`, `tc_json_get`, `tc_json_index`, `tc_json_array_size`, `tc_json_string`, `tc_json_int`, `tc_json_double` |
| System | `tc_gethostname` |
| Logging | `tc_log` |

HTTP calls go through the daemon's BearSSL stack -- plugins get HTTPS for free.

See [`include/tc_plugin.h`](../include/tc_plugin.h) for the full declarations.
