#include "util/tt.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

bool tt_rand_hex(char *out, size_t n) {
    static const char hex[] = "0123456789abcdef";
    out[0] = '\0';
    if (!n) return true;
    int fd;
    do { fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC); } while (fd < 0 && errno == EINTR);
    if (fd < 0) return false;

    size_t written = 0;
    while (written < n) {
        unsigned char raw[64];
        size_t remaining = n - written;
        size_t need = remaining / 2 + remaining % 2;
        if (need > sizeof raw) need = sizeof raw;
        size_t got = 0;
        while (got < need && got < sizeof raw) {
            size_t want = need - got;
            if (want > sizeof raw - got) want = sizeof raw - got;
            ssize_t count = read(fd, raw + got, want);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                int saved = count == 0 ? EIO : errno;
                close(fd);
                out[0] = '\0';
                errno = saved;
                return false;
            }
            got += (size_t)count;
        }
        for (size_t i = 0; i < need; i++) {
            out[written++] = hex[raw[i] & 0xf];
            if (written < n) out[written++] = hex[raw[i] >> 4];
        }
    }
    close(fd);
    out[n] = '\0';
    return true;
}
