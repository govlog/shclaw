#include "tc_plugin.h"

const char *TC_PLUGIN_NAME = "dummy";
const char *TC_PLUGIN_DESC = "Plugin dummy qui dit yolo";
const char *TC_PLUGIN_SCHEMA =
    "{\"type\":\"object\",\"properties\":{},\"required\":[]}";

static char result[16];

const char *tc_execute(const char *input_json) {
    (void)input_json;
    tc_snprintf(result, sizeof(result), "yolo");
    return result;
}
