#include "icat/icat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* kitty caps one APC payload at 4096 base64 bytes; that keeps every
 * sequence within the VT core's APC buffer too. */
#define CHUNK_B64 4096

static const unsigned char png_magic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

int tt_icat_encode(const unsigned char *data, size_t len, tt_buf *out, const char **err) {
    if (len == 0) {
        *err = "empty input";
        return -1;
    }
    if (len < sizeof png_magic || memcmp(data, png_magic, sizeof png_magic) != 0) {
        *err = "only PNG is supported in phase 1 (docs/adr/0003); JPEG/GIF decode is phase 2";
        return -1;
    }
    size_t b64_len = (len + 2) / 3 * 4;
    char *b64 = malloc(b64_len + 1);
    if (!b64) {
        *err = "out of memory";
        return -1;
    }
    tt_base64_encode(b64, data, len);
    size_t off = 0;
    bool first = true;
    while (off < b64_len) {
        size_t n = b64_len - off;
        if (n > CHUNK_B64) n = CHUNK_B64;
        bool last = off + n >= b64_len;
        if (first) tt_buf_appendf(out, "\x1b_Ga=T,f=100,m=%d;", last ? 0 : 1);
        else tt_buf_appendf(out, "\x1b_Gm=%d;", last ? 0 : 1);
        tt_buf_append(out, b64 + off, n);
        tt_buf_append(out, "\x1b\\", 2);
        off += n;
        first = false;
    }
    free(b64);
    if (out->oom) {
        *err = "out of memory";
        return -1;
    }
    return 0;
}

static unsigned char *read_all(FILE *f, size_t *len_out) {
    size_t cap = 1 << 16, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (len == cap) {
            cap *= 2;
            unsigned char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        size_t r = fread(buf + len, 1, cap - len, f);
        len += r;
        if (r == 0) {
            if (ferror(f)) {
                free(buf);
                return NULL;
            }
            break;
        }
    }
    *len_out = len;
    return buf;
}

int tt_icat_main(const char *path) {
    FILE *f = stdin;
    if (strcmp(path, "-") != 0) {
        f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "tnytty icat: %s: %s\n", path, strerror(errno));
            return 1;
        }
    }
    size_t len = 0;
    unsigned char *data = read_all(f, &len);
    if (f != stdin) fclose(f);
    if (!data) {
        fprintf(stderr, "tnytty icat: reading %s failed\n", path);
        return 1;
    }
    tt_buf out;
    tt_buf_init(&out);
    const char *err = NULL;
    int rc = tt_icat_encode(data, len, &out, &err);
    free(data);
    if (rc != 0) {
        fprintf(stderr, "tnytty icat: %s\n", err);
        tt_buf_free(&out);
        return 1;
    }
    fwrite(out.data, 1, out.len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    tt_buf_free(&out);
    return 0;
}
