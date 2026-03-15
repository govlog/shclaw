/*
 * tool_exec.c — fork+exec with pipe capture, timeout, process group kill
 */

#include "../include/tc.h"

const char *tool_exec_cmd(const char *cmd, int timeout,
                          char *out, size_t out_sz) {
    if (!cmd || !cmd[0]) {
        snprintf(out, out_sz, "Error: empty command");
        return out;
    }
    if (timeout <= 0) timeout = TC_TOOL_TIMEOUT;

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
        /* Child: new process group */
        setpgid(0, 0);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }

    /* Parent */
    close(pipefd[1]);

    size_t total = 0;
    size_t max_capture = out_sz - 128; /* Leave room for status suffix */
    if (max_capture > (size_t)TC_TOOL_OUTPUT_MAX)
        max_capture = TC_TOOL_OUTPUT_MAX;

    struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
    int64_t start = now_ms();
    int deadline_ms = timeout * 1000;
    int timed_out = 0;

    while (total < max_capture) {
        int remaining = deadline_ms - (int)(now_ms() - start);
        if (remaining <= 0) {
            timed_out = 1;
            break;
        }
        int r = poll(&pfd, 1, remaining < 100 ? remaining : 100);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            if (r == 0 && (int)(now_ms() - start) >= deadline_ms) {
                timed_out = 1;
                break;
            }
            continue;
        }
        ssize_t n = read(pipefd[0], out + total, max_capture - total);
        if (n <= 0) break;
        total += n;
    }
    out[total] = '\0';
    close(pipefd[0]);

    if (timed_out) {
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        snprintf(out + total, out_sz - total, "\n[TIMEOUT after %ds]", timeout);
        return out;
    }

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code != 0) {
            size_t len = strlen(out);
            snprintf(out + len, out_sz - len, "\n[exit %d]", code);
        }
    } else if (WIFSIGNALED(status)) {
        size_t len = strlen(out);
        snprintf(out + len, out_sz - len, "\n[killed by signal %d]", WTERMSIG(status));
    }

    return out;
}
