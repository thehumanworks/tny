/* The disposable fault binary compiles the production random helper with
 * syscall failures here. The shipped binary contains no fault switches. */
#include "util/tt.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool open_interrupted, read_interrupted;
static size_t bytes_read;
static unsigned random_opens;

static bool random_fault(const char *name) {
    const char *mode = getenv("TNYTTY_FIXTURE_RANDOM");
    return mode && strcmp(mode, name) == 0;
}

static bool wait_for_child(void) {
    const char *path = getenv("TNYTTY_FIXTURE_CHILD_PID_FILE");
    if (!path) return false;
    for (int i = 0; i < 500; i++) {
        FILE *f = fopen(path, "r");
        if (f) {
            long pid = 0;
            bool ready = fscanf(f, "%ld", &pid) == 1 && pid > 0;
            fclose(f);
            if (ready) return true;
        }
        poll(NULL, 0, 10);
    }
    return false;
}

static int random_open(const char *path, int flags) {
    bytes_read = 0;
    random_opens++;
    if (random_opens > 1 && (random_fault("after-session") || random_fault("bind"))) {
        if (!wait_for_child()) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (random_fault("after-session")) {
            errno = EIO;
            return -1;
        }
    }
    if (random_fault("open")) {
        errno = EACCES;
        return -1;
    }
    if (random_fault("interrupted") && !open_interrupted) {
        open_interrupted = true;
        errno = EINTR;
        return -1;
    }
    return open(path, flags);
}

static ssize_t random_read(int fd, void *buf, size_t n) {
    if (random_fault("read") || (random_fault("partial") && bytes_read) ||
        (random_fault("late") && bytes_read >= 64)) {
        errno = EIO;
        return -1;
    }
    if (random_fault("eof")) return 0;
    if (random_fault("interrupted") && !read_interrupted) {
        read_interrupted = true;
        errno = EINTR;
        return -1;
    }
    if (random_fault("partial") && n > 1) n = 1;
    if (random_fault("interrupted") && n > 3) n = 3;
    ssize_t count = read(fd, buf, n);
    if (count > 0) bytes_read += (size_t)count;
    return count;
}

#define open random_open
#define read random_read
#include "../../src/util/random.c"
#undef read
#undef open
