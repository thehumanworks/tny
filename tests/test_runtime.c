/* test_runtime.c — private engine ownership and terminal normalization. */
#include "greatest.h"
#include "core/runtime.h"
#include "util/util.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    tny_backend_event_cb cb;
    void *ud;
    int mode;
    bool dispatched;
} fake_runtime_backend;

static int fake_connect(tny_backend *b, char *err, size_t errlen) {
    (void)b; (void)err; (void)errlen; return 0;
}
static void fake_disconnect(tny_backend *b) { (void)b; }
static int fake_resume(tny_backend *b, const char *p, char *e, size_t n) {
    (void)b; (void)p; (void)e; (void)n; return 0;
}
static int fake_send(tny_backend *b, const char *p, const char **images,
                     tny_backend_event_cb cb, void *ud, char *e, size_t n) {
    (void)p; (void)images; (void)e; (void)n;
    fake_runtime_backend *f = b->impl;
    f->cb = cb; f->ud = ud; f->dispatched = false;
    return 0;
}
static void fake_cancel(tny_backend *b) {
    fake_runtime_backend *f = b->impl;
    tny_backend_event ev = {0}; ev.kind = TNY_EV_TURN_END;
    ev.stop = TNY_STOP_INTERRUPTED;
    f->cb(&ev, f->ud);
}
static int fake_pollfds(tny_backend *b, struct pollfd *fds, int max) {
    (void)b; (void)fds; (void)max; return 0;
}
static int fake_dispatch(tny_backend *b, struct pollfd *fds, int n) {
    (void)fds; (void)n;
    fake_runtime_backend *f = b->impl;
    if (f->dispatched) return 0;
    f->dispatched = true;
    if (f->mode == 1) return -1; /* transport death without events */
    if (f->mode == 2) {
        f->mode = 0; /* the next turn proves overflow state was cleared */
        for (int i = 0; i < 300; i++) {
            tny_backend_event status = {0}; status.kind = TNY_EV_STATUS;
            status.text = "burst"; status.text_len = 5;
            f->cb(&status, f->ud);
        }
        return 0;
    }
    if (f->mode == 3) return 0; /* quiet backend: timeout path */
    char borrowed[] = "copied delta";
    tny_backend_event text = {0}; text.kind = TNY_EV_TEXT_DELTA;
    text.text = borrowed; text.text_len = strlen(borrowed);
    f->cb(&text, f->ud);
    memset(borrowed, 'x', strlen(borrowed));
    tny_backend_event end = {0}; end.kind = TNY_EV_TURN_END; end.stop = TNY_STOP_DONE;
    f->cb(&end, f->ud);
    f->cb(&end, f->ud); /* duplicate must be suppressed */
    return 0;
}
static void fake_destroy(tny_backend *b) { free(b->impl); free(b); }

static tny_backend *fake_backend(int mode) {
    tny_backend *b = calloc(1, sizeof *b);
    fake_runtime_backend *f = calloc(1, sizeof *f);
    if (!b || !f) abort();
    f->mode = mode;
    b->id = TNY_BK_ACP; b->impl = f;
    b->connect = fake_connect; b->disconnect = fake_disconnect;
    b->create_or_resume = fake_resume; b->send = fake_send;
    b->cancel = fake_cancel; b->pollfds = fake_pollfds;
    b->dispatch = fake_dispatch; b->destroy = fake_destroy;
    return b;
}

typedef struct {
    tny_ctx *ctx;
    tny_session_state *session;
    perm_engine *perm;
    tny_engine *engine;
} fixture;

static fixture fixture_new(int mode) {
    char root[] = "/tmp/tny-runtime-test-XXXXXX";
    if (!mkdtemp(root)) abort();
    setenv("HOME", root, 1);
    char ws[512]; snprintf(ws, sizeof ws, "%s/ws", root); mkdir_p(ws);
    fixture x = {0};
    x.ctx = tny_ctx_load(ws);
    x.ctx->backend = TNY_BK_ACP;
    x.ctx->no_save = true;
    x.session = session_new(x.ctx);
    x.perm = perm_new(x.ctx);
    x.engine = tny_engine_new(x.ctx, x.session, x.perm, NULL, NULL);
    char err[128];
    if (!x.engine || tny_engine_prepare(x.engine, fake_backend(mode),
                                        TNY_ENGINE_PREPARE_FRESH,
                                        err, sizeof err) != 0)
        abort();
    return x;
}

static void fixture_free(fixture *x) {
    tny_engine_free(x->engine);
    perm_free(x->perm);
    session_close(x->session);
    tny_ctx_free(x->ctx);
}

