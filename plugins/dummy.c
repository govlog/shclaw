#include "tc_plugin.h"

const char *TC_PLUGIN_NAME = "dummy";
const char *TC_PLUGIN_DESC = "Plugin dummy qui dit yolo";

const char *tc_execute(const char *input) {
    (void)input;
    return "yolo";
}
