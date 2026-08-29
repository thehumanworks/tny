/* tt.h — tnytty's small utility kit: growable buffer, base64, OS random
 * hex ids, constant-time compare. */
#ifndef TNYTTY_TT_H
#define TNYTTY_TT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *data;
    size_t len, cap;
    bool oom;
} tt_buf;

void tt_buf_init(tt_buf *b);
void tt_buf_free(tt_buf *b);
bool tt_buf_append(tt_buf *b, const void *bytes, size_t len);
bool tt_buf_appendf(tt_buf *b, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Encoded length is 4 * ceil(len / 3); dst gets no NUL. Returns bytes
 * written. */
size_t tt_base64_encode(char *dst, const unsigned char *src, size_t len);

/* Fill out with n random lowercase hex chars + NUL (OS RNG; aborts to
 * a degraded time/pid mix only if the OS source is unreadable). */
void tt_rand_hex(char *out, size_t n);

/* Constant-time equality for secrets. */
bool tt_const_eq(const char *a, const char *b);

#endif
