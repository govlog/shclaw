/*
 * irc.c — IRC client over TLS (single nick, multiplexed agents)
 */

#include "../include/tc.h"
#include <bearssl.h>

/* From ca.c */
extern br_x509_trust_anchor *ca_get_anchors(size_t *count);

typedef struct {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
} irc_tls_t;

static int irc_sock_read(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, 300000); /* 5 min timeout for IRC */
    if (r <= 0) return -1;
    return (int)read(fd, buf, len);
}

static int irc_sock_write(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    return (int)write(fd, buf, len);
}

static int irc_write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void irc_sendf(irc_t *irc, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    buf[n++] = '\r';
    buf[n++] = '\n';

    irc_tls_t *tls = irc->tls_state;
    if (tls) {
        br_sslio_write_all(&tls->ioc, buf, n);
        br_sslio_flush(&tls->ioc);
    } else {
        irc_write_all(irc->fd, buf, (size_t)n);
    }
}

int irc_connect(irc_t *irc, const char *host, int port) {
    /* TCP connect */
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        log_error("IRC: cannot resolve %s", host);
        return -1;
    }

    irc->fd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        irc->fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (irc->fd < 0) continue;
        if (connect(irc->fd, r->ai_addr, r->ai_addrlen) == 0) break;
        close(irc->fd);
        irc->fd = -1;
    }
    freeaddrinfo(res);

    if (irc->fd < 0) {
        log_error("IRC: cannot connect to %s:%d", host, port);
        return -1;
    }

    /* TLS if port 6697 */
    if (port == 6697) {
        irc_tls_t *tls = calloc(1, sizeof(irc_tls_t));
        size_t anchor_count;
        br_x509_trust_anchor *anchors = ca_get_anchors(&anchor_count);

        if (!anchors || anchor_count == 0) {
            log_error("IRC: no CA anchors for TLS");
            close(irc->fd);
            free(tls);
            return -1;
        }

        ssl_client_init_minimal(&tls->sc, &tls->xc, anchors, anchor_count);
        br_ssl_engine_set_buffer(&tls->sc.eng, tls->iobuf, sizeof(tls->iobuf), 1);
        br_ssl_client_reset(&tls->sc, host, 0);
        br_sslio_init(&tls->ioc, &tls->sc.eng,
                      irc_sock_read, &irc->fd, irc_sock_write, &irc->fd);
        irc->tls_state = tls;
    }

    /* IRC registration */
    irc_sendf(irc, "NICK %s", irc->nick);
    irc_sendf(irc, "USER %s 0 * :shclaw", irc->nick);

    log_info("IRC: connecting to %s:%d as %s", host, port, irc->nick);
    return 0;
}

int irc_fd(irc_t *irc) {
    return irc->fd;
}

