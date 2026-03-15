/*
 * tui.c — terminal UI client for shclaw
 *
 * Connects via unix socket, provides a readline-style chat interface.
 * Usage: shclaw tui [agent]
 */

#include "../include/tc.h"

/* ── Terminal handling ── */

static struct termios orig_termios;
static int term_raw = 0;
static void term_write(const char *buf, size_t len);

static void term_restore(void) {
    if (term_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        term_raw = 0;
    }
    /* Show cursor */
    term_write("\033[?25h", 6);
}

static void term_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    term_raw = 1;
    atexit(term_restore);
}

static void get_terminal_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

/* ── ANSI helpers ── */

#define CSI        "\033["
#define CLEAR_LINE CSI "2K"
#define BOLD       CSI "1m"
#define DIM        CSI "2m"
#define RESET      CSI "0m"
#define CYAN       CSI "36m"
#define GREEN      CSI "32m"

static void term_write(const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (n == 0) return;
        buf += n;
        len -= (size_t)n;
    }
}

static void cursor_to(int row, int col) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), CSI "%d;%dH", row, col);
    term_write(buf, (size_t)n);
}

/* ── Scrollback buffer ── */

#define MAX_LINES    1024
#define MAX_LINE_LEN 512

typedef struct {
    char lines[MAX_LINES][MAX_LINE_LEN];
    int count;
    int scroll_offset;
} scrollback_t;

static void sb_add(scrollback_t *sb, const char *line) {
    if (sb->count >= MAX_LINES) {
        memmove(sb->lines[0], sb->lines[1], (MAX_LINES - 1) * MAX_LINE_LEN);
        sb->count = MAX_LINES - 1;
    }
    snprintf(sb->lines[sb->count], MAX_LINE_LEN, "%s", line);
    sb->count++;
    sb->scroll_offset = 0;
}

/* ── TUI state ── */

typedef struct {
    scrollback_t sb;
    char input[512];
    int input_len;
    int input_pos;
    int rows, cols;
    char target[64];
    char sock_path[4200];
    int event_fd;       /* persistent ATTACH connection for receiving events */
    int quit;
} tui_state_t;

/* ── Socket path resolution ── */

static void resolve_sock_path(tui_state_t *st) {
    ini_t *cfg = ini_load("etc/config.ini");
    if (cfg) {
        const char *data_dir = ini_get(cfg, "daemon", "data_dir");
        snprintf(st->sock_path, sizeof(st->sock_path), "%s/shclaw.sock",
                 data_dir ? data_dir : "./data");
        ini_free(cfg);
    } else {
        memcpy(st->sock_path, "./data/shclaw.sock", sizeof("./data/shclaw.sock"));
    }
}

/* ── Drawing ── */

static void draw_status_bar(tui_state_t *st) {
    cursor_to(1, 1);
    term_write(CLEAR_LINE, strlen(CLEAR_LINE));

    char bar[512];
    int n = snprintf(bar, sizeof(bar),
             CSI "7m" " shclaw │ @%s │ %d msgs │ /help " RESET,
             st->target, st->sb.count);
    term_write(bar, (size_t)n);
}

static void draw_messages(tui_state_t *st) {
    int msg_rows = st->rows - 2;
    int total = st->sb.count;
    int start = total - msg_rows - st->sb.scroll_offset;
    if (start < 0) start = 0;
    int end = start + msg_rows;
    if (end > total) end = total;

    for (int row = 0; row < msg_rows; row++) {
        cursor_to(row + 2, 1);
        term_write(CLEAR_LINE, strlen(CLEAR_LINE));

        int idx = start + row;
        if (idx < end) {
            const char *line = st->sb.lines[idx];
            term_write(line, strlen(line));
        }
    }
}

static void draw_input(tui_state_t *st) {
    cursor_to(st->rows, 1);
    term_write(CLEAR_LINE, strlen(CLEAR_LINE));

    char prompt[128];
    int n = snprintf(prompt, sizeof(prompt), GREEN "@%s> " RESET, st->target);
    term_write(prompt, (size_t)n);
    if (st->input_len > 0)
        term_write(st->input, (size_t)st->input_len);

    /* Position cursor after prompt + input_pos */
    int prompt_visible = 1 + (int)strlen(st->target) + 2;  /* @name>_ */
    cursor_to(st->rows, prompt_visible + st->input_pos + 1);
}

static void redraw(tui_state_t *st) {
    term_write("\033[?25l", 6);  /* hide cursor */
    draw_status_bar(st);
    draw_messages(st);
    draw_input(st);
    term_write("\033[?25h", 6);  /* show cursor */
}

/* ── Message formatting ── */

static void add_system_msg(tui_state_t *st, const char *text) {
    char line[MAX_LINE_LEN];
    snprintf(line, sizeof(line), DIM "--- %s ---" RESET, text);
    sb_add(&st->sb, line);
}

