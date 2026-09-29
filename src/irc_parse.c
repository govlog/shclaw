/*
 * irc_parse.c — @mention parsing and agent routing
 */

#include "../include/tc.h"

/* Index of the agent named by s[0..len) (case-insensitive), -1 if none */
static int find_agent(const char *s, size_t len, const char agents[][32], int n) {
    for (int i = 0; i < n; i++)
        if (strlen(agents[i]) == len && strncasecmp(s, agents[i], len) == 0)
            return i;
    return -1;
}

static int is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

static void set_text(mention_t *m, const char *s, size_t len) {
    while (len > 0 && (*s == ' ' || *s == '\t' || *s == ':' || *s == ',')) {
        s++;
        len--;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
        len--;
    if (len >= sizeof(m->text)) len = sizeof(m->text) - 1;
    memcpy(m->text, s, len);
    m->text[len] = '\0';
}

int parse_mentions(const char *msg,
                   const char agents[][32], int n_agents,
                   const char *hub,
                   mention_t *out, int max_out) {
    if (!msg || !msg[0] || max_out <= 0) return 0;
    size_t msg_len = strlen(msg);

    /* Mentions of known agents or @all; "user@oracle.com" is not one */
    struct { const char *at, *end; char name[32]; } m[16];
    int n = 0;
    for (const char *p = msg; *p && n < 16; p++) {
        if (*p != '@' || (p > msg && is_word_char(p[-1]))) continue;
        const char *s = p + 1, *e = s;
        while (*e && !strchr(" \t@,:.!?;", *e)) e++;
        size_t len = (size_t)(e - s);
        int idx = find_agent(s, len, agents, n_agents);
        int all = len == 3 && strncasecmp(s, "all", 3) == 0;
        if (idx < 0 && !all) continue;
        m[n].at = p;
        m[n].end = e;
        /* Canonical name from the config, whatever the case typed */
        snprintf(m[n].name, sizeof(m[n].name), "%s", all ? "all" : agents[idx]);
        n++;
        p = e - 1;
    }

    /* No mentions → everything goes to hub */
    if (n == 0) {
        snprintf(out[0].agent, sizeof(out[0].agent), "%s", hub);
        set_text(&out[0], msg, msg_len);
        return 1;
    }

    /* @all → every agent gets the whole message, minus the mention */
    for (int i = 0; i < n; i++) {
        if (strcmp(m[i].name, "all") != 0) continue;
        char clean[TC_IRC_LINE_MAX];
        snprintf(clean, sizeof(clean), "%.*s %s", (int)(m[i].at - msg), msg, m[i].end);
        snprintf(out[0].agent, sizeof(out[0].agent), "all");
        set_text(&out[0], clean, strlen(clean));
        if (!out[0].text[0])
            set_text(&out[0], msg, msg_len);
        return 1;
    }

    /* Text before the first mention goes to the hub. Each agent gets the
     * text up to the next mention; an empty segment, as in
     * "@oracle @builder do X", takes the next non-empty one. */
    int count = 0;
    if (m[0].at > msg) {
        snprintf(out[0].agent, sizeof(out[0].agent), "%s", hub);
        set_text(&out[0], msg, (size_t)(m[0].at - msg));
        if (out[0].text[0]) count = 1;
    }
    int pending = count;
    for (int i = 0; i < n && count < max_out; i++) {
        const char *seg_end = i + 1 < n ? m[i + 1].at : msg + msg_len;
        snprintf(out[count].agent, sizeof(out[count].agent), "%s", m[i].name);
        set_text(&out[count], m[i].end, (size_t)(seg_end - m[i].end));
        count++;
        if (out[count - 1].text[0]) {
            for (int j = pending; j < count - 1; j++)
                memcpy(out[j].text, out[count - 1].text, sizeof(out[j].text));
            pending = count;
        }
    }
    return pending;   /* trailing mentions without any text are dropped */
}
