/*
 * irc_parse.c — @mention parsing and agent routing
 */

#include "../include/tc.h"

static int is_agent(const char *name, const char agents[][32], int n_agents) {
    for (int i = 0; i < n_agents; i++)
        if (strcasecmp(name, agents[i]) == 0) return 1;
    return 0;
}

int parse_mentions(const char *msg,
                   const char agents[][32], int n_agents,
                   const char *hub,
                   mention_t *out, int max_out) {
    if (!msg || !msg[0] || max_out <= 0) return 0;

    /* First pass: find all @mentions and their positions */
    struct { int pos; char name[32]; } mentions[16];
    int n_mentions = 0;

    const char *p = msg;
    while (*p && n_mentions < 16) {
        if (*p == '@') {
            /* Extract word after @ */
            const char *start = p + 1;
            const char *end = start;
            while (*end && *end != ' ' && *end != '\t' && *end != '@'
                   && *end != ',' && *end != ':' && *end != '.')
                end++;

            int len = (int)(end - start);
            if (len > 0 && len < 32) {
                char name[32];
                memcpy(name, start, len);
                name[len] = '\0';

                if (strcasecmp(name, "all") == 0 || is_agent(name, agents, n_agents)) {
                    mentions[n_mentions].pos = (int)(p - msg);
                    snprintf(mentions[n_mentions].name, 32, "%s", name);
                    /* Lowercase the agent name for matching */
                    for (char *c = mentions[n_mentions].name; *c; c++)
                        if (*c >= 'A' && *c <= 'Z') *c += 32;
                    n_mentions++;
                }
            }
            p = end;
        } else {
            p++;
        }
    }

    /* No mentions → everything goes to hub */
    if (n_mentions == 0) {
        snprintf(out[0].agent, sizeof(out[0].agent), "%s", hub);
        snprintf(out[0].text, sizeof(out[0].text), "%s", msg);
        return 1;
    }

    /* @all → broadcast entire message */
    for (int i = 0; i < n_mentions; i++) {
        if (strcmp(mentions[i].name, "all") == 0) {
            snprintf(out[0].agent, sizeof(out[0].agent), "all");
            /* Strip the @all from the text */
            char clean[TC_IRC_LINE_MAX] = "";
            int ci = 0;
            const char *s = msg;
            while (*s && ci < TC_IRC_LINE_MAX - 1) {
                if (s == msg + mentions[i].pos) {
                    /* Skip @all */
                    s += strlen("@all") + 1;
                    while (*s == ' ') s++;
                    continue;
                }
                clean[ci++] = *s++;
            }
            clean[ci] = '\0';
            /* Trim */
            while (ci > 0 && (clean[ci-1] == ' ' || clean[ci-1] == '\t'))
                clean[--ci] = '\0';
            snprintf(out[0].text, sizeof(out[0].text), "%s",
                     clean[0] ? clean : msg);
            return 1;
        }
    }

    /* Multiple mentions: split text between them */
    int count = 0;
    for (int i = 0; i < n_mentions && count < max_out; i++) {
        int start_pos = mentions[i].pos;
        int end_pos;

        if (i + 1 < n_mentions)
            end_pos = mentions[i + 1].pos;
        else
            end_pos = (int)strlen(msg);

        /* Text starts after "@name " */
        const char *text_start = msg + start_pos + 1 + strlen(mentions[i].name);
        while (*text_start == ' ' || *text_start == '\t' || *text_start == ':')
            text_start++;

        int text_len = (int)(msg + end_pos - text_start);
        /* Trim trailing spaces */
        while (text_len > 0 && (text_start[text_len-1] == ' ' || text_start[text_len-1] == '\t'))
            text_len--;

        if (text_len <= 0) continue;

        snprintf(out[count].agent, sizeof(out[count].agent), "%s", mentions[i].name);
        int copy_len = text_len < TC_IRC_LINE_MAX - 1 ? text_len : TC_IRC_LINE_MAX - 1;
        memcpy(out[count].text, text_start, copy_len);
        out[count].text[copy_len] = '\0';
        count++;
    }

    /* Text before first mention goes to hub */
    if (mentions[0].pos > 0 && count < max_out) {
        int pre_len = mentions[0].pos;
        while (pre_len > 0 && (msg[pre_len-1] == ' ' || msg[pre_len-1] == '\t'))
            pre_len--;
        if (pre_len > 0) {
            /* Shift existing entries */
            if (count > 0)
                memmove(&out[1], &out[0], count * sizeof(mention_t));
            snprintf(out[0].agent, sizeof(out[0].agent), "%s", hub);
            int copy_len = pre_len < TC_IRC_LINE_MAX - 1 ? pre_len : TC_IRC_LINE_MAX - 1;
            memcpy(out[0].text, msg, copy_len);
            out[0].text[copy_len] = '\0';
            count++;
        }
    }

    return count;
}
