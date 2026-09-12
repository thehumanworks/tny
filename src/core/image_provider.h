/* Internal adapter boundary. Providers perform no artifact I/O and return image
 * bytes; shared code owns validation, cancellation and atomic persistence. */
#ifndef TNY_IMAGE_PROVIDER_H
#define TNY_IMAGE_PROVIDER_H
#include "core/image_service.h"

typedef struct {
    buf_t data;
    const char *mime;
} tny_image_input;

/* What the adapter actually put on the wire, and the few scalar identifiers
 * the provider actually returned. Shared code reports these literally: it
 * never infers provider internals, never substitutes a local operation id or
 * a requested seed, and never copies an unrecognized provider object. */
typedef struct {
    const char *size; /* exact size literal sent; NULL when none was sent */
    bool have_seed;   /* false means the response carried no seed at all */
    int64_t seed;
    char request_id[TNY_IMAGE_REQUEST_ID_MAX]; /* empty when none was returned */
} tny_image_wire;

typedef struct {
    const char *name;
    const char *default_model;
    size_t max_references; /* zero means generation only */
    bool (*available)(const tny_ctx *, char *, size_t);
    int (*render)(const tny_ctx *, const tny_image_request *, const tny_image_input *, buf_t *,
                  tny_image_wire *, char *, size_t);
} tny_image_provider;

extern const tny_image_provider tny_image_codex;
bool tny_image_stopped(const tny_image_request *);
/* Strict canonical base64 decoder with a bounded, magic-checked result. */
int tny_image_decode(const char *, size_t, buf_t *, char *, size_t);
#endif
