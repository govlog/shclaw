/*
 * prompt.h — all LLM prompt strings (system prompts, nudges, labels)
 *
 * Centralised here so they are easy to find, translate, and maintain.
 * Every string is a #define so it can be embedded in format strings.
 */

#ifndef PROMPT_H
#define PROMPT_H

/* ── System prompt skeleton ────────────────────────────── */

#define PROMPT_IDENTITY           "You are %s.\n%s\n\n%s\n"

#define PROMPT_HUB_ROLE \
    "## Hub Role\n" \
    "You are the main agent. Analyse requests and delegate when needed.\n" \
    "Do not describe what you WILL do — do it. " \
    "If you delegate, call send_message immediately.\n\n"

#define PROMPT_TRIGGER_HEADER     "## Trigger\nType: %s\n%s\n\n"
#define PROMPT_THREAD_HEADER      "## Active thread: %s\n"
#define PROMPT_OBJECTIVES_HEADER  "## Objectives\n%s\n"
#define PROMPT_AGENTS_HEADER      "## Other agents\n%s\n"
#define PROMPT_SCHEDULE_HEADER    "## Scheduled tasks\n%s\n"
#define PROMPT_MEMORIES_HEADER    "## Recent memories\n%s\n"

#define PROMPT_RULES \
    "## Rules\n" \
    "1. Be concise. Finish quickly if nothing to do.\n" \
    "2. No spending without authorisation.\n" \
    "3. ALWAYS show the result of a tool call to the user. " \
        "Never call a tool and end your turn without reporting " \
        "the result in your reply.\n" \
    "4. If a tool returns an error, empty output, or " \
        "\"" TC_EMPTY_OUTPUT_MARKER "\", report it as-is.\n" \
    "5. Never invent an expected result or fabricate missing output.\n\n"

#define PROMPT_NOW                "## Now\n%s\nAct.\n"

#define PROMPT_NONE               "None.\n"
#define PROMPT_NO_DATA            "(no data)"
#define PROMPT_STARTUP_FALLBACK   "Daemon STARTUP. Act."

/* ── Communication rules ───────────────────────────────── */

#define PROMPT_COMM_AGENT_MSG \
    "## Communication (inter-agent message)\n" \
    "You received a message from another agent.\n" \
    "- If it is a status update, completion notice, or acknowledgment: " \
        "inform the owner in text and STOP. Do NOT reply to the sender.\n" \
    "- If it asks you to do something: do it, then reply with send_message.\n" \
    "- NEVER send acknowledgments like \"OK\", \"Received\", \"Done\" " \
        "back to an agent. These trigger unnecessary work.\n" \
    "- Do NOT relay an agent's response back to the same agent.\n\n"

#define PROMPT_COMM_DIRECT \
    "## Communication\n" \
    "To REPLY TO THE OWNER: respond directly in text.\n" \
    "To contact ANOTHER AGENT: use send_message.\n" \
    "Do NOT use send_message(to='owner') — it is redundant.\n\n"

/* ── Builder rules (format: one %s for template content) ─ */

#define PROMPT_BUILDER_RULES \
    "## Builder Workflow\n" \
    "- The template below contains signatures and examples.\n" \
    "- Do NOT call read_file for _template.c or tc_plugin.h.\n" \
    "- Read a similar existing plugin if useful.\n" \
    "- Once a file has been read in this session, do not re-read it.\n" \
    "- To read local files, prefer read_file over exec(cat ...).\n" \
    "- Write a single C file with #include \"tc_plugin.h\", " \
        "TC_PLUGIN_NAME, TC_PLUGIN_DESC, TC_PLUGIN_SCHEMA and " \
        "tc_execute(const char *input_json).\n" \
    "- Use only tc_* functions from the header. " \
        "No libc, no system headers.\n" \
    "\n## HTTP signatures (exact)\n" \
    "  int tc_http_get(const char *url, char *buf, size_t buf_sz);\n" \
    "  int tc_http_post(const char *url, const char *content_type,\n" \
    "                   const char *body, size_t body_len,\n" \
    "                   char *resp, size_t resp_sz);\n" \
    "  int tc_http_post_json(const char *url, const char *json,\n" \
    "                        char *resp, size_t resp_sz);\n" \
    "\n" \
    "- If compilation fails, fix the code and retry.\n" \
    "- Do not describe your next step: call create_plugin, " \
        "or send_message to report failure.\n" \
    "- After success, send ONE send_message to the requesting agent. " \
        "Do NOT also send to owner.\n" \
    "\n## Plugin template\n```c\n%s```\n\n"

/* ── Builder nudges / stall messages ───────────────────── */

#define PROMPT_BUILDER_NUDGE \
    "Plugin creation was requested. Do not narrate. " \
    "Call create_plugin now, or send_message to " \
    "explain why you cannot."

#define PROMPT_BUILDER_AUTO_FAIL \
    "Auto-compilation failed:\n%.3000s\n" \
    "Call create_plugin with corrected code."

/* ── Hub nudge ─────────────────────────────────────────── */

#define PROMPT_HUB_NUDGE \
    "You called list_agents but did not call send_message. " \
    "Do not narrate — call send_message now to delegate."

/* ── System prompt format (all sections assembled) ─────── */

#define PROMPT_SYSTEM_FMT \
    PROMPT_IDENTITY \
    "%s"  /* hub role (or empty) */ \
    "%s"  /* builder rules (or empty) */ \
    PROMPT_TRIGGER_HEADER \
    PROMPT_THREAD_HEADER \
    "%s"  /* comm rules */ \
    PROMPT_OBJECTIVES_HEADER \
    PROMPT_AGENTS_HEADER \
    PROMPT_SCHEDULE_HEADER \
    PROMPT_MEMORIES_HEADER \
    PROMPT_RULES \
    PROMPT_NOW

#endif /* PROMPT_H */
