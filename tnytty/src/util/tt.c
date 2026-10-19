#include "util/tt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tt_buf_init(tt_buf *b) { memset(b, 0, sizeof *b); }

void tt_buf_free(tt_buf *b) {
    free(b->data);
    memset(b, 0, sizeof *b);
}

static bool buf_reserve(tt_buf *b, size_t extra) {
    if (b->oom) return false;
    if (extra == SIZE_MAX || b->len > SIZE_MAX - extra - 1) {
        b->oom = true;
        return false;
    }
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    char *nd = realloc(b->data, cap);
    if (!nd) {
        b->oom = true;
        return false;
    }
    b->data = nd;
    b->cap = cap;
    return true;
}

bool tt_buf_append(tt_buf *b, const void *bytes, size_t len) {
    if (!buf_reserve(b, len)) return false;
    if (len) memcpy(b->data + b->len, bytes, len);
    b->len += len;
    b->data[b->len] = '\0';
    return true;
}

bool tt_buf_appendf(tt_buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0 || !buf_reserve(b, (size_t)need)) {
        va_end(ap2);
        return false;
    }
    vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)need;
    return true;
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t tt_base64_encode(char *dst, const unsigned char *src, size_t len) {
    size_t o = 0;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];
        dst[o++] = b64[(v >> 18) & 63];
        dst[o++] = b64[(v >> 12) & 63];
        dst[o++] = b64[(v >> 6) & 63];
        dst[o++] = b64[v & 63];
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)src[i] << 16;
        dst[o++] = b64[(v >> 18) & 63];
        dst[o++] = b64[(v >> 12) & 63];
        dst[o++] = '=';
        dst[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8);
        dst[o++] = b64[(v >> 18) & 63];
        dst[o++] = b64[(v >> 12) & 63];
        dst[o++] = b64[(v >> 6) & 63];
        dst[o++] = '=';
    }
    return o;
}

bool tt_const_eq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la ^ lb);
    size_t n = la > lb ? la : lb;
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

int tt_parse_listen(const char *spec, char *host, size_t hostlen, int *port) {
    const char *colon = strrchr(spec, ':');
    if (!colon || colon == spec) return -1;
    size_t len = (size_t)(colon - spec);
    if (len >= hostlen) return -1;
    memcpy(host, spec, len);
    host[len] = '\0';
    char *end = NULL;
    long value = strtol(colon + 1, &end, 10);
    if (!end || *end || value < 1 || value > 65535) return -1;
    *port = (int)value;
    return 0;
}
