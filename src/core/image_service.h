/* Provider-independent image operations (ADR 0072). */
#ifndef TNY_IMAGE_SERVICE_H
#define TNY_IMAGE_SERVICE_H
#include "core/config.h"

#define TNY_IMAGE_PROMPT_MAX     (16u * 1024u)
#define TNY_IMAGE_INPUT_MAX      (8u * 1024u * 1024u)
#define TNY_IMAGE_OUTPUT_MAX     (32u * 1024u * 1024u)
#define TNY_IMAGE_REFERENCES_MAX 5

typedef struct {
    bool edit;
    const char *prompt;
    const char *provider; /* NULL: codex, independent of conversation provider */
    const char *model;    /* NULL: adapter default */
    const char *quality;  /* NULL: auto */
    const char *size;     /* NULL: auto */
    const char *output_file;
    const char *images[TNY_IMAGE_REFERENCES_MAX];
    size_t image_count;
    bool (*cancelled)(void *);
    void *userdata;
} tny_image_request;

typedef struct {
    const char *provider;
    const char *model;
    const char *mime; /* static; bytes are always sniffed */
    size_t bytes;
} tny_image_result;

/* No network or refresh. Presence is not a remote entitlement check. */
bool tny_image_available(const tny_ctx *, const char *provider, bool edit, char *err,
                         size_t errlen);
/* Probe all registered adapters; optionally append available names separated by commas. */
bool tny_image_capabilities(const tny_ctx *, bool edit, buf_t *names);
/* 0 success, 1 local/config/protocol error, 2 HTTP rejection, 130 cancelled.
 * Output is replaced only after a complete, validated image. */
int tny_image_run(const tny_ctx *, const tny_image_request *, tny_image_result *, char *, size_t);
/* Shared CLI/interception grammar. Prompt is supplied separately on stdin.
 * 0 parsed, 1 invalid, -1 help. Request must be zero initialized. */
int tny_image_options(int argc, char **argv, tny_image_request *, bool *json, bool *check);
void tny_image_result_json(const tny_image_request *, const tny_image_result *, buf_t *);
#endif
