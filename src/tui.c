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

static void term_write(const char *buf, size_t len) {
    write_all(STDOUT_FILENO, buf, len);
}

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
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2 && ws.ws_col > 10) {
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
#define DIM        CSI "2m"
#define RESET      CSI "0m"
#define CYAN       CSI "36m"
#define GREEN      CSI "32m"

static void cursor_to(int row, int col) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), CSI "%d;%dH", row, col);
    term_write(buf, (size_t)n);
}

static int is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

/* ── Scrollback buffer ── */

#define MAX_LINES    1024
#define MAX_LINE_LEN 512

typedef struct {
    char lines[MAX_LINES][MAX_LINE_LEN];
    int count;
    int scroll_offset;
} scrollback_t;

static void sb_push(scrollback_t *sb, const char *line) {
    if (sb->count >= MAX_LINES) {
        memmove(sb->lines, sb->lines + 1, (MAX_LINES - 1) * sizeof(sb->lines[0]));
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
    int event_fd;       /* persistent ATTACH connection */
    int quit;
    int dirty;          /* redraw needed */
} tui_state_t;

/* Add text split on newlines and wrapped at the terminal width. The first
 * `hl` bytes are drawn with `style`. Control bytes (escape sequences from
 * model or web text) are neutralized. */
static void add_text(tui_state_t *st, const char *style, size_t hl, const char *text) {
    int width = st->cols - 1;
    size_t len = strlen(text), pos = 0;

    do {
        size_t take = 0;
        int cols = 0;
        while (pos + take < len && text[pos + take] != '\n' &&
               cols < width && take < MAX_LINE_LEN - 64) {
            take++;
            while (pos + take < len && take < MAX_LINE_LEN - 1 &&
                   is_cont((unsigned char)text[pos + take]))
                take++;
            cols++;
        }

        char row[MAX_LINE_LEN], clean[MAX_LINE_LEN];
        for (size_t i = 0; i < take; i++) {
            unsigned char c = (unsigned char)text[pos + i];
            clean[i] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
        }
        size_t h = pos < hl ? (hl - pos < take ? hl - pos : take) : 0;
        if (h && style)
            snprintf(row, sizeof(row), "%s%.*s" RESET "%.*s", style,
                     (int)h, clean, (int)(take - h), clean + h);
        else
            snprintf(row, sizeof(row), "%.*s", (int)take, clean);
        sb_push(&st->sb, row);

        pos += take;
        if (pos < len && text[pos] == '\n') pos++;
    } while (pos < len);
    st->dirty = 1;
}

static void add_system_msg(tui_state_t *st, const char *text) {
    char line[MAX_LINE_LEN];
    snprintf(line, sizeof(line), "--- %s ---", text);
    add_text(st, DIM, strlen(line), line);
}

/* "agent: text" from the daemon, or "* agent calls tool(...)" */
static void add_agent_line(tui_state_t *st, const char *line) {
    if (line[0] == '*') {
        add_text(st, DIM, strlen(line), line);
        return;
    }
    const char *colon = strchr(line, ':');
    add_text(st, CYAN, colon ? (size_t)(colon - line) : 0, line);
}

/* ── Drawing ── */

static void draw_status_bar(tui_state_t *st) {
    cursor_to(1, 1);
    term_write(CLEAR_LINE, strlen(CLEAR_LINE));

    char bar[512];
    int n = snprintf(bar, sizeof(bar),
             CSI "7m" " shclaw │ @%s │ %d lines │ /help " RESET,
             st->target, st->sb.count);
    term_write(bar, (size_t)n);
}

static void draw_messages(tui_state_t *st) {
    int msg_rows = st->rows - 2;
    int total = st->sb.count;
    int start = total - msg_rows - st->sb.scroll_offset;
    if (start < 0) start = 0;

    for (int row = 0; row < msg_rows; row++) {
        cursor_to(row + 2, 1);
        term_write(CLEAR_LINE, strlen(CLEAR_LINE));
        int idx = start + row;
        if (idx < total)
            term_write(st->sb.lines[idx], strlen(st->sb.lines[idx]));
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

    /* Cursor column: prompt + characters (not bytes) before input_pos */
    int col = 1 + (int)strlen(st->target) + 2;
    for (int i = 0; i < st->input_pos; i++)
        if (!is_cont((unsigned char)st->input[i])) col++;
    cursor_to(st->rows, col + 1);
}

static void redraw(tui_state_t *st) {
    term_write("\033[?25l", 6);  /* hide cursor */
    draw_status_bar(st);
    draw_messages(st);
    draw_input(st);
    term_write("\033[?25h", 6);  /* show cursor */
    st->dirty = 0;
}

/* ── Socket I/O ── */

static int send_to_agent(tui_state_t *st, const char *text) {
    /* Format: agent_name\0text, over the attached connection */
    char data[TC_BUF_LG];
    size_t alen = strlen(st->target);
    size_t text_len = strlen(text);
    size_t total = alen + 1 + text_len;
    if (st->event_fd < 0 || total >= sizeof(data))
        return -1;

    memcpy(data, st->target, alen + 1);
    memcpy(data + alen + 1, text, text_len);
    return sock_send(st->event_fd, SOCK_CMD_MSG, data, (uint32_t)total);
}

static void poll_events(tui_state_t *st) {
    struct pollfd pfd = { .fd = st->event_fd, .events = POLLIN };
    while (st->event_fd >= 0 && poll(&pfd, 1, 0) > 0) {
        uint32_t type;
        char data[TC_BUF_LG];
        if (!(pfd.revents & POLLIN) ||
            sock_read_cmd(st->event_fd, &type, data, sizeof(data)) < 0) {
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
}

static void fetch_status(tui_state_t *st) {
    int fd = sock_client_connect();
    uint32_t type;
    char data[TC_BUF_LG];
    if (fd < 0)
        add_system_msg(st, "Cannot connect to daemon");
    else if (sock_send(fd, SOCK_CMD_STATUS, NULL, 0) != 0 ||
             sock_read_cmd(fd, &type, data, sizeof(data)) < 0)
        add_system_msg(st, "No response");
    else
        add_system_msg(st, data);
    if (fd >= 0) close(fd);
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
            add_system_msg(st, "  Up/Down PgUp/PgDn  Scroll messages");
            add_system_msg(st, "  Ctrl-C/D         Exit");
        } else {
            add_system_msg(st, "Unknown command. Type /help");
        }
    } else {
        char line[MAX_LINE_LEN];
        snprintf(line, sizeof(line), "you: %s", st->input);
        add_text(st, GREEN, 3, line);
        if (send_to_agent(st, st->input) < 0)
            add_system_msg(st, "Send failed (daemon not running?)");
    }
    st->input_len = 0;
    st->input_pos = 0;
}

static void delete_range(tui_state_t *st, int from, int to) {
    memmove(st->input + from, st->input + to, (size_t)(st->input_len - to));
    st->input_len -= to - from;
    if (st->input_pos > from) st->input_pos = from;
}

/* Previous / next character boundary (UTF-8) */
static int prev_char(tui_state_t *st, int pos) {
    do pos--; while (pos > 0 && is_cont((unsigned char)st->input[pos]));
    return pos;
}

static int next_char(tui_state_t *st, int pos) {
    do pos++; while (pos < st->input_len && is_cont((unsigned char)st->input[pos]));
    return pos;
}

static void handle_csi(tui_state_t *st, unsigned char key, int tilde) {
    int page = st->rows - 3;
    int max = st->sb.count - (st->rows - 2);
    if (max < 0) max = 0;
    switch (key) {
    case 'A': st->sb.scroll_offset += 1; break;      /* Up */
    case 'B': st->sb.scroll_offset -= 1; break;      /* Down */
    case 'C': if (st->input_pos < st->input_len) st->input_pos = next_char(st, st->input_pos); break;
    case 'D': if (st->input_pos > 0) st->input_pos = prev_char(st, st->input_pos); break;
    case 'H': st->input_pos = 0; break;
    case 'F': st->input_pos = st->input_len; break;
    case '5': if (tilde) st->sb.scroll_offset += page; break;   /* Page Up */
    case '6': if (tilde) st->sb.scroll_offset -= page; break;   /* Page Down */
    case '3':                                                   /* Delete */
        if (tilde && st->input_pos < st->input_len)
            delete_range(st, st->input_pos, next_char(st, st->input_pos));
        break;
    }
    if (st->sb.scroll_offset > max) st->sb.scroll_offset = max;
    if (st->sb.scroll_offset < 0) st->sb.scroll_offset = 0;
}

/* A read may hold one key, an escape sequence, UTF-8 text or a paste */
static void handle_input(tui_state_t *st, const unsigned char *buf, int n) {
    for (int i = 0; i < n && !st->quit; i++) {
        unsigned char c = buf[i];
        if (c == 27) {
            if (i + 2 < n && buf[i + 1] == '[') {
                int tilde = i + 3 < n && buf[i + 3] == '~';
                handle_csi(st, buf[i + 2], tilde);
                i += tilde ? 3 : 2;
            }
            continue;
        }
        switch (c) {
        case 3: case 4:    /* Ctrl-C, Ctrl-D */
            st->quit = 1;
            break;
        case 13: case 10:  /* Enter */
            handle_submit(st);
            break;
        case 127: case 8:  /* Backspace */
            if (st->input_pos > 0)
                delete_range(st, prev_char(st, st->input_pos), st->input_pos);
            break;
        case 1:  st->input_pos = 0; break;              /* Ctrl-A */
        case 5:  st->input_pos = st->input_len; break;  /* Ctrl-E */
        case 21: st->input_len = st->input_pos = 0; break;  /* Ctrl-U */
        case 11: st->input_len = st->input_pos; break;  /* Ctrl-K */
        case 12: term_write(CSI "2J", 4); break;        /* Ctrl-L */
        default:
            if (c >= 32 && st->input_len < (int)sizeof(st->input) - 1) {
                memmove(st->input + st->input_pos + 1, st->input + st->input_pos,
                        (size_t)(st->input_len - st->input_pos));
                st->input[st->input_pos++] = (char)c;
                st->input_len++;
            }
        }
    }
    st->dirty = 1;
}

/* ── Entry point ── */

int tui_run(const char *target_agent) {
    static tui_state_t st;   /* ~512KB of scrollback: not on the stack */
    st.event_fd = -1;
    snprintf(st.target, sizeof(st.target), "%s", target_agent ? target_agent : "jarvis");

    /* Establish persistent ATTACH connection */
    st.event_fd = sock_client_connect();
    if (st.event_fd < 0 || sock_send(st.event_fd, SOCK_CMD_ATTACH, NULL, 0) != 0) {
        fprintf(stderr, "Cannot connect to the daemon. Is shclaw running?\n");
        return 1;
    }

    get_terminal_size(&st.rows, &st.cols);
    term_raw_mode();
    term_write(CSI "2J", 4);

    add_system_msg(&st, "Connected to shclaw");
    char welcome[128];
    snprintf(welcome, sizeof(welcome),
             "Target: @%s — Type /help for commands", st.target);
    add_system_msg(&st, welcome);

    while (!st.quit) {
        if (st.dirty)
            redraw(&st);

        struct pollfd pfd[2] = {
            { .fd = STDIN_FILENO, .events = POLLIN },
            { .fd = st.event_fd, .events = POLLIN },
        };
        poll(pfd, 2, 500);

        if (pfd[1].revents)
            poll_events(&st);
        if (pfd[0].revents & POLLIN) {
            unsigned char buf[4096];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0)
                handle_input(&st, buf, (int)n);
        }

        /* Track terminal resize */
        int nr, nc;
        get_terminal_size(&nr, &nc);
        if (nr != st.rows || nc != st.cols) {
            st.rows = nr;
            st.cols = nc;
            st.dirty = 1;
        }
    }

    /* Detach cleanly */
    if (st.event_fd >= 0) {
        sock_send(st.event_fd, SOCK_CMD_DETACH, NULL, 0);
        close(st.event_fd);
    }

    term_restore();
    term_write(CSI "2J" CSI "H", 7);
    printf("Bye.\n");
    return 0;
}
