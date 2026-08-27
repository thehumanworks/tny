#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>

void buf_init(buf_t *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = false;
}
void buf_free(buf_t *b) { free(b->data); buf_init(b); }

void buf_reserve(buf_t *b, size_t extra) {
    if (b->oom) return;
    if (extra > SIZE_MAX - b->len - 1) { b->oom = true; return; }
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    char *next = realloc(b->data, cap);
    if (!next) { b->oom = true; return; }
    b->data = next;
    b->cap = cap;
}

void buf_append(buf_t *b, const void *data, size_t n) {
    if (b->oom) return;
    if (!n) {
        buf_reserve(b, 0);
        if (!b->oom) b->data[b->len] = 0;
        return;
    }
    buf_reserve(b, n);
    if (b->oom) return;
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = 0;
}

void buf_appends(buf_t *b, const char *s) {
    if (!s) { b->oom = true; return; }
    buf_append(b, s, strlen(s));
}

void buf_appendf(buf_t *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    buf_reserve(b, (size_t)n);
    if (b->oom) { va_end(ap2); return; }
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

void buf_clear(buf_t *b) { b->len = 0; if (b->data) b->data[0] = 0; }

void buf_consume(buf_t *b, size_t n) {
    if (n >= b->len) { buf_clear(b); return; }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
    b->data[b->len] = 0;
}

char *buf_detach(buf_t *b) {
    if (b->oom) {
        free(b->data);
        buf_init(b);
        return NULL;
    }
    if (!b->data) {
        buf_reserve(b, 0);
        if (b->oom) {
            free(b->data);
            buf_init(b);
            return NULL;
        }
        b->data[0] = 0;
    }
    char *p = b->data;
    buf_init(b);
    return p;
}

bool buf_oom(const buf_t *b) { return b && b->oom; }

char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

char *xstrndup(const char *s, size_t n) {
    if (n == SIZE_MAX) return NULL;
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

bool str_starts(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool str_ends(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    return lf <= ls && memcmp(s + ls - lf, suffix, lf) == 0;
}

char *str_trim(char *s) {
    char *end;
    while (isspace((unsigned char)*s)) memmove(s, s + 1, strlen(s));
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}

void secure_zero(void *data, size_t n) {
    volatile unsigned char *p = data;
    while (n--) *p++ = 0;
}

void secure_free(char *s) {
    if (!s) return;
    secure_zero(s, strlen(s));
    free(s);
}

bool glob_match(const char *p, const char *s) {
    /* iterative backtracking match: '*' any run, '?' one char */
    const char *star = NULL, *ss = NULL;
    while (*s) {
        if (*p == '*') { star = p++; ss = s; }
        else if (*p == '?' || *p == *s) { p++; s++; }
        else if (star) { p = star + 1; s = ++ss; }
        else return false;
    }
    while (*p == '*') p++;
    return *p == 0;
}

char *path_join(const char *a, const char *b) {
    if (!a || !b) return NULL;
    buf_t buf;
    buf_init(&buf);
    buf_appends(&buf, a);
    if (buf.len && buf.data[buf.len - 1] != '/') buf_appends(&buf, "/");
    buf_appends(&buf, b[0] == '/' ? b + 1 : b);
    return buf_detach(&buf);
}

char *path_home(void) {
    const char *h = getenv("HOME");
    return xstrdup(h && *h ? h : "/tmp");
}

char *path_tny_dir(void) {
    char *h = path_home();
    char *p = path_join(h, ".tny");
    free(h);
    return p;
}

char *path_abs(const char *p) {
    if (!p) return NULL;
    char out[PATH_MAX];
    if (realpath(p, out)) return xstrdup(out);
    if (p[0] == '/') return xstrdup(p);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return NULL;
    return path_join(cwd, p);
}

bool path_is_within(const char *root, const char *p) {
    size_t rl = strlen(root);
    while (rl > 1 && root[rl - 1] == '/') rl--;
    if (strncmp(p, root, rl) != 0) return false;
    return p[rl] == 0 || p[rl] == '/';
}

int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for (char *q = tmp + 1; *q; q++) {
        if (*q == '/') {
            *q = 0;
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
            *q = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

char *file_slurp(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    buf_t b;
    buf_init(&b);
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_append(&b, chunk, n);
    bool err = ferror(f);
    fclose(f);
    if (err) { buf_free(&b); return NULL; }
    if (len_out) *len_out = b.len;
    return buf_detach(&b);
}

int file_write_atomic(const char *path, const void *data, size_t len) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, getpid()) >= (int)sizeof tmp) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    const char *p = data;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w < 0) { close(fd); unlink(tmp); return -1; }
        off += (size_t)w;
    }
    close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void b64_encode(const uint8_t *in, size_t n, buf_t *out) {
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        char q[4];
        q[0] = B64[(v >> 18) & 63];
        q[1] = B64[(v >> 12) & 63];
        q[2] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        q[3] = i + 2 < n ? B64[v & 63] : '=';
        buf_append(out, q, 4);
    }
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

size_t b64_decode(const char *in, uint8_t *out, size_t outcap) {
    size_t o = 0;
    uint32_t v = 0;
    int bits = 0;
    for (const char *p = in; *p && *p != '='; p++) {
        int d = b64_val(*p);
        if (d < 0) continue;
        v = (v << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= outcap) return 0;
            out[o++] = (uint8_t)(v >> bits);
        }
    }
    return o;
}

/* Minimal SHA-1 (needed only for the WebSocket accept key). */
static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

bool sha1(const uint8_t *in, size_t n, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t ml = (uint64_t)n * 8;
    size_t total = ((n + 8) / 64 + 1) * 64;
    uint8_t *msg = calloc(1, total);
    if (!msg) return false;
    memcpy(msg, in, n);
    msg[n] = 0x80;
    for (int i = 0; i < 8; i++) msg[total - 1 - (size_t)i] = (uint8_t)(ml >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)msg[off + 4 * (size_t)i] << 24 | (uint32_t)msg[off + 4 * (size_t)i + 1] << 16 |
                   (uint32_t)msg[off + 4 * (size_t)i + 2] << 8 | msg[off + 4 * (size_t)i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(msg);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
    return true;
}

uint64_t fnv1a(const void *data, size_t n) {
    const uint8_t *p = data;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

char *gen_id(void) {
    uint8_t r[8];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t got = read(fd, r, sizeof r);
        close(fd);
        if (got != (ssize_t)sizeof r) fd = -1;
    }
    if (fd < 0) {
        uint64_t t = (uint64_t)now_ms() ^ ((uint64_t)getpid() << 32);
        memcpy(r, &t, sizeof r);
    }
    char *s = malloc(17);
    if (!s) return NULL;
    for (int i = 0; i < 8; i++) sprintf(s + 2 * i, "%02x", r[i]);
    return s;
}

int64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return now_ms();
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool tny_debug(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TNY_DEBUG");
        on = e && *e && strcmp(e, "0") != 0;
    }
    return on;
}

char *now_iso8601(void) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    char *s = malloc(24);
    if (!s) return NULL;
    strftime(s, 24, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return s;
}
