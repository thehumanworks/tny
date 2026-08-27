/* tny_poll.c — native tny_poll: poll(2), nothing else. The wasm build
 * excludes this file and gets tny_poll from src/net/net_wasm.c instead. */
#include "util/tny_poll.h"
#include "util/tny_wake.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int tny_poll(struct pollfd *fds, nfds_t n, int timeout_ms) {
    return poll(fds, n, timeout_ms);
}

static int fd_flags(int fd, int get, int set, int add) {
    int flags = fcntl(fd, get, 0);
    return flags < 0 || fcntl(fd, set, flags | add) < 0 ? -1 : 0;
}

int tny_wake_init(tny_wake *wake) {
    if (!wake) return -1;
    wake->read_fd = -1;
    wake->write_fd = -1;
    int fds[2];
    if (pipe(fds) != 0) return -1;
    if (fd_flags(fds[0], F_GETFL, F_SETFL, O_NONBLOCK) != 0 ||
        fd_flags(fds[1], F_GETFL, F_SETFL, O_NONBLOCK) != 0 ||
        fd_flags(fds[0], F_GETFD, F_SETFD, FD_CLOEXEC) != 0 ||
        fd_flags(fds[1], F_GETFD, F_SETFD, FD_CLOEXEC) != 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    wake->read_fd = fds[0];
    wake->write_fd = fds[1];
    return 0;
}

void tny_wake_close(tny_wake *wake) {
    if (!wake) return;
    if (wake->read_fd >= 0) close(wake->read_fd);
    if (wake->write_fd >= 0) close(wake->write_fd);
    wake->read_fd = -1;
    wake->write_fd = -1;
}

int tny_wake_fd(const tny_wake *wake) {
    return wake ? wake->read_fd : -1;
}

void tny_wake_signal(tny_wake *wake) {
    if (!wake || wake->write_fd < 0) return;
    unsigned char byte = 1;
    ssize_t n;
    do n = write(wake->write_fd, &byte, sizeof byte); while (n < 0 && errno == EINTR);
    /* EAGAIN means an earlier byte already guarantees a wake. */
}

void tny_wake_drain(tny_wake *wake) {
    if (!wake || wake->read_fd < 0) return;
    unsigned char bytes[64];
    for (;;) {
        ssize_t n = read(wake->read_fd, bytes, sizeof bytes);
        if (n > 0) continue;
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}
