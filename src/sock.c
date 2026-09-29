/*
 * sock.c — Unix domain socket server + client
 */

#include "../include/tc.h"

static int read_full(int fd, void *buf, size_t len) {
    char *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
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

int sock_server_create(const char *path) {
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    /* The socket can start sessions (and so run commands): owner only */
    mode_t old = umask(077);
    int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old);
    if (rc < 0 || chmod(path, 0600) != 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }

    log_info("Socket: listening on %s", path);
    return fd;
}

int sock_send(int fd, uint32_t type, const char *data, uint32_t len) {
    wire_header_t hdr = { .type = type, .len = len };
    if (write_all(fd, &hdr, sizeof(hdr)) != 0) return -1;
    if (len > 0 && data && write_all(fd, data, len) != 0) return -1;
    return 0;
}

int sock_read_cmd(int fd, uint32_t *type, char *data, size_t max_len) {
    wire_header_t hdr;
    if (read_full(fd, &hdr, sizeof(hdr)) != 0) return -1;

    *type = hdr.type;
    data[0] = '\0';
    if (hdr.len == 0) return 0;

    size_t to_read = hdr.len < max_len - 1 ? hdr.len : max_len - 1;
    if (to_read > 0 && read_full(fd, data, to_read) != 0) return -1;
    data[to_read] = '\0';

    /* Discard excess */
    if (hdr.len > to_read) {
        char discard[256];
        size_t remaining = hdr.len - to_read;
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
            if (read_full(fd, discard, chunk) != 0) return -1;
            remaining -= chunk;
        }
    }

    return (int)to_read;
}

int sock_client_connect(void) {
    char dir[4096];
    cli_data_dir(dir, sizeof(dir));

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/shclaw.sock", dir);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}
