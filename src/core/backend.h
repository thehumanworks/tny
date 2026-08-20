/* backend.h — shared client contract (docs/backends/README.md).
 * One backend instance per process. The frontend owns the poll loop;
 * backends expose fds and a dispatch hook and emit normalized events. */
#ifndef TNY_BACKEND_H
#define TNY_BACKEND_H

#include <poll.h>
#include "core/events.h"

struct tny_ctx; /* core/config.h */

typedef enum {
    TNY_BK_OPENAI = 0,
    TNY_BK_CURSOR,
    TNY_BK_CODEX,
    TNY_BK_ACP,
    TNY_BK_COUNT
} tny_backend_id;

const char *tny_backend_name(tny_backend_id id);
int tny_backend_from_name(const char *name); /* -1 if unknown */

typedef enum {
    TNY_PERM_DECISION_ALLOW,
    TNY_PERM_DECISION_ALLOW_ALWAYS,
    TNY_PERM_DECISION_DENY
} tny_perm_decision;

typedef struct tny_backend tny_backend;

struct tny_backend {
    tny_backend_id id;
    void *impl;

    /* Establish transport (spawn host, connect socket). 0 ok, -1 error
     * (write a one-line reason into errbuf). Must be lazy: called only
     * when a turn is about to start, never at CLI startup. */
    int (*connect)(tny_backend *b, char *errbuf, size_t errlen);
    void (*disconnect)(tny_backend *b);

    /* Create a new backend session or resume from a stored pointer
     * (session.json host alias, may be NULL for new). */
    int (*create_or_resume)(tny_backend *b, const char *resume_pointer,
                            char *errbuf, size_t errlen);
    /* Serialized pointer for session storage; malloc'd, may return NULL. */
    char *(*session_pointer)(tny_backend *b);

    /* Start one turn. Non-blocking after the initial write. Events flow
     * through cb until TNY_EV_TURN_END. images: NULL-terminated array of
     * file paths or NULL. */
    int (*send)(tny_backend *b, const char *prompt, const char **images,
                tny_event_cb cb, void *ud, char *errbuf, size_t errlen);

    /* Ask the backend to stop the current turn. Backend still emits
     * TURN_END (stop=INTERRUPTED) when the host confirms. */
    void (*cancel)(tny_backend *b);

    void (*respond_permission)(tny_backend *b, const char *perm_id,
                               tny_perm_decision d);

    /* Fill fds the frontend should poll while a turn is active.
     * Returns count written (<= max). */
    int (*pollfds)(tny_backend *b, struct pollfd *fds, int max);
    /* Called when poll reports readiness (or every tick with n==0 for
     * timeouts). Returns 0, or -1 on dead transport. */
    int (*dispatch)(tny_backend *b, struct pollfd *fds, int n);

    /* List selectable models as malloc'd JSON [{"id":…,"name":…},…] into
     * *out. Requires connect() first. Optional: NULL when the host offers
     * no catalog. 0 ok, -1 error (reason in errbuf). */
    int (*list_models)(tny_backend *b, char **out, char *errbuf, size_t errlen);

    /* One-line health check for `tny doctor`. 0 healthy. */
    int (*doctor)(struct tny_ctx *ctx, char *line, size_t linelen);

    void (*destroy)(tny_backend *b);
};

/* Constructors (each backend dir provides one). */
tny_backend *tny_backend_openai_new(struct tny_ctx *ctx);
tny_backend *tny_backend_cursor_new(struct tny_ctx *ctx);
tny_backend *tny_backend_codex_new(struct tny_ctx *ctx);
tny_backend *tny_backend_acp_new(struct tny_ctx *ctx);

tny_backend *tny_backend_create(tny_backend_id id, struct tny_ctx *ctx);

#endif