static void handle_line(irc_t *irc, const char *line) {
    log_debug("IRC< %s", line);

    /* PING → PONG */
    if (strncmp(line, "PING ", 5) == 0) {
        irc_sendf(irc, "PONG %s", line + 5);
        return;
    }

    /* Check for 001 (RPL_WELCOME) → join channel */
    if (strstr(line, " 001 ")) {
        log_info("IRC: registered, joining %s", irc->channel);
        if (irc->channel_key[0])
            irc_sendf(irc, "JOIN %s %s", irc->channel, irc->channel_key);
        else
            irc_sendf(irc, "JOIN %s", irc->channel);
        return;
    }

    /* JOIN (self or others) */
    if (strstr(line, " JOIN ") || strstr(line, " JOIN :")) {
        const char *p = line;
        if (*p == ':') p++;
        const char *bang = strchr(p, '!');
        if (bang) {
            char joiner[64];
            int jlen = (int)(bang - p);
            if (jlen > 63) jlen = 63;
            memcpy(joiner, p, jlen);
            joiner[jlen] = '\0';

            if (strcmp(joiner, irc->nick) == 0) {
                /* We joined — set +k if we have a key */
                if (irc->channel_key[0]) {
                    irc_sendf(irc, "MODE %s +k %s", irc->channel, irc->channel_key);
                    log_info("IRC: set channel key on %s", irc->channel);
                }
            } else if (irc->owner[0] && strcmp(joiner, irc->owner) == 0) {
                /* Owner joined — give them +o */
                irc_sendf(irc, "MODE %s +o %s", irc->channel, irc->owner);
                log_info("IRC: opped owner %s", irc->owner);
            }
        }
        return;
    }

    /* PRIVMSG */
    /* :nick!user@host PRIVMSG #channel :message text */
    if (!strstr(line, "PRIVMSG")) return;

    const char *p = line;
    if (*p != ':') return;
    p++;

    /* Extract sender nick */
    const char *bang = strchr(p, '!');
    if (!bang) return;
    char from[64];
    int from_len = (int)(bang - p);
    if (from_len > 63) from_len = 63;
    memcpy(from, p, from_len);
    from[from_len] = '\0';

    /* Find PRIVMSG */
    const char *privmsg = strstr(line, "PRIVMSG ");
    if (!privmsg) return;
    privmsg += 8;

    /* Target (channel or nick) */
    const char *space = strchr(privmsg, ' ');
    if (!space) return;

    /* Skip " :" to get message text */
    const char *text = space + 1;
    if (*text == ':') text++;

    /* Only accept from owner */
    if (irc->owner[0] && strcmp(from, irc->owner) != 0) {
        log_debug("IRC: ignoring message from %s (not owner %s)", from, irc->owner);
        return;
    }

    /* Route via @mention parsing */
    if (irc->on_trigger) {
        mention_t mentions[8];
        int n = parse_mentions(text, (const char (*)[32])irc->agents, irc->n_agents,
                               irc->hub, mentions, 8);
        for (int i = 0; i < n; i++)
            irc->on_trigger(mentions[i].agent, from, mentions[i].text, irc->ctx);
    }
}

int irc_poll(irc_t *irc) {
    char buf[512];
    int n;

    irc_tls_t *tls = irc->tls_state;
    if (tls) {
        n = br_sslio_read(&tls->ioc, buf, sizeof(buf) - 1);
    } else {
        n = read(irc->fd, buf, sizeof(buf) - 1);
    }

    if (n <= 0) return -1;
    buf[n] = '\0';

    /* Append to read buffer and process complete lines */
    int space = (int)sizeof(irc->readbuf) - irc->readbuf_len - 1;
    if (n > space) n = space;
    memcpy(irc->readbuf + irc->readbuf_len, buf, n);
    irc->readbuf_len += n;
    irc->readbuf[irc->readbuf_len] = '\0';

    /* Process lines */
    char *start = irc->readbuf;
    char *eol;
    while ((eol = strstr(start, "\r\n")) != NULL) {
        *eol = '\0';
        if (start[0])
            handle_line(irc, start);
        start = eol + 2;
    }

    /* Move remaining partial line to start of buffer */
    int remaining = irc->readbuf_len - (int)(start - irc->readbuf);
    if (remaining > 0 && start != irc->readbuf)
        memmove(irc->readbuf, start, remaining);
    irc->readbuf_len = remaining;

    return 0;
}

void irc_reply(irc_t *irc, const char *agent_name, const char *text) {
    if (!irc || !text || !text[0]) return;

    /* Broadcast to TUI clients regardless of IRC state */
    if (irc->ctx)
        daemon_tui_broadcast((daemon_t *)irc->ctx, agent_name, text);

    /* Send to IRC if connected */
    if (irc->fd < 0) return;

    /* Split on newlines, prefix each with "agent: " */
    char line[512];
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        int len = eol ? (int)(eol - p) : (int)strlen(p);
        if (len > 400) len = 400;
        if (len > 0) {
            snprintf(line, sizeof(line), "%s: %.*s", agent_name, len, p);
            irc_sendf(irc, "PRIVMSG %s :%s", irc->channel, line);
        }
        p += len;
        if (*p == '\n') p++;
    }
}

void irc_disconnect(irc_t *irc) {
    if (irc->fd >= 0) {
        irc_sendf(irc, "QUIT :shclaw shutting down");
        irc_tls_t *tls = irc->tls_state;
        if (tls) {
            br_ssl_engine_close(&tls->sc.eng);
            free(tls);
            irc->tls_state = NULL;
        }
        close(irc->fd);
        irc->fd = -1;
    }
}
