/*
 * irc.c — IRC client over TLS (single nick, multiplexed agents)
 */

#include "../include/tc.h"
#include <bearssl.h>

typedef struct {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
} irc_tls_t;

/* Agent threads reply while the main loop reads: every use of the socket
 * and of the TLS engine goes through this lock. */
static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;

static int irc_sock_read(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r;
    do r = poll(&pfd, 1, 30000); while (r < 0 && errno == EINTR);
    if (r <= 0) return -1;
    ssize_t n = read(fd, buf, len);
    return n > 0 ? (int)n : -1;
}

__attribute__((format(printf, 2, 3)))
static void irc_sendf(irc_t *irc, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;  /* 512 with CRLF */
    /* A CR or LF coming from model or web text would start a new command */
    for (int i = 0; i < n; i++)
        if (buf[i] == '\r' || buf[i] == '\n') buf[i] = ' ';
    buf[n++] = '\r';
    buf[n++] = '\n';

    pthread_mutex_lock(&io_lock);
    irc_tls_t *tls = irc->tls_state;
    if (irc->fd >= 0) {
        if (tls) {
            br_sslio_write_all(&tls->ioc, buf, (size_t)n);
            br_sslio_flush(&tls->ioc);
        } else {
            write_all(irc->fd, buf, (size_t)n);
        }
    }
    pthread_mutex_unlock(&io_lock);
}

int irc_connect(irc_t *irc, const char *host, int port) {
    int fd = net_connect(host, port, TC_CONNECT_TIMEOUT);
    if (fd < 0) {
        log_error("IRC: cannot connect to %s:%d", host, port);
        return -1;
    }

    /* TLS if port 6697 */
    irc_tls_t *tls = NULL;
    if (port == 6697) {
        tls = calloc(1, sizeof(irc_tls_t));
        if (!tls || tls_client_start(&tls->sc, &tls->xc, &tls->ioc, tls->iobuf,
                                     sizeof(tls->iobuf), host, &irc->fd,
                                     irc_sock_read, net_write) != 0) {
            log_error("IRC: no CA anchors for TLS");
            close(fd);
            free(tls);
            return -1;
        }
    }

    pthread_mutex_lock(&io_lock);
    irc->fd = fd;
    irc->tls_state = tls;
    irc->readbuf_len = 0;
    irc->last_rx = now_ms();
    irc->pinged = 0;
    irc->registered = 0;
    snprintf(irc->nick, sizeof(irc->nick), "%s", irc->base_nick);
    pthread_mutex_unlock(&io_lock);

    /* IRC registration (the TLS handshake happens on this first write) */
    irc_sendf(irc, "NICK %s", irc->nick);
    irc_sendf(irc, "USER %s 0 * :shclaw", irc->nick);

    if (tls && br_ssl_engine_current_state(&tls->sc.eng) == BR_SSL_CLOSED) {
        log_error("IRC: TLS handshake with %s failed (error %d)",
                  host, br_ssl_engine_last_error(&tls->sc.eng));
        irc_disconnect(irc);
        return -1;
    }

    log_info("IRC: connecting to %s:%d as %s", host, port, irc->nick);
    return 0;
}

/* Split ":nick!user@host CMD params" in place; returns CMD or NULL */
static char *parse_line(char *line, char **nick, char **params) {
    *nick = "";
    if (*line == ':') {
        char *sp = strchr(line, ' ');
        if (!sp) return NULL;
        *sp = '\0';
        *nick = line + 1;
        char *bang = strchr(*nick, '!');
        if (bang) *bang = '\0';
        line = sp + 1;
        while (*line == ' ') line++;
    }
    char *sp = strchr(line, ' ');
    if (sp) {
        *sp = '\0';
        *params = sp + 1;
    } else {
        *params = line + strlen(line);
    }
    return line[0] ? line : NULL;
}

static void handle_line(irc_t *irc, char *line) {
    log_debug("IRC< %s", line);

    char *nick, *params;
    char *cmd = parse_line(line, &nick, &params);
    if (!cmd) return;

    if (strcmp(cmd, "PING") == 0) {
        irc_sendf(irc, "PONG %s", params);
    } else if (strcmp(cmd, "001") == 0) {        /* RPL_WELCOME */
        irc->registered = 1;
        log_info("IRC: registered, joining %s", irc->channel);
        if (irc->channel_key[0])
            irc_sendf(irc, "JOIN %s %s", irc->channel, irc->channel_key);
        else
            irc_sendf(irc, "JOIN %s", irc->channel);
    } else if (strcmp(cmd, "433") == 0) {        /* ERR_NICKNAMEINUSE */
        /* Stay short: servers truncate nicks past NICKLEN (often 9-16),
         * which would make every retry the same nick */
        size_t len = strlen(irc->nick);
        if (len < 9) {
            irc->nick[len] = '_';
            irc->nick[len + 1] = '\0';
        } else {
            char c = irc->nick[8];
            irc->nick[8] = c >= '0' && c < '9' ? (char)(c + 1) : '0';
            irc->nick[9] = '\0';
        }
        log_warn("IRC: nick in use, trying %s", irc->nick);
        irc_sendf(irc, "NICK %s", irc->nick);
    } else if (strcmp(cmd, "JOIN") == 0) {
        if (strcasecmp(nick, irc->nick) == 0) {
            /* We joined — set +k if we have a key */
            if (irc->channel_key[0]) {
                irc_sendf(irc, "MODE %s +k %s", irc->channel, irc->channel_key);
                log_info("IRC: set channel key on %s", irc->channel);
            }
        } else if (irc->owner[0] && strcasecmp(nick, irc->owner) == 0) {
            irc_sendf(irc, "MODE %s +o %s", irc->channel, irc->owner);
            log_info("IRC: opped owner %s", irc->owner);
        }
    } else if (strcmp(cmd, "PRIVMSG") == 0) {
        /* params: "<target> :<text>" */
        char *text = strstr(params, " :");
        if (!text) return;
        text += 2;

        /* Only accept from owner (IRC nicks are case-insensitive) */
        if (irc->owner[0] && strcasecmp(nick, irc->owner) != 0) {
            log_debug("IRC: ignoring message from %s (not owner %s)", nick, irc->owner);
            return;
        }
        if (text[0] == '\x01' || !irc->on_trigger)  /* CTCP: VERSION, ACTION... */
            return;

        mention_t mentions[8];
        int n = parse_mentions(text, (const char (*)[32])irc->agents, irc->n_agents,
                               irc->hub, mentions, 8);
        for (int i = 0; i < n; i++)
            irc->on_trigger(mentions[i].agent, nick, mentions[i].text, irc->ctx);
    }
}

