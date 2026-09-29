/*
 * tool_exec.c — child processes: shell commands with pipe capture,
 * timeout and process group kill
 */

#include "../include/tc.h"

int child_collect(pid_t pid, int fd, int timeout_s, char *out, size_t cap,
                  size_t *len_out, size_t *dropped_out) {
    int64_t deadline = now_ms() + (int64_t)timeout_s * 1000;
    size_t len = 0, dropped = 0;
    int timed_out = 0;

    /* Read to EOF: past `cap` the output is drained and counted, so a
     * chatty child never blocks on a full pipe. */
    for (;;) {
        int64_t left = deadline - now_ms();
        if (left <= 0) { timed_out = 1; break; }
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int r = poll(&pfd, 1, left > 1000 ? 1000 : (int)left);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        char sink[4096];
        char *dst = len < cap ? out + len : sink;
        size_t room = len < cap ? cap - len : sizeof(sink);
        ssize_t n = read(fd, dst, room);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (dst == sink) dropped += (size_t)n;
        else len += (size_t)n;
    }
    close(fd);
    out[len] = '\0';

    /* The child can outlive its output (closed stdout, background job) */
    int status = 0;
    while (!timed_out) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid || (w < 0 && errno != EINTR)) break;
        if (now_ms() >= deadline) timed_out = 1;
        else usleep(10000);
    }
    if (timed_out) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }

    if (len_out) *len_out = len;
    if (dropped_out) *dropped_out = dropped;
    return timed_out ? -1 : status;
}

const char *tool_exec_cmd(const char *cmd, int timeout,
                          char *out, size_t out_sz) {
    if (!out || out_sz < 256)
        return "";

    out[0] = '\0';

    if (!cmd || !cmd[0]) {
        snprintf(out, out_sz, "Error: empty command");
        return out;
    }
    if (timeout <= 0) timeout = TC_TOOL_TIMEOUT;
    if (timeout > TC_TOOL_TIMEOUT_MAX) timeout = TC_TOOL_TIMEOUT_MAX;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(out, out_sz, "Error: pipe() failed");
        return out;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        snprintf(out, out_sz, "Error: fork() failed");
        return out;
    }

    if (pid == 0) {
        /* Child: new process group, stdin from /dev/null, and none of the
         * daemon's descriptors (IRC link, control socket, logs). */
        setpgid(0, 0);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        long max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0 || max_fd > 4096) max_fd = 4096;
        for (int fd = 3; fd < max_fd; fd++)
            close(fd);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* Parent */
    setpgid(pid, pid);
    close(pipefd[1]);

    /* Keep room for the status notes appended below */
    size_t cap = out_sz - 128;
    if (cap > TC_TOOL_OUTPUT_MAX) cap = TC_TOOL_OUTPUT_MAX;
    size_t len, dropped;
    int status = child_collect(pid, pipefd[0], timeout, out, cap, &len, &dropped);

    if (dropped)
        buf_appendf(out, out_sz, &len, "\n[output truncated: %zu more bytes]", dropped);
    if (status == -1)
        buf_appendf(out, out_sz, &len, "\n[TIMEOUT after %ds]", timeout);
    else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        buf_appendf(out, out_sz, &len, "\n[exit %d]", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        buf_appendf(out, out_sz, &len, "\n[killed by signal %d]", WTERMSIG(status));
    if (len == 0)
        buf_appendf(out, out_sz, &len, "%s", TC_EMPTY_OUTPUT_MARKER);
    return out;
}
