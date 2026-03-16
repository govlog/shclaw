#include "tc_plugin.h"

/*
 * Template plugin for the builder agent.
 * Files prefixed with '_' are ignored by the plugin scanner.
 */

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
