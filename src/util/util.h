/* util.h — growable buffer, paths, files, small codecs. No agent knowledge. */
#ifndef TNY_UTIL_H
#define TNY_UTIL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

/* ---- growable byte buffer (always NUL-terminated) ---- */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool oom; /* sticky: append operations become no-ops after exhaustion */
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
void buf_reserve(buf_t *b, size_t extra);
void buf_append(buf_t *b, const void *data, size_t n);
void buf_appends(buf_t *b, const char *s);
void buf_appendf(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_clear(buf_t *b);
/* Remove the first n bytes (cheap front-consume for stream parsers). */
void buf_consume(buf_t *b, size_t n);
char *buf_detach(buf_t *b); /* caller frees */
bool buf_oom(const buf_t *b);

/* ---- strings ---- */
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
bool str_starts(const char *s, const char *prefix);
bool str_ends(const char *s, const char *suffix);
char *str_trim(char *s); /* in place, returns s */
/* Length of the leading run of ASCII whitespace (space, \t \n \v \f \r).
 * Renderers skip it at the start of a streamed reply so a model that opens
 * with blank lines does not paint them; the wire keeps the raw delta. */
size_t str_ws_prefix(const char *s, size_t n);
/* Strict UTF-8 scalar validation. Embedded NUL is rejected so a validated
 * byte sequence is safe to copy into a C string. */
bool utf8_valid_bytes(const void *data, size_t len);
void secure_zero(void *p, size_t n);
void secure_free(char *s); /* wipe a NUL-terminated secret, then free */
/* Glob-style match: '*' any run, '?' one char. Not regex. */
bool glob_match(const char *pattern, const char *s);

/* ---- paths and files ---- */
char *path_join(const char *a, const char *b);        /* malloc'd */
char *path_home(void);                                /* $HOME, malloc'd */
char *path_tny_dir(void);                             /* ~/.tny, malloc'd (not created) */
char *path_abs(const char *p);                        /* absolute, resolved; malloc'd or NULL */
bool path_is_within(const char *root, const char *p); /* p inside root (both absolute) */
int mkdir_p(const char *path);
char *file_slurp(const char *path, size_t *len_out); /* NULL on error */
int file_write_atomic(const char *path, const void *data, size_t len);
bool file_exists(const char *path);
bool dir_exists(const char *path);

/* ---- codecs / ids ---- */
void b64_encode(const uint8_t *in, size_t n, buf_t *out);
size_t b64_decode(const char *in, uint8_t *out, size_t outcap); /* returns bytes or 0 */
/* base64url without padding (RFC 4648 §5): PKCE, OAuth state, JWTs. */
void b64url_encode(const uint8_t *in, size_t n, buf_t *out);
bool sha1(const uint8_t *in, size_t n, uint8_t out[20]);
bool sha256(const uint8_t *in, size_t n, uint8_t out[32]); /* PKCE S256 */
/* CSPRNG bytes from /dev/urandom; false when unavailable (callers must
 * refuse to mint secrets on false, never fall back to a clock). */
bool random_bytes(uint8_t *out, size_t n);
/* Append `key=value` (& separated) with application/x-www-form-urlencoded
 * percent-escaping of the value. */
void url_form_append(buf_t *b, const char *key, const char *val);
uint64_t fnv1a(const void *data, size_t n);
/* 16 lowercase hex chars from CSPRNG + time; caller frees */
char *gen_id(void);
/* milliseconds since epoch */
int64_t now_ms(void);
int64_t monotonic_ms(void);
bool tny_debug(void); /* TNY_DEBUG=1: pass host/protocol diagnostics through */
/* ISO-8601 UTC "YYYY-MM-DDTHH:MM:SSZ"; caller frees */
char *now_iso8601(void);
/* Whole-second UTC ISO-8601 <-> epoch seconds. The parser ignores fractional
 * seconds and any zone suffix (callers only pass UTC stamps); -1 on error. */
void iso8601_from_epoch(int64_t t, char out[32]);
int64_t iso8601_to_epoch(const char *s);

#endif