static void add_user_msg(tui_state_t *st, const char *text) {
    char line[MAX_LINE_LEN];
    snprintf(line, sizeof(line), GREEN "you" RESET ": %s", text);
    sb_add(&st->sb, line);
}

/* ── Socket I/O ── */

static int send_to_agent(tui_state_t *st, const char *text) {
    /* Send MSG on a separate one-shot connection */
    int fd = sock_client_connect(st->sock_path);
    if (fd < 0) return -1;

    char data[TC_BUF_LG];
    size_t alen = strlen(st->target);
    size_t text_len = strlen(text);
    size_t total = alen + 1 + text_len;
    if (total >= sizeof(data)) {
        close(fd);
        return -1;
    }

    memcpy(data, st->target, alen);
    data[alen] = '\0';
    memcpy(data + alen + 1, text, text_len);

    if (sock_send_cmd(fd, SOCK_CMD_MSG, data, (uint32_t)total) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static void add_agent_line(tui_state_t *st, const char *line) {
    /* line is already "agent: text" from daemon broadcast */
    char formatted[MAX_LINE_LEN];
    /* Color the agent name part */
    const char *colon = strchr(line, ':');
    if (colon) {
        int nlen = (int)(colon - line);
        snprintf(formatted, sizeof(formatted), CYAN "%.*s" RESET "%s",
                 nlen, line, colon);
    } else {
        snprintf(formatted, sizeof(formatted), "%s", line);
    }
    sb_add(&st->sb, formatted);
}

static void poll_events(tui_state_t *st) {
    if (st->event_fd < 0) return;

    struct pollfd pfd = { .fd = st->event_fd, .events = POLLIN };
    while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        uint32_t type;
        char data[TC_BUF_LG] = "";
        if (sock_read_cmd(st->event_fd, &type, data, sizeof(data)) < 0) {
            /* Connection lost */
            close(st->event_fd);
            st->event_fd = -1;
            add_system_msg(st, "Lost connection to daemon");
            return;
        }

        switch (type) {
        case SOCK_EVT_LINE:
            add_agent_line(st, data);
            break;
        case SOCK_EVT_GOODBYE:
            add_system_msg(st, "Daemon shutting down");
            close(st->event_fd);
            st->event_fd = -1;
            return;
        }
    }

    if (pfd.revents & (POLLHUP | POLLERR)) {
        close(st->event_fd);
        st->event_fd = -1;
        add_system_msg(st, "Daemon disconnected");
    }
}

static void fetch_status(tui_state_t *st) {
    int fd = sock_client_connect(st->sock_path);
    if (fd < 0) {
        add_system_msg(st, "Cannot connect to daemon");
        return;
    }
    if (sock_send_cmd(fd, SOCK_CMD_STATUS, NULL, 0) != 0) {
        add_system_msg(st, "Status request failed");
        close(fd);
        return;
    }
    uint32_t type;
    char data[TC_BUF_LG];
    if (sock_read_cmd(fd, &type, data, sizeof(data)) >= 0)
        add_system_msg(st, data);
    else
        add_system_msg(st, "No response");
    close(fd);
}

/* ── Input handling ── */

static void handle_submit(tui_state_t *st) {
    if (st->input_len == 0) return;
    st->input[st->input_len] = '\0';

    /* Slash commands */
    if (st->input[0] == '/') {
        if (strcmp(st->input, "/quit") == 0 || strcmp(st->input, "/q") == 0) {
            st->quit = 1;
        } else if (strncmp(st->input, "/agent ", 7) == 0) {
            snprintf(st->target, sizeof(st->target), "%s", st->input + 7);
            char msg[128];
            snprintf(msg, sizeof(msg), "Target changed to @%s", st->target);
            add_system_msg(st, msg);
        } else if (strcmp(st->input, "/status") == 0) {
            fetch_status(st);
        } else if (strcmp(st->input, "/help") == 0) {
            add_system_msg(st, "Commands:");
            add_system_msg(st, "  /agent <name>    Switch target agent");
            add_system_msg(st, "  /status          Show daemon status");
            add_system_msg(st, "  /quit or /q      Exit");
            add_system_msg(st, "  Up/Down          Scroll messages");
            add_system_msg(st, "  Ctrl-C/D         Exit");
        } else {
            add_system_msg(st, "Unknown command. Type /help");
        }
        st->input_len = 0;
        st->input_pos = 0;
        return;
    }

    /* Send message */
    add_user_msg(st, st->input);
    if (send_to_agent(st, st->input) < 0)
        add_system_msg(st, "Send failed (daemon not running?)");

    st->input_len = 0;
    st->input_pos = 0;
}

static void handle_key(tui_state_t *st, const char *seq, int n) {
    /* Single byte */
    if (n == 1) {
        unsigned char c = seq[0];
        switch (c) {
        case 3: case 4:  /* Ctrl-C, Ctrl-D */
            st->quit = 1;
            return;
        case 13: case 10:  /* Enter */
            handle_submit(st);
            return;
        case 127: case 8:  /* Backspace */
            if (st->input_pos > 0) {
                memmove(st->input + st->input_pos - 1,
                        st->input + st->input_pos,
                        st->input_len - st->input_pos);
                st->input_pos--;
                st->input_len--;
            }
            return;
        case 1:  /* Ctrl-A: home */
            st->input_pos = 0;
            return;
        case 5:  /* Ctrl-E: end */
            st->input_pos = st->input_len;
            return;
        case 21:  /* Ctrl-U: clear line */
            st->input_len = 0;
            st->input_pos = 0;
            return;
        case 11:  /* Ctrl-K: kill to end */
            st->input_len = st->input_pos;
            return;
        case 12:  /* Ctrl-L: force redraw */
            term_write(CSI "2J", 4);
            return;
        default:
            if (c >= 32 && c < 127 && st->input_len < (int)sizeof(st->input) - 1) {
                memmove(st->input + st->input_pos + 1,
                        st->input + st->input_pos,
                        st->input_len - st->input_pos);
                st->input[st->input_pos] = c;
                st->input_pos++;
                st->input_len++;
            }
            return;
        }
    }

    /* Escape sequences: ESC [ X */
    if (n >= 3 && seq[0] == 27 && seq[1] == '[') {
        switch (seq[2]) {
        case 'A':  /* Up — scroll */
            if (st->sb.scroll_offset < st->sb.count - (st->rows - 2))
                st->sb.scroll_offset++;
            break;
        case 'B':  /* Down — scroll */
            if (st->sb.scroll_offset > 0)
                st->sb.scroll_offset--;
            break;
        case 'C':  /* Right */
            if (st->input_pos < st->input_len)
                st->input_pos++;
            break;
        case 'D':  /* Left */
            if (st->input_pos > 0)
                st->input_pos--;
            break;
        case 'H':  /* Home */
            st->input_pos = 0;
            break;
        case 'F':  /* End */
            st->input_pos = st->input_len;
            break;
        case '5':  /* Page Up */
            if (n >= 4 && seq[3] == '~') {
                st->sb.scroll_offset += st->rows - 3;
                int max = st->sb.count - (st->rows - 2);
                if (max < 0) max = 0;
                if (st->sb.scroll_offset > max) st->sb.scroll_offset = max;
            }
            break;
        case '6':  /* Page Down */
            if (n >= 4 && seq[3] == '~') {
                st->sb.scroll_offset -= st->rows - 3;
                if (st->sb.scroll_offset < 0) st->sb.scroll_offset = 0;
            }
            break;
        case '3':  /* Delete */
            if (n >= 4 && seq[3] == '~' && st->input_pos < st->input_len) {
                memmove(st->input + st->input_pos,
                        st->input + st->input_pos + 1,
                        st->input_len - st->input_pos - 1);
                st->input_len--;
            }
            break;
        }
    }
}

/* ── Entry point ── */

int tui_run(const char *target_agent) {
    tui_state_t st = {0};
    st.event_fd = -1;
    snprintf(st.target, sizeof(st.target), "%s",
             target_agent ? target_agent : "jarvis");
    resolve_sock_path(&st);

    /* Establish persistent ATTACH connection */
    st.event_fd = sock_client_connect(st.sock_path);
    if (st.event_fd < 0) {
        fprintf(stderr, "Cannot connect to daemon at %s\n", st.sock_path);
        fprintf(stderr, "Is shclaw running?\n");
        return 1;
    }
    if (sock_send_cmd(st.event_fd, SOCK_CMD_ATTACH, NULL, 0) != 0) {
        close(st.event_fd);
        fprintf(stderr, "Cannot attach to daemon at %s\n", st.sock_path);
        return 1;
    }

    get_terminal_size(&st.rows, &st.cols);
    term_raw_mode();

    /* Clear screen */
    term_write(CSI "2J", 4);

    add_system_msg(&st, "Connected to shclaw");
    char welcome[128];
    snprintf(welcome, sizeof(welcome),
             "Target: @%s — Type /help for commands", st.target);
    add_system_msg(&st, welcome);

    while (!st.quit) {
        /* Poll for daemon events */
        poll_events(&st);

        redraw(&st);

        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
        poll(&pfd, 1, 100);

        if (pfd.revents & POLLIN) {
            char buf[32];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0)
                handle_key(&st, buf, (int)n);
        }

        /* Track terminal resize */
        int nr, nc;
        get_terminal_size(&nr, &nc);
        if (nr != st.rows || nc != st.cols) {
            st.rows = nr;
            st.cols = nc;
        }
    }

    /* Detach cleanly */
    if (st.event_fd >= 0) {
        sock_send_cmd(st.event_fd, SOCK_CMD_DETACH, NULL, 0);
        close(st.event_fd);
    }

    term_restore();
    term_write(CSI "2J" CSI "H", 7);
    printf("Bye.\n");
    return 0;
}
