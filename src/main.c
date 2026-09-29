/*
 * main.c — shclaw CLI dispatch
 */

#include "../include/tc.h"
#ifndef TC_NO_PLUGINS
#include <libtcc.h>
#endif
#ifdef __COSMOPOLITAN__
#include <cosmo.h>
#endif

static void usage(void) {
    fprintf(stderr,
        "shclaw v%s — self-contained multi-agent AI daemon\n\n"
        "Usage:\n"
        "  shclaw [options]           Start daemon (foreground)\n"
        "  shclaw [options] -d        Start daemon (background)\n"
        "  shclaw [options] status    Show agent status\n"
        "  shclaw [options] msg <agent> <text>  Send message\n"
        "  shclaw [options] stop      Graceful shutdown\n"
        "  shclaw [options] irc-info  Show IRC channel + key\n"
        "  shclaw [options] compile <file.c>  Compile a plugin\n"
        "  shclaw [options] tui [agent]       Terminal chat UI\n"
        "\nOptions:\n"
        "  --workdir=DIR  Set working directory (instance root)\n"
        "                 Auto-detected if $PWD/etc/config.ini exists\n"
        "\n", TC_VERSION);
}

/* One request, optional one reply, for the short CLI commands */
static int request(uint32_t cmd, const char *data, uint32_t len, int want_reply) {
    int fd = sock_client_connect();
    if (fd < 0) { fprintf(stderr, "Cannot connect to daemon\n"); return 1; }

    if (sock_send(fd, cmd, data, len) != 0) {
        fprintf(stderr, "Request failed\n");
        close(fd);
        return 1;
    }
    if (want_reply) {
        uint32_t type;
        char reply[TC_BUF_LG];
        if (sock_read_cmd(fd, &type, reply, sizeof(reply)) >= 0)
            printf("%s\n", reply);
    }
    close(fd);
    return 0;
}

static int cmd_irc_info(void) {
    int fd = sock_client_connect();
    if (fd >= 0) {
        close(fd);
        return request(SOCK_CMD_IRC_INFO, NULL, 0, 1);
    }
    /* Daemon down: read the file it left behind */
    char dir[4096], path[4200];
    cli_data_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/irc.secret", dir);
    char *data = file_slurp(path, NULL);
    if (data) { printf("%s", data); free(data); return 0; }
    fprintf(stderr, "Cannot connect to daemon and no %s found\n", path);
    return 1;
}

static int cmd_msg(const char *agent, const char *text) {
    /* Format: agent_name\0text */
    char data[TC_BUF_LG];
    size_t agent_len = strlen(agent);
    size_t text_len = strlen(text);
    size_t total = agent_len + 1 + text_len;
    if (total >= sizeof(data)) {
        fprintf(stderr, "Message too long\n");
        return 1;
    }
    memcpy(data, agent, agent_len + 1);
    memcpy(data + agent_len + 1, text, text_len);
    if (request(SOCK_CMD_MSG, data, (uint32_t)total, 0) != 0)
        return 1;
    printf("Message sent to %s.\n", agent);
    return 0;
}

#ifndef TC_NO_PLUGINS
static int cmd_compile(const char *src) {
#ifdef __COSMOPOLITAN__
    if (IsWindows()) {
        fprintf(stderr, "Plugins not supported on Windows.\n");
        return 1;
    }
#endif
    static plugin_registry_t tmp;
    pthread_mutex_init(&tmp.lock, NULL);

    struct stat st;
    time_t mtime = (stat(src, &st) == 0) ? st.st_mtime : time(NULL);
    char err[TC_BUF_LG];

    if (plugin_compile(&tmp, src, NULL, mtime, err, sizeof(err)) == 0) {
        printf("Compiled and loaded: %s (plugin: %s)\n", src, err);
        tcc_delete(tmp.plugins[0].tcc_state);
        return 0;
    }
    fprintf(stderr, "Compilation failed:\n%s\n", err);
    return 1;
}
#endif /* !TC_NO_PLUGINS */

static volatile sig_atomic_t *g_shutdown_ptr;
static void shutdown_handler(int sig) {
    (void)sig;
    if (g_shutdown_ptr) *g_shutdown_ptr = 1;
}

static int cmd_daemon(int daemonize) {
    if (daemonize) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid > 0) {
            printf("Daemon started (pid=%d)\n", pid);
            return 0;
        }
        setsid();
        /* Redirect stdio */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
    }

    ini_t *cfg = ini_load("etc/config.ini");
    if (!cfg) {
        fprintf(stderr, "Cannot load etc/config.ini\n");
        return 1;
    }

    /* ~1MB of state: keep it off the stack */
    static daemon_t d;

    signal(SIGPIPE, SIG_IGN);

    /* Graceful shutdown on SIGTERM/SIGINT */
    g_shutdown_ptr = &d.shutdown;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shutdown_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    int rc = daemon_run(&d, cfg);
    ini_free(cfg);
    return rc;
}

int main(int argc, char **argv) {
    int workdir_set = 0;

    /* Parse --workdir override first */
    int argi = 1;
    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        if (strncmp(argv[argi], "--workdir=", 10) == 0) {
            const char *dir = argv[argi] + 10;
            if (chdir(dir) != 0) {
                fprintf(stderr, "Cannot chdir to '%s': %s\n",
                        dir, strerror(errno));
                return 1;
            }
            workdir_set = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[argi]);
            usage();
            return 1;
        }
        argi++;
    }

    if (!workdir_set) {
#ifdef INSTALL_PREFIX
        /* Baked prefix from make install */
        if (chdir(INSTALL_PREFIX) != 0) {
            fprintf(stderr, "Cannot chdir to '%s': %s\n",
                    INSTALL_PREFIX, strerror(errno));
            return 1;
        }
#else
        /* Auto-detect: check if $PWD/etc/config.ini exists */
        if (!file_exists("etc/config.ini")) {
            fprintf(stderr,
                "No etc/config.ini found in current directory.\n\n"
                "Either run shclaw from an instance directory containing\n"
                "etc/config.ini, or specify one with:\n\n"
                "  shclaw --workdir=/path/to/instance\n\n");
            return 1;
        }
#endif
    }

    if (argi >= argc)
        return cmd_daemon(0);

    const char *cmd = argv[argi];

    if (strcmp(cmd, "-d") == 0)
        return cmd_daemon(1);

    if (strcmp(cmd, "status") == 0)
        return request(SOCK_CMD_STATUS, NULL, 0, 1);

    if (strcmp(cmd, "stop") == 0) {
        if (request(SOCK_CMD_STOP, NULL, 0, 0) != 0) return 1;
        printf("Stop signal sent.\n");
        return 0;
    }

    if (strcmp(cmd, "irc-info") == 0)
        return cmd_irc_info();

    if (strcmp(cmd, "msg") == 0) {
        if (argi + 2 >= argc) { fprintf(stderr, "Usage: shclaw msg <agent> <text>\n"); return 1; }
        return cmd_msg(argv[argi + 1], argv[argi + 2]);
    }

    if (strcmp(cmd, "compile") == 0) {
#ifndef TC_NO_PLUGINS
        if (argi + 1 >= argc) { fprintf(stderr, "Usage: shclaw compile <file.c>\n"); return 1; }
        return cmd_compile(argv[argi + 1]);
#else
        fprintf(stderr, "Plugins disabled in this build.\n");
        return 1;
#endif
    }

    if (strcmp(cmd, "tui") == 0)
        return tui_run(argi + 1 < argc ? argv[argi + 1] : NULL);

    usage();
    return 1;
}
