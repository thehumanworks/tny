#include "core/runtime.h"

#include "backends/openai/openai.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENGINE_EVENT_MAX 256u
#define ENGINE_EVENT_BYTES_MAX (1024u * 1024u)
#define ENGINE_RESERVED_EVENTS 2u
#define ENGINE_RESERVED_BYTES 1024u

struct tny_engine {
    tny_ctx *ctx;
    tny_session_state *session;
    perm_engine *perm;
    tny_perm_decision (*prompt)(const char *, const char *, void *);
    void *prompt_ud;

    tny_backend *bk;
    bool active;
    bool terminal;
    bool terminal_popped;
    bool finalize_pending;
    bool overflow_pending;
    bool forcing_error;
    tny_stop_reason stop;
    char *prompt_text;

    tny_owned_event *head;
    tny_owned_event *tail;
    size_t queue_count;
    size_t queue_bytes;
};

static char *dup_bytes(const char *s, size_t n) {
    if (!s) return NULL;
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *dup_cstr(const char *s) {
    return s ? dup_bytes(s, strlen(s)) : NULL;
}

static void free_fields(tny_owned_event *o) {
    free((char *)o->ev.text);
    free((char *)o->ev.tool_name);
    free((char *)o->ev.tool_id);
    free((char *)o->ev.tool_detail);
    free((char *)o->ev.perm_id);
    free((char *)o->ev.perm_summary);
}

void tny_owned_event_free(tny_owned_event *o) {
    if (!o) return;
    free_fields(o);
    free(o);
}

static bool copy_field(const char *src, size_t n, const char **dst,
                       size_t *owned) {
    if (!src) { *dst = NULL; return true; }
    char *copy = dup_bytes(src, n);
    if (!copy) return false;
    *dst = copy;
    *owned += n + 1;
    return true;
}

static tny_owned_event *event_copy(const tny_backend_event *ev) {
    tny_owned_event *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->ev = *ev;
    if (!copy_field(ev->text, ev->text ? ev->text_len : 0,
                    &o->ev.text, &o->owned_bytes) ||
        !copy_field(ev->tool_name, ev->tool_name ? strlen(ev->tool_name) : 0,
                    &o->ev.tool_name, &o->owned_bytes) ||
        !copy_field(ev->tool_id, ev->tool_id ? strlen(ev->tool_id) : 0,
                    &o->ev.tool_id, &o->owned_bytes) ||
        !copy_field(ev->tool_detail, ev->tool_detail ? strlen(ev->tool_detail) : 0,
                    &o->ev.tool_detail, &o->owned_bytes) ||
        !copy_field(ev->perm_id, ev->perm_id ? strlen(ev->perm_id) : 0,
                    &o->ev.perm_id, &o->owned_bytes) ||
        !copy_field(ev->perm_summary,
                    ev->perm_summary ? strlen(ev->perm_summary) : 0,
                    &o->ev.perm_summary, &o->owned_bytes)) {
        tny_owned_event_free(o);
        return NULL;
    }
    return o;
}

static bool is_reserved_kind(tny_event_kind kind) {
    return kind == TNY_EV_ERROR || kind == TNY_EV_TURN_END;
}

static void queue_event(tny_engine *e, const tny_backend_event *ev) {
    if (e->terminal) return; /* duplicate/post-terminal events ignored */

    tny_owned_event *copy = event_copy(ev);
    if (!copy) { e->overflow_pending = true; return; }
    bool reserved = is_reserved_kind(ev->kind);
    size_t count_limit = reserved ? ENGINE_EVENT_MAX
                                  : ENGINE_EVENT_MAX - ENGINE_RESERVED_EVENTS;
    size_t byte_limit = reserved ? ENGINE_EVENT_BYTES_MAX
                                 : ENGINE_EVENT_BYTES_MAX - ENGINE_RESERVED_BYTES;
    if (e->queue_count >= count_limit || copy->owned_bytes > byte_limit ||
        e->queue_bytes > byte_limit - copy->owned_bytes) {
        tny_owned_event_free(copy);
        e->overflow_pending = true;
        return;
    }
    if (e->tail) e->tail->next = copy;
    else e->head = copy;
    e->tail = copy;
    e->queue_count++;
    e->queue_bytes += copy->owned_bytes;
    if (ev->kind == TNY_EV_TURN_END) {
        e->terminal = true;
        e->active = false;
        e->stop = e->forcing_error ? TNY_STOP_ERROR : ev->stop;
        copy->ev.stop = e->stop;
        e->finalize_pending = true;
    }
}

static void backend_event(const tny_backend_event *ev, void *ud) {
    tny_engine *e = ud;
    if (e->terminal) return; /* suppress duplicate or post-terminal events */
    queue_event(e, ev);
}

static void synth_error(tny_engine *e, tny_event_error_kind code,
                        const char *text) {
    tny_backend_event ev = {0};
    ev.kind = TNY_EV_ERROR;
    ev.error_code = code;
    ev.text = text;
    ev.text_len = strlen(text);
    queue_event(e, &ev);
}

static void synth_terminal(tny_engine *e, tny_stop_reason stop) {
    if (e->terminal) return;
    tny_backend_event ev = {0};
    ev.kind = TNY_EV_TURN_END;
    ev.stop = stop;
    queue_event(e, &ev);
}

static void finalize_turn(tny_engine *e) {
    if (!e->finalize_pending || !e->session || !e->bk) return;
    e->finalize_pending = false;
    if (e->bk->session_pointer) {
        char *ptr = e->bk->session_pointer(e->bk);
        if (ptr) {
            session_set_host_pointer(e->session, ptr);
            free(ptr);
        }
    }
    session_set_meta(e->session, tny_provider_name(e->ctx), e->ctx->model);
    if (!session_title(e->session) && e->prompt_text)
        session_set_title(e->session, e->prompt_text);
    session_save(e->session);
}

static void after_backend(tny_engine *e, int dispatch_rc) {
    if (e->overflow_pending && !e->terminal) {
        e->overflow_pending = false;
        e->forcing_error = true;
        synth_error(e, TNY_EVENT_ERROR_BACKPRESSURE,
                    "event queue backpressure");
        if (e->bk && e->bk->cancel) e->bk->cancel(e->bk);
        synth_terminal(e, TNY_STOP_ERROR);
    } else if (dispatch_rc != 0 && !e->terminal) {
        synth_error(e, TNY_EVENT_ERROR_IO, "backend transport failed");
        synth_terminal(e, TNY_STOP_ERROR);
    }
    finalize_turn(e);
}

tny_engine *tny_engine_new(tny_ctx *ctx, tny_session_state *session,
                           perm_engine *perm,
                           tny_perm_decision (*prompt)(const char *,
                                                       const char *, void *),
                           void *prompt_ud) {
    if (!ctx || !session || !perm) return NULL;
    tny_engine *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->ctx = ctx;
    e->session = session;
    e->perm = perm;
    e->prompt = prompt;
    e->prompt_ud = prompt_ud;
    return e;
}

static void drop_backend(tny_engine *e) {
    if (!e || !e->bk) return;
    e->bk->disconnect(e->bk);
    e->bk->destroy(e->bk);
    e->bk = NULL;
}

int tny_engine_prepare(tny_engine *e, tny_backend *prepared,
                       tny_engine_prepare_state state,
                       char *err, size_t errlen) {
    if (!e || e->bk || e->active) {
        if (err && errlen) snprintf(err, errlen, "runtime is not ready for a backend");
        if (prepared) prepared->destroy(prepared);
        return -1;
    }
    tny_backend *bk = prepared;
    if (!bk) {
        if (err && errlen) snprintf(err, errlen, "a prepared backend is required");
        return -1;
    }
    e->bk = bk; /* ownership transfers before any fallible operation */
    if (state == TNY_ENGINE_PREPARE_FRESH &&
        bk->connect(bk, err, errlen) != 0) {
        drop_backend(e);
        return -1;
    }
    if (bk->id == TNY_BK_OPENAI)
        tny_backend_openai_bind(bk, e->session, e->perm, e->prompt, e->prompt_ud);
    if (state != TNY_ENGINE_PREPARE_RESUMED && bk->create_or_resume) {
        const char *ptr = session_host_pointer(e->session);
        const char *owner = session_backend(e->session);
        if (ptr && owner && strcmp(owner, tny_provider_name(e->ctx)) != 0)
            ptr = NULL;
        if (bk->create_or_resume(bk, ptr, err, errlen) != 0) {
            drop_backend(e);
            return -1;
        }
    }
    return 0;
}

int tny_engine_start(tny_engine *e, const char *prompt, const char **images,
                     char *err, size_t errlen) {
    if (!e || !e->bk || !prompt || e->active || e->head ||
        (e->terminal && !e->terminal_popped)) {
        if (err && errlen) snprintf(err, errlen, "runtime is not ready for a turn");
        return -1;
    }
    free(e->prompt_text);
    e->prompt_text = dup_cstr(prompt);
    if (!e->prompt_text) {
        if (err && errlen) snprintf(err, errlen, "out of memory");
        return -1;
    }
    e->active = true;
    e->terminal = false;
    e->terminal_popped = false;
    e->forcing_error = false;
    e->stop = TNY_STOP_ERROR;
    int rc = e->bk->send(e->bk, prompt, images, backend_event, e, err, errlen);
    if (rc != 0) {
        e->active = false;
        return -1; /* start failure creates no event stream */
    }
    after_backend(e, 0);
    return 0;
}

int tny_engine_steer(tny_engine *e, const char *text,
                     char *err, size_t errlen) {
    if (!e || !e->active || !e->bk || !e->bk->steer) {
        if (err && errlen) snprintf(err, errlen, "turn cannot accept steering");
        return -1;
    }
    return e->bk->steer(e->bk, text, err, errlen);
}

void tny_engine_cancel(tny_engine *e) {
    if (!e || !e->active || !e->bk || !e->bk->cancel) return;
    e->bk->cancel(e->bk);
    after_backend(e, 0);
}

void tny_engine_respond_permission(tny_engine *e, const char *id,
                                   tny_perm_decision decision) {
    if (!e || !e->active || !e->bk || !e->bk->respond_permission) return;
    e->bk->respond_permission(e->bk, id, decision);
    after_backend(e, 0);
}

int tny_engine_pollfds(tny_engine *e, struct pollfd *fds, int max) {
    if (!e || !e->active || !e->bk || !e->bk->pollfds) return 0;
    return e->bk->pollfds(e->bk, fds, max);
}

int tny_engine_dispatch(tny_engine *e, struct pollfd *fds, int n) {
    if (!e || !e->active || !e->bk || !e->bk->dispatch) return 0;
    int rc = e->bk->dispatch(e->bk, fds, n);
    after_backend(e, rc);
    return rc;
}

tny_owned_event *tny_engine_pop_event(tny_engine *e) {
    if (!e || !e->head) return NULL;
    tny_owned_event *o = e->head;
    e->head = o->next;
    if (!e->head) e->tail = NULL;
    o->next = NULL;
    e->queue_count--;
    e->queue_bytes -= o->owned_bytes;
    if (o->ev.kind == TNY_EV_TURN_END) e->terminal_popped = true;
    return o;
}

tny_engine_next tny_engine_next_event(tny_engine *e, int timeout_ms,
                                      tny_owned_event **out,
                                      char *err, size_t errlen) {
    if (out) *out = NULL;
    if (!e || !out || timeout_ms < 0) {
        if (err && errlen) snprintf(err, errlen, "invalid next_event arguments");
        return TNY_ENGINE_NEXT_ERROR;
    }
    tny_owned_event *queued = tny_engine_pop_event(e);
    if (queued) { *out = queued; return TNY_ENGINE_NEXT_EVENT; }
    if (e->terminal_popped) return TNY_ENGINE_NEXT_DRAINED;
    if (!e->active) {
        if (err && errlen) snprintf(err, errlen, "no turn is active");
        return TNY_ENGINE_NEXT_ERROR;
    }

    int64_t deadline = monotonic_ms() + timeout_ms;
    do {
        struct pollfd fds[8];
        int n = tny_engine_pollfds(e, fds, 8);
        int remaining = (int)(deadline - monotonic_ms());
        if (remaining < 0) remaining = 0;
        int pr = tny_poll(n ? fds : NULL, (nfds_t)n, remaining);
        if (pr < 0) {
            if (errno == EINTR) continue;
            e->forcing_error = true;
            synth_error(e, TNY_EVENT_ERROR_IO, "runtime poll failed");
            if (e->bk && e->bk->cancel) e->bk->cancel(e->bk);
            synth_terminal(e, TNY_STOP_ERROR);
            finalize_turn(e);
        } else {
            tny_engine_dispatch(e, fds, n);
        }
        queued = tny_engine_pop_event(e);
        if (queued) { *out = queued; return TNY_ENGINE_NEXT_EVENT; }
        if (e->terminal_popped) return TNY_ENGINE_NEXT_DRAINED;
        if (monotonic_ms() >= deadline)
            return TNY_ENGINE_NEXT_TIMEOUT;
    } while (e->active);

    return e->terminal_popped ? TNY_ENGINE_NEXT_DRAINED
                              : TNY_ENGINE_NEXT_TIMEOUT;
}

bool tny_engine_ready(const tny_engine *e) { return e && e->bk; }
tny_backend_id tny_engine_backend_id(const tny_engine *e) {
    return e && e->bk ? e->bk->id : TNY_BK_COUNT;
}

int tny_engine_openai_steps(tny_engine *e) {
    return tny_engine_backend_id(e) == TNY_BK_OPENAI
        ? tny_backend_openai_steps(e->bk) : 1;
}

const char *tny_engine_openai_toolcalls_json(tny_engine *e) {
    return tny_engine_backend_id(e) == TNY_BK_OPENAI
        ? tny_backend_openai_toolcalls_json(e->bk) : "[]";
}

void tny_engine_free(tny_engine *e) {
    if (!e) return;
    if (e->active) tny_engine_cancel(e);
    drop_backend(e);
    tny_owned_event *event;
    while ((event = tny_engine_pop_event(e))) tny_owned_event_free(event);
    free(e->prompt_text);
    free(e);
}
