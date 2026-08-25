/* runtime.h — private lifecycle engine shared by CLI/TUI/ACP/libtny.
 * Public embedders use include/tny/tny.h; this header may expose pollfd and
 * internal types because it is never installed. */
#ifndef TNY_RUNTIME_H
#define TNY_RUNTIME_H

#include "core/backend.h"
#include "core/config.h"
#include "core/perm.h"
#include "core/session.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct tny_engine tny_engine;

typedef struct tny_owned_event {
    tny_backend_event ev;
    size_t owned_bytes;
    bool hooks_done;
    struct tny_owned_event *next;
} tny_owned_event;

typedef enum {
    TNY_ENGINE_PREPARE_FRESH = 0,
    TNY_ENGINE_PREPARE_CONNECTED,
    TNY_ENGINE_PREPARE_RESUMED
} tny_engine_prepare_state;

typedef enum {
    TNY_ENGINE_NEXT_ERROR = -1,
    TNY_ENGINE_NEXT_TIMEOUT = 0,
    TNY_ENGINE_NEXT_EVENT = 1,
    TNY_ENGINE_NEXT_DRAINED = 2
} tny_engine_next;

/* ctx/session/perm are borrowed and must outlive the engine. The engine owns
 * the non-NULL backend passed to prepare() from the moment prepare is called. */
tny_engine *tny_engine_new(tny_ctx *ctx, tny_session_state *session,
                           perm_engine *perm,
                           tny_perm_decision (*prompt)(const char *,
                                                       const char *, void *),
                           void *prompt_ud);
int tny_engine_prepare(tny_engine *e, tny_backend *prepared,
                       tny_engine_prepare_state state,
                       char *err, size_t errlen);
int tny_engine_start(tny_engine *e, const char *prompt, const char **images,
                     char *err, size_t errlen);
int tny_engine_steer(tny_engine *e, const char *text,
                     char *err, size_t errlen);
void tny_engine_cancel(tny_engine *e);
void tny_engine_respond_permission(tny_engine *e, const char *id,
                                   tny_perm_decision decision);

/* Private external-loop integration. pollfds() fills backend interests;
 * dispatch() consumes readiness and normalizes transport death. */
int tny_engine_pollfds(tny_engine *e, struct pollfd *fds, int max);
int tny_engine_dispatch(tny_engine *e, struct pollfd *fds, int n);

/* Pull one copied event, driving the same poll/dispatch engine. */
tny_engine_next tny_engine_next_event(tny_engine *e, int timeout_ms,
                                      tny_owned_event **out,
                                      char *err, size_t errlen);
/* Nonblocking queue drain for TUI/ACP adapters after private dispatch. */
tny_owned_event *tny_engine_pop_event(tny_engine *e);
void tny_owned_event_free(tny_owned_event *event);

bool tny_engine_ready(const tny_engine *e);
tny_backend_id tny_engine_backend_id(const tny_engine *e);
int tny_engine_openai_steps(tny_engine *e);
const char *tny_engine_openai_toolcalls_json(tny_engine *e);

void tny_engine_free(tny_engine *e);

#endif
