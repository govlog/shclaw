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

#define PROMPT_TRIGGER_HEADER     "## Trigger\nType: %s\n\n"
#define PROMPT_OBJECTIVES_HEADER  "## Objectives\n%s\n"
#define PROMPT_AGENTS_HEADER      "## Other agents\n%s\n"
#define PROMPT_SCHEDULE_HEADER    "## Scheduled tasks\n%s\n"
#define PROMPT_FACTS_HEADER       "## Facts\n%s\n"
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

#define PROMPT_NOW                "## Now\n%s\n\n"

#define PROMPT_NONE               "None.\n"

/* ── Trigger content (the user turn) ───────────────────── */

#define PROMPT_EMPTY_TRIGGER      "(empty message)"
#define PROMPT_TASK_DUE \
    "It is time for a task you scheduled earlier: %s\n" \
    "Do it now (for a reminder: remind the owner in text). " \
    "It must not be scheduled again."
#define PROMPT_MESSAGE_FROM       "Message from %s:\n%s"
#define PROMPT_REPLY_FROM         "Answer from %s:\n%s"

/* ── Communication rules ───────────────────────────────── */

#define PROMPT_COMM_AGENT_MSG \
    "## Communication (request from another agent)\n" \
    "Another agent asked you something. Do it, then answer in text: " \
        "your final text reply is sent back to it automatically.\n" \
    "- Use send_message only to involve a third agent.\n" \
    "- NEVER send acknowledgments like \"OK\", \"Received\", \"Done\". " \
        "These trigger unnecessary work.\n\n"

/* Used when every message received is an answer to an earlier request */
#define PROMPT_COMM_AGENT_REPLY \
    "## Communication (answer from another agent)\n" \
    "This answers a request you sent earlier. Give the owner the result " \
        "in text, then stop.\n" \
    "- Do NOT reply to that agent or thank it.\n\n"

#define PROMPT_COMM_DIRECT \
    "## Communication\n" \
    "To REPLY TO THE OWNER: respond directly in text.\n" \
    "To contact ANOTHER AGENT: use send_message.\n" \
    "Do NOT use send_message(to='owner') — it is redundant.\n\n"

/* Follows the comm rules when the reply is relayed to IRC. */
#define PROMPT_IRC_FORMAT \
    "## IRC Output Format\n" \
    "You are an IRC assistant. Your text reply is relayed to a chat channel.\n" \
    "Hard rule: fit your answer on ONE short line whenever possible " \
    "(<= 200 chars, no newline).\n" \
    "- No bullet lists, no headings, no markdown, no code blocks.\n" \
    "- One concise sentence beats a paragraph.\n" \
    "- If the answer truly needs more, split into the fewest possible " \
    "short lines (each still standalone).\n" \
    "- Drop greetings, sign-offs, and 'let me know if...' filler.\n\n"

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
    "- Use only the functions listed in the template. " \
        "No libc, no system headers.\n" \
    "- Never simulate, mock or hard-code results: fetch real data with " \
        "tc_http_get from a public API that needs no key (e.g. " \
        "https://wttr.in/<city>?format=3 for weather, " \
        "https://api.frankfurter.dev/v1/latest?from=EUR for currency rates). " \
        "If no such API exists, say so.\n" \
    "- If compilation fails, fix the code and retry.\n" \
    "- Pass test_input to create_plugin to see your plugin run.\n" \
    "- Do not describe your next step: call create_plugin.\n" \
    "- When done, answer in text with the plugin name and its " \
        "parameters: the answer goes to whoever asked.\n" \
    "\n## Plugin template\n```c\n%s```\n\n"

/* ── Builder nudges / stall messages ───────────────────── */

#define PROMPT_BUILDER_NUDGE \
    "Do not narrate. Call create_plugin now with the complete, " \
    "corrected code, or say in one sentence why you cannot."

#define PROMPT_BUILDER_AUTO_FAIL \
    "Auto-compilation failed:\n%.3000s\n" \
    "Call create_plugin with corrected code."

/* ── Hub nudge ─────────────────────────────────────────── */

#define PROMPT_HUB_NUDGE \
    "You called list_agents but did not call send_message. " \
    "Do not narrate — call send_message now to delegate."

/* ── Harness guards (small models loop, invent tools, send bad JSON) ── */

#define PROMPT_EMPTY_NUDGE \
    "Your last reply was empty. Reply in text, or call a tool."

#define PROMPT_BAD_ARGS \
    "Error: the arguments of %s are not a valid JSON object. " \
    "Call it again with a JSON object that matches its schema."

#define PROMPT_UNKNOWN_TOOL \
    "Error: unknown tool '%s'. Available tools:"

#define PROMPT_REPEAT_CALL \
    "Error: %s was already called with these exact arguments. " \
    "Use the earlier result, or do something else."

#define PROMPT_ELIDED \
    "\n[... %zu bytes of old tool output removed to fit the context budget]"

/* ── Notices to the owner ──────────────────────────────── */

#define PROMPT_LLM_ERROR          "(LLM error: %s)"
#define PROMPT_NO_ANSWER          "(%s finished without an answer)"
#define PROMPT_FAILED_ANSWER      "(%s could not finish the request)"
#define PROMPT_REFUSED            "(the model declined to answer)"
#define PROMPT_STUCK              "(stopped: the model kept repeating the same tool call)"
#define PROMPT_MAX_TURNS          "(stopped after %d turns without finishing)"

/* ── Prompt assembly ───────────────────────────────────── */

/* The system prompt is fixed for the agent: with the tools, it is the
 * prefix that providers cache across turns and sessions. Anything that
 * changes between sessions goes to the first user turn instead. */
#define PROMPT_SYSTEM_FMT \
    PROMPT_IDENTITY \
    "%s"  /* hub role (or empty) */ \
    "%s"  /* builder rules (or empty) */ \
    PROMPT_OBJECTIVES_HEADER \
    PROMPT_AGENTS_HEADER \
    PROMPT_RULES

/* The first user turn, from the most stable part to the most volatile:
 * servers reuse their cache up to the first byte that differs from an
 * earlier request, and OpenAI only when little follows that byte. */
#define PROMPT_FIRST_TURN_FMT \
    PROMPT_TRIGGER_HEADER \
    "%s"  /* comm rules */ \
    "%s"  /* IRC format (or empty) */ \
    PROMPT_FACTS_HEADER \
    PROMPT_SCHEDULE_HEADER \
    PROMPT_MEMORIES_HEADER \
    PROMPT_NOW \
    "## Message\n%s"

#endif /* PROMPT_H */
