/* Fault-injection harness for the actual extracted output_pipe definition.
 * The Python seam test replaces ACTUAL_OUTPUT_PIPE, never a handwritten copy. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>

static int failed_stage, stage, closes;

static int probe_pipe(int fds[2]) {
    if (failed_stage == 5) {
        errno = EMFILE;
        return -1;
    }
    return pipe(fds);
}

static int probe_fcntl(int fd, int command, ...) {
    if (++stage == failed_stage) {
        errno = EACCES;
        return -1;
    }
    if (command == F_GETFL) return fcntl(fd, command);
    va_list args;
    va_start(args, command);
    int argument = va_arg(args, int);
    va_end(args);
    return fcntl(fd, command, argument);
}

static int probe_close(int fd) {
    ++closes;
    int rc = close(fd);
    /* Cleanup must preserve the original setup error, not this errno. */
    errno = EINTR;
    return rc;
}

#define pipe  probe_pipe
#define fcntl probe_fcntl
#define close probe_close
/* ACTUAL_OUTPUT_PIPE */
#undef pipe
#undef fcntl
#undef close

int main(void) {
    for (failed_stage = 0; failed_stage <= 5; ++failed_stage) {
        int fds[2] = {-1, -1};
        stage = closes = 0;
        errno = 0;
        int rc = output_pipe(fds);
        if (!failed_stage) {
            if (rc || fds[0] < 0 || fds[1] < 0 || closes) return 1;
            if (!(fcntl(fds[0], F_GETFD) & FD_CLOEXEC) || !(fcntl(fds[1], F_GETFD) & FD_CLOEXEC) ||
                !(fcntl(fds[0], F_GETFL) & O_NONBLOCK))
                return 2;
            close(fds[0]);
            close(fds[1]);
        } else {
            if (rc != -1) return 10 + failed_stage;
            if (errno != (failed_stage == 5 ? EMFILE : EACCES)) return 20 + failed_stage;
            if (fds[0] != -1 || fds[1] != -1) return 30 + failed_stage;
            if (closes != (failed_stage == 5 ? 0 : 2)) return 40 + failed_stage;
        }
    }
    return 0;
}
