/* Ephemeral, read-only prompt rewriting through the shared native loop. */
#ifndef TNY_OPTIMISE_H
#define TNY_OPTIMISE_H

#include "core/config.h"
#include "core/backend.h"

#define TNY_OPTIMISE_MODEL    "inception/mercury-2.5"
#define TNY_OPTIMISE_TEXT_MAX (64u * 1024u)

typedef struct tny_optimise tny_optimise;
typedef struct {
    const char *provider;
    const char *model;
    const char *text;
    const char *timeout_seconds; /* Optional decimal seconds, 1..86400. */
    const char *base_url;        /* Explicit SDK overrides after profile resolution. */
    const char *api_key;
    const char *wire_api;
} tny_optimise_request;

tny_optimise *tny_optimise_start(const tny_ctx *, const tny_optimise_request *, char *, size_t);
int tny_optimise_pollfds(tny_optimise *, struct pollfd *, int);
void tny_optimise_step(tny_optimise *);
void tny_optimise_cancel(tny_optimise *);
/* -1 pending; otherwise 0 success, 1 failed, 130 cancelled. Borrowed strings. */
int tny_optimise_result(const tny_optimise *, const char **text, const char **error);
const char *tny_optimise_model(const tny_optimise *);
const char *tny_optimise_provider(const tny_optimise *);
void tny_optimise_free(tny_optimise *);

#endif
