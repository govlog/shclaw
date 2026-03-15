#include "tc_plugin.h"

const char *TC_PLUGIN_NAME = "hostname_writer";
const char *TC_PLUGIN_DESC = "Ecrit le hostname dans /tmp/test";

static char result[256];

const char *tc_execute(const char *input) {
    (void)input;
    char host[128];
    if (tc_gethostname(host, sizeof(host)) != 0)
        return "error: gethostname failed";
    int len = tc_strlen(host);
    if (tc_write_file("/tmp/test", host, len) != 0)
        return "error: write failed";
    tc_snprintf(result, sizeof(result),
                "hostname '%s' ecrit dans /tmp/test", host);
    return result;
}
