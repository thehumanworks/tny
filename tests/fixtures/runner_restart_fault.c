/* Local deterministic failure injection for the private restart protocol.
 * Loaded only by test-owned binaries; no product fault hook or secrets. */
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ssize_t fault_write(int fd, const void *buffer, size_t size) {
    const char *mode = getenv("TNY_FIXTURE_RESTART_FAULT");
    if (fd == 3 && size == 1 && *(const char *)buffer == 'C' && mode &&
        strcmp(mode, "post-go") == 0)
        _exit(94);
#ifdef __APPLE__
    return write(fd, buffer, size);
#else
    ssize_t (*original)(int, const void *, size_t) = dlsym(RTLD_NEXT, "write");
    return original ? original(fd, buffer, size) : -1;
#endif
}

static ssize_t fault_read(int fd, void *buffer, size_t size) {
#ifdef __APPLE__
    ssize_t n = read(fd, buffer, size);
#else
    ssize_t (*original)(int, void *, size_t) = dlsym(RTLD_NEXT, "read");
    ssize_t n = original ? original(fd, buffer, size) : -1;
#endif
    const char *mode = getenv("TNY_FIXTURE_RESTART_FAULT");
    if (fd == 3 && size == 1 && n == 1 && *(const char *)buffer == 'X' && mode &&
        strcmp(mode, "post-run") == 0)
        _exit(95);
    return n;
}
#ifdef __APPLE__
__attribute__((used, section("__DATA,__interpose"))) static const struct {
    ssize_t (*replacement)(int, const void *, size_t);
    ssize_t (*original)(int, const void *, size_t);
} write_interpose = {fault_write, write};
__attribute__((used, section("__DATA,__interpose"))) static const struct {
    ssize_t (*replacement)(int, void *, size_t);
    ssize_t (*original)(int, void *, size_t);
} read_interpose = {fault_read, read};
#else
ssize_t write(int fd, const void *buffer, size_t size) { return fault_write(fd, buffer, size); }
ssize_t read(int fd, void *buffer, size_t size) { return fault_read(fd, buffer, size); }
#endif
