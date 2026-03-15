
#include "tc_plugin.h"

const char *TC_PLUGIN_NAME = "myip";
const char *TC_PLUGIN_DESC = "Fetches public IP from ip.wtf and returns it";

const char *tc_execute(const char *input_json) {
    static char buf[1024];
    static char result[1100];

    tc_memset(buf, 0, sizeof(buf));
    tc_memset(result, 0, sizeof(result));

    int status = tc_http_get("https://ip.wtf", buf, sizeof(buf));

    if (status < 0) {
        tc_snprintf(result, sizeof(result), "Error: HTTP request failed (status %d)", status);
        return result;
    }

    /* Trim trailing newline if present */
    int len = tc_strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }

    tc_snprintf(result, sizeof(result), "%s", buf);
    return result;
}
