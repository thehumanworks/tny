/* Private STT adapter interface. Providers own auth, wire format and transport;
 * the service owns recording, bounds, cancellation and validated prompt text. */
#ifndef TNY_DICTATION_PROVIDER_H
#define TNY_DICTATION_PROVIDER_H
#include "core/dictation.h"

typedef struct {
    const char *name;
    bool (*available)(const tny_ctx *, char *, size_t);
    void *(*start)(const tny_ctx *, const buf_t *wav, char *, size_t);
    int (*fd)(const void *);
    /* -1 pending, 0 complete, 1 local/protocol error, 2 HTTP rejection. */
    int (*step)(void *, buf_t *text, char *, size_t);
    void (*destroy)(void *);
} tny_dictation_provider;

extern const tny_dictation_provider tny_dictation_codex;
extern const tny_dictation_provider tny_dictation_xai;

/* Private shared transport; caller validates credentials and owns their lifetime. */
void *tny_dictation_http_start(const char *url, const char *token, const char *account_id,
                               const buf_t *wav, char *err, size_t len);
int tny_dictation_http_fd(const void *);
int tny_dictation_http_step(void *, buf_t *, char *, size_t);
void tny_dictation_http_destroy(void *);
#endif
