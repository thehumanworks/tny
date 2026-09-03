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
void buf_free(buf_t *b) {
    free(b->data);
    buf_init(b);
}

void buf_reserve(buf_t *b, size_t extra) {
    if (b->oom) return;
    if (extra > SIZE_MAX - b->len - 1) {
        b->oom = true;
        return;
    }
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    char *next = realloc(b->data, cap);
    if (!next) {
        b->oom = true;
        return;
    }
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
    if (!s) {
        b->oom = true;
        return;
    }
    buf_append(b, s, strlen(s));
}

void buf_appendf(buf_t *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    buf_reserve(b, (size_t)n);
    if (b->oom) {
        va_end(ap2);
        return;
    }
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

void buf_clear(buf_t *b) {
    b->len = 0;
    if (b->data) b->data[0] = 0;
}

void buf_consume(buf_t *b, size_t n) {
    if (n >= b->len) {
        buf_clear(b);
        return;
    }
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

size_t str_ws_prefix(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\v' && c != '\f' && c != '\r') break;
        i++;
    }
    return i;
}

bool utf8_valid_bytes(const void *data, size_t len) {
    if (!data && len) return false;
    const unsigned char *s = data;
    for (size_t i = 0; i < len;) {
        unsigned c = s[i++];
        if (c == 0) return false;
        if (c < 0x80) continue;
        unsigned need, min;
        if ((c & 0xe0) == 0xc0) {
            need = 1;
            min = 0x80;
            c &= 0x1f;
        } else if ((c & 0xf0) == 0xe0) {
            need = 2;
            min = 0x800;
            c &= 0x0f;
        } else if ((c & 0xf8) == 0xf0) {
            need = 3;
            min = 0x10000;
            c &= 0x07;
        } else return false;
        if (len - i < need) return false;
        for (unsigned j = 0; j < need; j++) {
            unsigned d = s[i++];
            if ((d & 0xc0) != 0x80) return false;
            c = (c << 6) | (d & 0x3f);
        }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return true;
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
        if (*p == '*') {
            star = p++;
            ss = s;
        } else if (*p == '?' || *p == *s) {
            p++;
            s++;
        } else if (star) {
            p = star + 1;
            s = ++ss;
        } else return false;
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
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        buf_append(&b, chunk, n);
        if (n < sizeof chunk) break; /* short read: EOF or error; ferror below */
    }
    bool err = ferror(f);
    fclose(f);
    if (err) {
        buf_free(&b);
        return NULL;
    }
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
        if (w < 0) {
            close(fd);
            unlink(tmp);
            return -1;
        }
        off += (size_t)w;
    }
    close(fd);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
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

void b64url_encode(const uint8_t *in, size_t n, buf_t *out) {
    buf_t std;
    buf_init(&std);
    b64_encode(in, n, &std);
    for (size_t i = 0; i < std.len; i++) {
        char c = std.data[i];
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
        else if (c == '=') break;
        buf_append(out, &c, 1);
    }
    buf_free(&std);
}

void url_form_append(buf_t *b, const char *key, const char *val) {
    if (b->len) buf_appends(b, "&");
    buf_appends(b, key);
    buf_appends(b, "=");
    for (const unsigned char *p = (const unsigned char *)val; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~')
            buf_appendf(b, "%c", c);
        else buf_appendf(b, "%%%02X", c);
    }
}

/* Minimal SHA-256 (FIPS 180-4), needed for the PKCE S256 challenge. */
static uint32_t ror(uint32_t x, int c) { return (x >> c) | (x << (32 - c)); }

bool sha256(const uint8_t *in, size_t n, uint8_t out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t ml = (uint64_t)n * 8;
    size_t total = ((n + 8) / 64 + 1) * 64;
    uint8_t *msg = calloc(1, total);
    if (!msg) return false;
    memcpy(msg, in, n);
    msg[n] = 0x80;
    for (int i = 0; i < 8; i++) msg[total - 1 - i] = (uint8_t)(ml >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)msg[off + 4 * i] << 24) | ((uint32_t)msg[off + 4 * i + 1] << 16) |
                   ((uint32_t)msg[off + 4 * i + 2] << 8) | msg[off + 4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    free(msg);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
    return true;
}

bool random_bytes(uint8_t *out, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, out + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    return got == n;
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
            w[i] = (uint32_t)msg[off + 4 * (size_t)i] << 24 |
                   (uint32_t)msg[off + 4 * (size_t)i + 1] << 16 |
                   (uint32_t)msg[off + 4 * (size_t)i + 2] << 8 | msg[off + 4 * (size_t)i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
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

void iso8601_from_epoch(int64_t t, char out[32]) {
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

int64_t iso8601_to_epoch(const char *s) {
    int y, mo, d, h, mi, sec;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return -1;
    /* days-from-civil (public-domain calendar algorithm) */
    int64_t yy = y - (mo < 2);
    int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    int64_t yoe = yy - era * 400;
    int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + sec;
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
