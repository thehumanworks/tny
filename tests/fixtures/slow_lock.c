/* Darwin test-only latency injection for the session-stop deadline. */
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int slow_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    const char *name = strrchr(path, '/');
    if (name && strcmp(name, "/lock") == 0 && !(flags & O_CREAT)) {
        struct timespec delay = {0, 80000000L};
        nanosleep(&delay, NULL);
    }
    return open(path, flags, mode);
}

__attribute__((used, section("__DATA,__interpose"))) static struct {
    void *replacement;
    void *original;
} hook = {(void *)slow_open, (void *)open};