/* Read what is available (caller holds io_lock) */
static int read_some(irc_t *irc, char *buf, size_t sz) {
    irc_tls_t *tls = irc->tls_state;
    if (irc->fd < 0) return -1;
    if (tls) return br_sslio_read(&tls->ioc, buf, sz);
    ssize_t n = read(irc->fd, buf, sz);
    return n > 0 ? (int)n : -1;
}

int irc_poll(irc_t *irc) {
    for (;;) {
        char buf[2048];
        pthread_mutex_lock(&io_lock);
        int n = read_some(irc, buf, sizeof(buf));
        irc_tls_t *tls = irc->tls_state;
        /* Decrypted data can stay buffered in BearSSL, where poll() on the
         * socket cannot see it: drain it now. */
        int more = n > 0 && tls &&
                   (br_ssl_engine_current_state(&tls->sc.eng) & BR_SSL_RECVAPP);
        pthread_mutex_unlock(&io_lock);
        if (n <= 0) return -1;

        irc->last_rx = now_ms();
        irc->pinged = 0;

        /* A line longer than the buffer is not IRC: drop it */
        if (n > (int)sizeof(irc->readbuf) - irc->readbuf_len - 1)
            irc->readbuf_len = 0;
        memcpy(irc->readbuf + irc->readbuf_len, buf, (size_t)n);
        irc->readbuf_len += n;

        char *start = irc->readbuf;
        char *end = irc->readbuf + irc->readbuf_len;
        char *eol;
        while ((eol = memchr(start, '\n', (size_t)(end - start))) != NULL) {
            *eol = '\0';
            if (eol > start && eol[-1] == '\r') eol[-1] = '\0';
            if (start[0]) handle_line(irc, start);
            start = eol + 1;
        }
        irc->readbuf_len = (int)(end - start);
        memmove(irc->readbuf, start, (size_t)irc->readbuf_len);

        if (!more) return 0;
    }
}

int irc_keepalive(irc_t *irc) {
    if (irc->fd < 0) return 0;
    int64_t idle = now_ms() - irc->last_rx;
    if (idle > 420000)          /* silent even after our PING */
        return -1;
    if (idle > 240000 && !irc->pinged) {
        irc_sendf(irc, "PING :shclaw");
        irc->pinged = 1;
    }
    return 0;
}

void irc_reply(irc_t *irc, const char *agent_name, const char *text) {
    if (!irc || !text || !text[0]) return;

    /* Broadcast to TUI clients regardless of IRC state */
    if (irc->ctx)
        daemon_tui_broadcast((daemon_t *)irc->ctx, agent_name, text);

    if (irc->fd < 0) return;

    /* One PRIVMSG per line, prefixed "agent: " except for the hub */
    const char *prefix = (agent_name && agent_name[0] &&
                          strcmp(agent_name, irc->hub) != 0) ? agent_name : NULL;
    int lines = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        size_t chunk = len > 400 ? 400 : len;
        /* Never split a UTF-8 sequence */
        while (chunk > 1 && chunk < len && ((unsigned char)p[chunk] & 0xC0) == 0x80)
            chunk--;
        if (chunk > 0) {
            if (++lines > 20) {
                irc_sendf(irc, "PRIVMSG %s :%s%s[... truncated, full reply in the TUI and logs]",
                          irc->channel, prefix ? prefix : "", prefix ? ": " : "");
                return;
            }
            if (lines > 4)
                sleep(1);   /* stay under the server's flood limit */
            irc_sendf(irc, "PRIVMSG %s :%s%s%.*s", irc->channel,
                      prefix ? prefix : "", prefix ? ": " : "", (int)chunk, p);
        }
        p += chunk;
        if (chunk == len && *p == '\n') p++;
    }
}

void irc_action(irc_t *irc, const char *agent_name, const char *text) {
    if (!irc || !text || !text[0]) return;

    char line[512];
    snprintf(line, sizeof(line), "* %s %s", agent_name ? agent_name : "system", text);

    if (irc->ctx)
        daemon_tui_broadcast((daemon_t *)irc->ctx, NULL, line);

    if (irc->fd < 0) return;
    irc_sendf(irc, "PRIVMSG %s :\x01" "ACTION %.400s\x01", irc->channel, line + 2);
}

void irc_disconnect(irc_t *irc) {
    if (irc->fd < 0) return;
    irc_sendf(irc, "QUIT :shclaw shutting down");
    pthread_mutex_lock(&io_lock);
    irc_tls_t *tls = irc->tls_state;
    if (tls) {
        br_ssl_engine_close(&tls->sc.eng);
        free(tls);
        irc->tls_state = NULL;
    }
    close(irc->fd);
    irc->fd = -1;
    pthread_mutex_unlock(&io_lock);
}
