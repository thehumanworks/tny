/* Disposable shared-library syscall fault fixture. The actual jobs_host.c is
 * compiled below, unchanged. Wrappers execute real syscalls except at the
 * selected failure. This verifies syscall ordering, NOT physical power loss. */
#include "util/jobs_host.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char fail_at;
static char trace[128];
static size_t trace_len;
static int file_fd = -1, dir_fd = -1;

void tny_mailbox_fault_reset(char stage);
const char *tny_mailbox_fault_trace(void);

/* Events: O=temp open, W=write, F=file fsync, C=file close, R=rename,
 * L=link, U=temp unlink, D=parent open, S=parent fsync, X=parent close. */
void tny_mailbox_fault_reset(char stage) {
    fail_at = stage;
    trace_len = 0;
    trace[0] = 0;
    file_fd = dir_fd = -1;
}
const char *tny_mailbox_fault_trace(void) { return trace; }

static bool event(char stage) {
    if (trace_len < sizeof(trace) - 1) {
        trace[trace_len++] = stage;
        trace[trace_len] = 0;
    }
    if (stage != fail_at) return false;
    errno = EIO;
    return true;
}
static int fault_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    bool directory = (flags & O_DIRECTORY) != 0;
    bool staging = (flags & O_EXCL) != 0;
    if ((directory || staging) && event(directory ? 'D' : 'O')) return -1;
    int fd = open(path, flags, mode);
    if (directory) dir_fd = fd;
    if (staging) file_fd = fd;
    return fd;
}
static ssize_t fault_write(int fd, const void *data, size_t len) {
    if (fd == file_fd && event('W')) return -1;
    return write(fd, data, len);
}
static int fault_fsync(int fd) {
    if (event(fd == dir_fd ? 'S' : 'F')) return -1;
    return fsync(fd);
}
static int fault_close(int fd) {
    char stage = fd == dir_fd ? 'X' : fd == file_fd ? 'C' : 0;
    /* Close for real even for injected failure; do not leak test descriptors. */
    int rc = close(fd);
    if (fd == file_fd) file_fd = -1;
    if (fd == dir_fd) dir_fd = -1;
    return stage && event(stage) ? -1 : rc;
}
static int fault_rename(const char *from, const char *to) {
    if (event('R')) return -1;
    return rename(from, to);
}
static int fault_link(const char *from, const char *to) {
    if (event('L')) return -1;
    return link(from, to);
}
static int fault_unlink(const char *path) {
    if (event('U')) return -1;
    return unlink(path);
}

#define open   fault_open
#define write  fault_write
#define fsync  fault_fsync
#define close  fault_close
#define rename fault_rename
#define link   fault_link
#define unlink fault_unlink
// NOLINTNEXTLINE(bugprone-suspicious-include): instrument the real host implementation.
#include "../../src/util/jobs_host.c"
#undef open
#undef write
#undef fsync
#undef close
#undef rename
#undef link
#undef unlink