TEST runtime_copies_events_and_suppresses_duplicate_terminal(void) {
    fixture x = fixture_new(0);
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    tny_owned_event *ev = NULL;
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT,
              tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
    ASSERT_EQ(TNY_EV_TEXT_DELTA, ev->ev.kind);
    ASSERT_STR_EQ("copied delta", ev->ev.text);
    tny_owned_event_free(ev);
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT,
              tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
    ASSERT_EQ(TNY_EV_TURN_END, ev->ev.kind);
    ASSERT_EQ(TNY_STOP_DONE, ev->ev.stop);
    tny_owned_event_free(ev);
    ASSERT_EQ(TNY_ENGINE_NEXT_DRAINED,
              tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
    fixture_free(&x);
    PASS();
}

TEST runtime_synthesizes_transport_error_and_terminal(void) {
    fixture x = fixture_new(1);
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    tny_owned_event *ev = NULL;
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT,
              tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
    ASSERT_EQ(TNY_EV_ERROR, ev->ev.kind);
    ASSERT_STR_EQ("backend transport failed", ev->ev.text);
    tny_owned_event_free(ev);
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT,
              tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
    ASSERT_EQ(TNY_EV_TURN_END, ev->ev.kind);
    ASSERT_EQ(TNY_STOP_ERROR, ev->ev.stop);
    tny_owned_event_free(ev);
    fixture_free(&x);
    PASS();
}

TEST runtime_overflow_keeps_error_and_single_terminal(void) {
    fixture x = fixture_new(2);
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    tny_owned_event *ev = NULL;
    int errors = 0, terminals = 0, statuses = 0;
    for (;;) {
        tny_engine_next next = tny_engine_next_event(x.engine, 0, &ev,
                                                     err, sizeof err);
        if (next == TNY_ENGINE_NEXT_DRAINED) break;
        ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, next);
        if (ev->ev.kind == TNY_EV_STATUS) statuses++;
        if (ev->ev.kind == TNY_EV_ERROR) {
            errors++;
            ASSERT_EQ(TNY_EVENT_ERROR_BACKPRESSURE, ev->ev.error_code);
        }
        if (ev->ev.kind == TNY_EV_TURN_END) {
            terminals++;
            ASSERT_EQ(TNY_STOP_ERROR, ev->ev.stop);
        }
        tny_owned_event_free(ev);
    }
    ASSERT_EQ(254, statuses); /* two queue slots are reserved for error/end */
    ASSERT_EQ(1, errors);
    ASSERT_EQ(1, terminals);

    ASSERT_EQ(0, tny_engine_start(x.engine, "after overflow", NULL,
                                  err, sizeof err));
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT,
              tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
    ASSERT_EQ(TNY_EV_TEXT_DELTA, ev->ev.kind);
    tny_owned_event_free(ev);
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT,
              tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
    ASSERT_EQ(TNY_EV_TURN_END, ev->ev.kind);
    ASSERT_EQ(TNY_STOP_DONE, ev->ev.stop);
    tny_owned_event_free(ev);
    fixture_free(&x);
    PASS();
}

TEST runtime_cancel_emits_one_interrupted_terminal(void) {
    fixture x = fixture_new(0);
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    tny_engine_cancel(x.engine);
    int terminals = 0;
    tny_owned_event *ev;
    while ((ev = tny_engine_pop_event(x.engine))) {
        if (ev->ev.kind == TNY_EV_TURN_END) {
            terminals++;
            ASSERT_EQ(TNY_STOP_INTERRUPTED, ev->ev.stop);
        }
        tny_owned_event_free(ev);
    }
    ASSERT_EQ(1, terminals);
    fixture_free(&x);
    PASS();
}

TEST runtime_next_event_waits_without_spinning(void) {
    fixture x = fixture_new(3);
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    tny_owned_event *ev = NULL;
    int64_t start = monotonic_ms();
    ASSERT_EQ(TNY_ENGINE_NEXT_TIMEOUT,
              tny_engine_next_event(x.engine, 40, &ev, err, sizeof err));
    int64_t elapsed = monotonic_ms() - start;
    ASSERT(elapsed >= 30);
    ASSERT(elapsed < 500);
    fixture_free(&x);
    PASS();
}

SUITE(runtime_suite) {
    RUN_TEST(runtime_copies_events_and_suppresses_duplicate_terminal);
    RUN_TEST(runtime_synthesizes_transport_error_and_terminal);
    RUN_TEST(runtime_overflow_keeps_error_and_single_terminal);
    RUN_TEST(runtime_cancel_emits_one_interrupted_terminal);
    RUN_TEST(runtime_next_event_waits_without_spinning);
}
