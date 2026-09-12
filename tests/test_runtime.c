/* test_runtime.c — private engine ownership and terminal normalization. */
#include "greatest.h"
#include "core/runtime.h"
#include "cli/cli.h"
#include "core/event_jsonl.h"
#include "core/instructions.h"
#include "core/extensions.h"
#include "core/tasks.h"
#include "core/skills.h"
#include "util/util.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>

typedef struct {
    tny_backend_event_cb cb;
    void *ud;
    int mode;
    bool dispatched;
    char *prompts[8];
    char *steers[8];
    int sends;
    int steer_count;
    int permission_responses;
    tny_perm_decision permission_decision;
    int cancels;
    int64_t deadline_ms;
} fake_runtime_backend;

static int fake_connect(tny_backend *b, char *err, size_t errlen) {
    (void)b;
    (void)err;
    (void)errlen;
    return 0;
}
static void fake_disconnect(tny_backend *b) { (void)b; }
static int fake_resume(tny_backend *b, const char *p, char *e, size_t n) {
    (void)b;
    (void)p;
    (void)e;
    (void)n;
    return 0;
}
static int fake_send(tny_backend *b, const char *p, const char **images, tny_backend_event_cb cb,
                     void *ud, char *e, size_t n) {
    (void)p;
    (void)images;
    (void)e;
    (void)n;
    fake_runtime_backend *f = b->impl;
    if (f->sends < (int)(sizeof f->prompts / sizeof *f->prompts)) f->prompts[f->sends] = xstrdup(p);
    f->sends++;
    f->cb = cb;
    f->ud = ud;
    f->dispatched = false;
    if (f->mode == 6) f->deadline_ms = monotonic_ms() + 30;
    if (f->mode == 7) f->deadline_ms = monotonic_ms() + 20;
    return 0;
}
static void fake_cancel(tny_backend *b) {
    fake_runtime_backend *f = b->impl;
    f->cancels++;
    tny_backend_event ev = {0};
    ev.kind = TNY_EV_TURN_END;
    ev.stop = TNY_STOP_INTERRUPTED;
    f->cb(&ev, f->ud);
}
static int fake_steer(tny_backend *b, const char *text, char *err, size_t errlen) {
    fake_runtime_backend *f = b->impl;
    if (f->steer_count < (int)(sizeof f->steers / sizeof *f->steers))
        f->steers[f->steer_count] = xstrdup(text);
    f->steer_count++;
    if (f->mode == 4) {
        snprintf(err, errlen, "synchronous rejection");
        return -1;
    }
    return 0;
}
static int fake_pollfds(tny_backend *b, struct pollfd *fds, int max) {
    (void)b;
    (void)fds;
    (void)max;
    return 0;
}
static int fake_poll_timeout(tny_backend *b) {
    fake_runtime_backend *f = b->impl;
    if ((f->mode < 6 || f->mode > 8) || f->dispatched) return -1;
    if (f->mode == 8) return 0;
    int64_t remaining = f->deadline_ms - monotonic_ms();
    return remaining > 0 ? (int)remaining : 0;
}
static int fake_dispatch(tny_backend *b, struct pollfd *fds, int n) {
    (void)fds;
    (void)n;
    fake_runtime_backend *f = b->impl;
    if ((f->mode == 6 || f->mode == 7) && monotonic_ms() < f->deadline_ms) return 0;
    if (f->dispatched) return 0;
    f->dispatched = true;
    if (f->mode == 7) return 0;  /* deadline wake is internal, not a caller timeout */
    if (f->mode == 1) return -1; /* transport death without events */
    if (f->mode == 2) {
        f->mode = 0; /* the next turn proves overflow state was cleared */
        for (int i = 0; i < 300; i++) {
            tny_backend_event status = {0};
            status.kind = TNY_EV_STATUS;
            status.text = "burst";
            status.text_len = 5;
            f->cb(&status, f->ud);
        }
        return 0;
    }
    if (f->mode == 3) return 0; /* quiet backend: timeout path */
    if (f->mode == 5) {
        tny_backend_event permission = {0};
        permission.kind = TNY_EV_PERMISSION;
        permission.perm_id = "permission-1";
        permission.perm_summary = "write_file /outside";
        permission.perm_options = TNY_PERM_ALLOW_ONCE | TNY_PERM_DENY;
        f->cb(&permission, f->ud);
        return 0;
    }
    char borrowed[] = "copied delta";
    tny_backend_event text = {0};
    text.kind = TNY_EV_TEXT_DELTA;
    text.text = borrowed;
    text.text_len = strlen(borrowed);
    f->cb(&text, f->ud);
    memset(borrowed, 'x', strlen(borrowed));
    tny_backend_event end = {0};
    end.kind = TNY_EV_TURN_END;
    end.stop = TNY_STOP_DONE;
    f->cb(&end, f->ud);
    f->cb(&end, f->ud); /* duplicate must be suppressed */
    return 0;
}
static void fake_respond_permission(tny_backend *b, const char *id, tny_perm_decision decision) {
    fake_runtime_backend *f = b->impl;
    if (!id || strcmp(id, "permission-1") != 0) return;
    f->permission_responses++;
    f->permission_decision = decision;
    tny_backend_event end = {0};
    end.kind = TNY_EV_TURN_END;
    end.stop = decision == TNY_PERM_DECISION_DENY ? TNY_STOP_DENIED : TNY_STOP_DONE;
    f->cb(&end, f->ud);
}
static void fake_destroy(tny_backend *b) {
    fake_runtime_backend *f = b->impl;
    for (int i = 0; i < 8; i++) free(f->prompts[i]);
    for (int i = 0; i < 8; i++) free(f->steers[i]);
    free(f);
    free(b);
}

static tny_backend *fake_backend(int mode, fake_runtime_backend **out) {
    tny_backend *b = calloc(1, sizeof *b);
    fake_runtime_backend *f = calloc(1, sizeof *f);
    if (!b || !f) abort();
    f->mode = mode;
    b->id = TNY_BK_ACP;
    b->impl = f;
    b->connect = fake_connect;
    b->disconnect = fake_disconnect;
    b->create_or_resume = fake_resume;
    b->send = fake_send;
    b->steer = fake_steer;
    b->cancel = fake_cancel;
    b->pollfds = fake_pollfds;
    b->poll_timeout = fake_poll_timeout;
    b->respond_permission = fake_respond_permission;
    b->dispatch = fake_dispatch;
    b->destroy = fake_destroy;
    if (out) *out = f;
    return b;
}

typedef struct {
    tny_ctx *ctx;
    tny_session_state *session;
    perm_engine *perm;
    tny_engine *engine;
    fake_runtime_backend *fake;
    char *old_home;
} fixture;

static fixture fixture_new_ext(int mode, const char *extension_source,
                               int max_extension_iterations) {
    char root[] = "/tmp/tny-runtime-test-XXXXXX";
    if (!mkdtemp(root)) abort();
    const char *home = getenv("HOME");
    char *old_home = home ? xstrdup(home) : NULL;
    setenv("HOME", root, 1);
    char ws[512];
    snprintf(ws, sizeof ws, "%s/ws", root);
    mkdir_p(ws);
    if (extension_source) {
        char tny_dir[512];
        snprintf(tny_dir, sizeof tny_dir, "%s/.tny/extensions", root);
        mkdir_p(tny_dir);
        char *entry = path_join(tny_dir, "gate.py");
        file_write_atomic(entry, extension_source, strlen(extension_source));
        free(entry);
    }
    fixture x = {0};
    x.old_home = old_home;
    x.ctx = tny_ctx_load(ws);
    x.ctx->backend = TNY_BK_ACP;
    x.ctx->no_save = true;
    x.ctx->max_extension_iterations = max_extension_iterations;
    x.session = session_new(x.ctx);
    x.perm = perm_new(x.ctx);
    x.engine = tny_engine_new(x.ctx, x.session, x.perm, NULL, NULL);
    char err[128];
    if (!x.engine || tny_engine_prepare(x.engine, fake_backend(mode, &x.fake),
                                        TNY_ENGINE_PREPARE_FRESH, err, sizeof err) != 0)
        abort();
    return x;
}

static fixture fixture_new(int mode) { return fixture_new_ext(mode, NULL, 0); }

static void fixture_free(fixture *x) {
    tny_engine_free(x->engine);
    perm_free(x->perm);
    session_close(x->session);
    tny_ctx_free(x->ctx);
    if (x->old_home) setenv("HOME", x->old_home, 1);
    else unsetenv("HOME");
    free(x->old_home);
}

static int drain_engine(tny_engine *engine, tny_stop_reason *stop) {
    char err[256];
    int events = 0;
    tny_owned_event *ev = NULL;
    for (;;) {
        tny_engine_next next = tny_engine_next_event(engine, 50, &ev, err, sizeof err);
        if (next == TNY_ENGINE_NEXT_DRAINED) break;
        if (next != TNY_ENGINE_NEXT_EVENT) return -1;
        events++;
        if (ev->ev.kind == TNY_EV_TURN_END && stop) *stop = ev->ev.stop;
        tny_owned_event_free(ev);
    }
    return events;
}

static int drain_engine_kind(tny_engine *engine, tny_stop_reason *stop, tny_event_kind kind,
                             int *kind_count) {
    char err[256];
    int events = 0;
    *kind_count = 0;
    tny_owned_event *ev = NULL;
    for (;;) {
        tny_engine_next next = tny_engine_next_event(engine, 50, &ev, err, sizeof err);
        if (next == TNY_ENGINE_NEXT_DRAINED) break;
        if (next != TNY_ENGINE_NEXT_EVENT) return -1;
        events++;
        if (ev->ev.kind == kind) (*kind_count)++;
        if (ev->ev.kind == TNY_EV_TURN_END && stop) *stop = ev->ev.stop;
        tny_owned_event_free(ev);
    }
    return events;
}

static int count_text(const char *haystack, const char *needle) {
    int count = 0;
    size_t n = strlen(needle);
    for (const char *p = haystack; (p = strstr(p, needle)) != NULL; p += n) count++;
    return count;
}

TEST runtime_copies_events_and_suppresses_duplicate_terminal(void) {
    fixture x = fixture_new(0);
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    tny_owned_event *ev = NULL;
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
    ASSERT_EQ(TNY_EV_TEXT_DELTA, ev->ev.kind);
    ASSERT_STR_EQ("copied delta", ev->ev.text);
    tny_owned_event_free(ev);
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
    ASSERT_EQ(TNY_EV_TURN_END, ev->ev.kind);
    ASSERT_EQ(TNY_STOP_DONE, ev->ev.stop);
    tny_owned_event_free(ev);
    ASSERT_EQ(TNY_ENGINE_NEXT_DRAINED, tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
    fixture_free(&x);

    PASS();
}

/* --system-prompt fallback (docs/adr/0045): host backends with no schema
 * field get the text prepended to the first user message only. */
TEST runtime_system_prompt_prefixes_only_the_first_user_message(void) {
    fixture x = fixture_new(0);
    x.ctx->system_prompt = xstrdup("Answer like a pirate.");
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    ASSERT(drain_engine(x.engine, NULL) >= 0);
    ASSERT_STR_EQ("Answer like a pirate.\n\nhello", x.fake->prompts[0]);
    ASSERT_EQ(0, tny_engine_start(x.engine, "again", NULL, err, sizeof err));
    ASSERT(drain_engine(x.engine, NULL) >= 0);
    ASSERT_STR_EQ("again", x.fake->prompts[1]);
    fixture_free(&x);
    PASS();
}

TEST runtime_task_precedes_explicit_system_prompt_on_host_first_turn(void) {
    fixture x = fixture_new(0);
    ASSERT_EQ(TNY_TASK_OK, tny_task_set_explicit(x.ctx, "review", "Review task body.", "explicit"));
    x.ctx->system_prompt = xstrdup("Explicit system addition.");
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    ASSERT(drain_engine(x.engine, NULL) >= 0);
    const char *task = strstr(x.fake->prompts[0], "Review task body.");
    const char *explicit_prompt = strstr(x.fake->prompts[0], "Explicit system addition.");
    const char *user = strstr(x.fake->prompts[0], "hello");
    ASSERT(task && explicit_prompt && user);
    ASSERT(task < explicit_prompt);
    ASSERT(explicit_prompt < user);
    ASSERT_EQ(1, count_text(x.fake->prompts[0], "Review task body."));
    ASSERT_EQ(0, tny_engine_start(x.engine, "again", NULL, err, sizeof err));
    ASSERT(drain_engine(x.engine, NULL) >= 0);
    ASSERT_STR_EQ("again", x.fake->prompts[1]);
    fixture_free(&x);
    PASS();
}

/* docs/adr/0056: a `$name` / `/name` mention puts the SKILL.md ahead of the
 * user text on every backend; the typed text is what the session shows and a
 * repeat mention in the same verbatim window sends a reminder, not a body. */
TEST runtime_skill_mention_rides_ahead_of_the_user_text(void) {
    fixture x = fixture_new(0);
    char *dir = path_join(x.ctx->cwd, "skills/deploy");
    mkdir_p(dir);
    char *sf = path_join(dir, "SKILL.md");
    const char *skill = "---\nname: deploy\ndescription: ship it\n---\nTag, then push.\n";
    file_write_atomic(sf, skill, strlen(skill));
    x.ctx->system_prompt = xstrdup("Be brief.");
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "$deploy now", NULL, err, sizeof err));
    ASSERT(drain_engine(x.engine, NULL) >= 0);
    buf_t want;
    buf_init(&want);
    buf_appendf(&want,
                "Be brief.\n\n<skill name=\"deploy\" path=\"%s\">\n%s</skill>\n\n$deploy now", sf,
                skill);
    ASSERT_STR_EQ(want.data, x.fake->prompts[0]);
    buf_free(&want);
    ASSERT(session_skill_injected(x.session, "deploy"));
    ASSERT_STR_EQ("$deploy now", session_message_display(x.session, 0));

    ASSERT_EQ(0, tny_engine_start(x.engine, "/deploy again", NULL, err, sizeof err));
    ASSERT(drain_engine(x.engine, NULL) >= 0);
    ASSERT(strstr(x.fake->prompts[1], "already loaded earlier"));
    ASSERT(!strstr(x.fake->prompts[1], "Tag, then push."));
    ASSERT(strstr(x.fake->prompts[1], "/>\n\n/deploy again"));

    ASSERT_EQ(0,
              tny_engine_start(x.engine, "see /deploy/logs and $deployer", NULL, err, sizeof err));
    ASSERT(drain_engine(x.engine, NULL) >= 0);
    ASSERT_STR_EQ("see /deploy/logs and $deployer", x.fake->prompts[2]);
    free(sf);
    free(dir);
    fixture_free(&x);
    PASS();
}

TEST runtime_system_prompt_skips_resumed_host_sessions(void) {
    fixture x = fixture_new(0);
    x.ctx->system_prompt = xstrdup("Answer like a pirate.");
    session_set_host_pointer(x.session, "thr_resumed");
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    ASSERT(drain_engine(x.engine, NULL) >= 0);
    ASSERT_STR_EQ("hello", x.fake->prompts[0]);
    fixture_free(&x);
    PASS();
}

TEST runtime_synthesizes_transport_error_and_terminal(void) {
    fixture x = fixture_new(1);
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    tny_owned_event *ev = NULL;
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
    ASSERT_EQ(TNY_EV_ERROR, ev->ev.kind);
    ASSERT_STR_EQ("backend transport failed", ev->ev.text);
    tny_owned_event_free(ev);
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
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
        tny_engine_next next = tny_engine_next_event(x.engine, 0, &ev, err, sizeof err);
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

    ASSERT_EQ(0, tny_engine_start(x.engine, "after overflow", NULL, err, sizeof err));
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
    ASSERT_EQ(TNY_EV_TEXT_DELTA, ev->ev.kind);
    tny_owned_event_free(ev);
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
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
    ASSERT_EQ(TNY_ENGINE_NEXT_TIMEOUT, tny_engine_next_event(x.engine, 40, &ev, err, sizeof err));
    int64_t elapsed = monotonic_ms() - start;
    ASSERT(elapsed >= 30);
    ASSERT(elapsed < 500);
    fixture_free(&x);
    PASS();
}

TEST runtime_backend_deadline_caps_long_caller_wait(void) {
    fixture x = fixture_new(6);
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    tny_owned_event *ev = NULL;
    int64_t start = monotonic_ms();
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, tny_engine_next_event(x.engine, 1000, &ev, err, sizeof err));
    int64_t elapsed = monotonic_ms() - start;
    ASSERT(elapsed >= 20);
    ASSERT(elapsed < 500);
    ASSERT(ev);
    ASSERT_EQ(TNY_EV_TEXT_DELTA, ev->ev.kind);
    tny_owned_event_free(ev);
    ev = NULL;
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
    ASSERT(ev);
    ASSERT_EQ(TNY_EV_TURN_END, ev->ev.kind);
    tny_owned_event_free(ev);
    fixture_free(&x);
    PASS();
}

TEST runtime_backend_deadline_preserves_caller_timeout(void) {
    fixture x = fixture_new(7);
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    tny_owned_event *ev = NULL;
    int64_t start = monotonic_ms();
    ASSERT_EQ(TNY_ENGINE_NEXT_TIMEOUT, tny_engine_next_event(x.engine, 100, &ev, err, sizeof err));
    int64_t elapsed = monotonic_ms() - start;
    ASSERT_EQ(NULL, ev);
    ASSERT(elapsed >= 75);
    ASSERT(elapsed < 500);
    fixture_free(&x);
    PASS();
}

TEST runtime_backend_due_deadline_dispatches_without_caller_delay(void) {
    fixture x = fixture_new(8);
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    tny_owned_event *ev = NULL;
    int64_t start = monotonic_ms();
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, tny_engine_next_event(x.engine, 1000, &ev, err, sizeof err));
    int64_t elapsed = monotonic_ms() - start;
    ASSERT(elapsed < 100);
    ASSERT(ev);
    ASSERT_EQ(TNY_EV_TEXT_DELTA, ev->ev.kind);
    tny_owned_event_free(ev);
    fixture_free(&x);
    PASS();
}

TEST runtime_oom_uses_reserved_error_and_terminal_once(void) {
    tny_engine_fail_oom(NULL); /* partial-constructor cleanup is harmless */
    fixture x = fixture_new(0);
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    tny_engine_fail_oom(x.engine);
    tny_engine_fail_oom(x.engine); /* settled calls are idempotent */
    int errors = 0, terminals = 0;
    tny_owned_event *ev = NULL;
    while ((ev = tny_engine_pop_event(x.engine))) {
        if (ev->ev.kind == TNY_EV_ERROR) {
            errors++;
            ASSERT_EQ(TNY_EVENT_ERROR_OOM, ev->ev.error_code);
            ASSERT_STR_EQ("out of memory", ev->ev.text);
        }
        if (ev->ev.kind == TNY_EV_TURN_END) {
            terminals++;
            ASSERT_EQ(TNY_STOP_ERROR, ev->ev.stop);
        }
        tny_owned_event_free(ev);
    }
    ASSERT_EQ(1, x.fake->cancels);
    ASSERT_EQ(1, errors);
    ASSERT_EQ(1, terminals);

    ASSERT_EQ(0, tny_engine_start(x.engine, "after oom", NULL, err, sizeof err));
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
    ASSERT_EQ(TNY_EV_TEXT_DELTA, ev->ev.kind);
    tny_owned_event_free(ev);
    ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, tny_engine_next_event(x.engine, 0, &ev, err, sizeof err));
    ASSERT_EQ(TNY_EV_TURN_END, ev->ev.kind);
    ASSERT_EQ(TNY_STOP_DONE, ev->ev.stop);
    tny_owned_event_free(ev);
    fixture_free(&x);
    PASS();
}

TEST runtime_extension_continues_visibly_then_settles(void) {
    const char *source = "from tny_ext import AgentEndEvent, continue_with\n"
                         "def setup(api):\n"
                         "    @api.on(AgentEndEvent)\n"
                         "    def gate(event):\n"
                         "        if event.continuation_count == 0:\n"
                         "            return continue_with('verify again')\n";
    fixture x = fixture_new_ext(0, source, 0);
    char err[256];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    int text = 0, visible = 0, terminal = 0;
    tny_owned_event *ev = NULL;
    for (;;) {
        tny_engine_next next = tny_engine_next_event(x.engine, 50, &ev, err, sizeof err);
        if (next == TNY_ENGINE_NEXT_DRAINED) break;
        ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, next);
        if (ev->ev.kind == TNY_EV_TEXT_DELTA) text++;
        if (ev->ev.kind == TNY_EV_USER_MESSAGE) {
            visible++;
            ASSERT_STR_EQ("verify again", ev->ev.text);
        }
        if (ev->ev.kind == TNY_EV_TURN_END) {
            terminal++;
            ASSERT_EQ(TNY_STOP_DONE, ev->ev.stop);
        }
        tny_owned_event_free(ev);
    }
    ASSERT_EQ(2, x.fake->sends);
    ASSERT_STR_EQ("hello", x.fake->prompts[0]);
    ASSERT_STR_EQ("verify again", x.fake->prompts[1]);
    ASSERT_EQ(2, text);
    ASSERT_EQ(1, visible);
    ASSERT_EQ(1, terminal);
    fixture_free(&x);
    PASS();
}

TEST runtime_extension_positive_continuation_cap_settles(void) {
    const char *source = "from tny_ext import AgentEndEvent, continue_with\n"
                         "def setup(api):\n"
                         "    @api.on(AgentEndEvent)\n"
                         "    def gate(event):\n"
                         "        return continue_with('again')\n";
    fixture x = fixture_new_ext(0, source, 1);
    char err[256];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    int limit_status = 0, terminal = 0;
    tny_owned_event *ev = NULL;
    for (;;) {
        tny_engine_next next = tny_engine_next_event(x.engine, 50, &ev, err, sizeof err);
        if (next == TNY_ENGINE_NEXT_DRAINED) break;
        ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, next);
        if (ev->ev.kind == TNY_EV_STATUS && ev->ev.text &&
            strstr(ev->ev.text, "continuation limit"))
            limit_status++;
        if (ev->ev.kind == TNY_EV_TURN_END) terminal++;
        tny_owned_event_free(ev);
    }
    ASSERT_EQ(2, x.fake->sends);
    ASSERT_EQ(1, limit_status);
    ASSERT_EQ(1, terminal);
    fixture_free(&x);
    PASS();
}

TEST runtime_extension_failure_is_visible_and_fail_open(void) {
    const char *source = "from tny_ext import TextDeltaEvent\n"
                         "def setup(api):\n"
                         "    @api.on(TextDeltaEvent)\n"
                         "    def broken(event):\n"
                         "        raise RuntimeError('hook exploded')\n";
    fixture x = fixture_new_ext(0, source, 0);
    char err[256];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    int failure_status = 0, terminal = 0;
    tny_owned_event *ev = NULL;
    for (;;) {
        tny_engine_next next = tny_engine_next_event(x.engine, 50, &ev, err, sizeof err);
        if (next == TNY_ENGINE_NEXT_DRAINED) break;
        ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, next);
        if (ev->ev.kind == TNY_EV_STATUS && ev->ev.text && strstr(ev->ev.text, "hook exploded"))
            failure_status++;
        if (ev->ev.kind == TNY_EV_TURN_END) {
            terminal++;
            ASSERT_EQ(TNY_STOP_DONE, ev->ev.stop);
        }
        tny_owned_event_free(ev);
    }
    ASSERT_EQ(1, failure_status);
    ASSERT_EQ(1, terminal);
    fixture_free(&x);
    PASS();
}

TEST runtime_extension_stop_requests_cancel_at_safe_boundary(void) {
    const char *source = "from tny_ext import StatusEvent, stop\n"
                         "def setup(api):\n"
                         "    @api.on(StatusEvent)\n"
                         "    def halt(event):\n"
                         "        return stop('condition met')\n";
    fixture x = fixture_new_ext(3, source, 0);
    char err[256];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    tny_backend_event status = {0};
    status.kind = TNY_EV_STATUS;
    status.text = "checkpoint";
    status.text_len = strlen(status.text);
    x.fake->cb(&status, x.fake->ud);
    ASSERT_EQ(0, tny_engine_dispatch(x.engine, NULL, 0));
    int terminal = 0;
    tny_owned_event *ev;
    while ((ev = tny_engine_pop_event(x.engine))) {
        if (ev->ev.kind == TNY_EV_TURN_END) {
            terminal++;
            ASSERT_EQ(TNY_STOP_INTERRUPTED, ev->ev.stop);
        }
        tny_owned_event_free(ev);
    }
    ASSERT_EQ(1, terminal);
    fixture_free(&x);
    PASS();
}

TEST runtime_extension_host_state_persists_across_sessions(void) {
    const char *source = "from tny_ext import BeforeAgentStartEvent, SessionStartEvent, context\n"
                         "starts = 0\n"
                         "def setup(api):\n"
                         "    @api.on(SessionStartEvent)\n"
                         "    def session(event):\n"
                         "        global starts\n"
                         "        starts += 1\n"
                         "    @api.on(BeforeAgentStartEvent)\n"
                         "    def inject(event):\n"
                         "        return context(str(starts), custom_type='session-count')\n";
    fixture x = fixture_new_ext(0, source, 0);
    char err[256];
    ASSERT_EQ(0, tny_engine_start(x.engine, "first", NULL, err, sizeof err));
    int custom = 0;
    tny_owned_event *ev = NULL;
    for (;;) {
        tny_engine_next next = tny_engine_next_event(x.engine, 50, &ev, err, sizeof err);
        if (next == TNY_ENGINE_NEXT_DRAINED) break;
        ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, next);
        if (ev->ev.kind == TNY_EV_CUSTOM_MESSAGE) {
            custom++;
            ASSERT_STR_EQ("1", ev->ev.text);
        }
        tny_owned_event_free(ev);
    }
    ASSERT_EQ(1, custom);
    ASSERT(strstr(x.fake->prompts[0], "session-count"));

    tny_engine_free(x.engine);
    perm_free(x.perm);
    session_close(x.session);
    x.session = session_new(x.ctx);
    x.perm = perm_new(x.ctx);
    x.engine = tny_engine_new(x.ctx, x.session, x.perm, NULL, NULL);
    ASSERT(x.engine);
    ASSERT_EQ(0, tny_engine_prepare(x.engine, fake_backend(0, &x.fake), TNY_ENGINE_PREPARE_FRESH,
                                    err, sizeof err));
    ASSERT_EQ(0, tny_engine_start(x.engine, "second", NULL, err, sizeof err));
    custom = 0;
    for (;;) {
        tny_engine_next next = tny_engine_next_event(x.engine, 50, &ev, err, sizeof err);
        if (next == TNY_ENGINE_NEXT_DRAINED) break;
        ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, next);
        if (ev->ev.kind == TNY_EV_CUSTOM_MESSAGE) {
            custom++;
            ASSERT_STR_EQ("2", ev->ev.text);
        }
        tny_owned_event_free(ev);
    }
    ASSERT_EQ(1, custom);
    fixture_free(&x);
    PASS();
}

TEST runtime_extension_stop_suppresses_earlier_continuation(void) {
    const char *source = "from tny_ext import AgentEndEvent, continue_with, stop\n"
                         "def setup(api):\n"
                         "    @api.on(AgentEndEvent)\n"
                         "    def first(event):\n"
                         "        return continue_with('must not be sent')\n"
                         "    @api.on(AgentEndEvent)\n"
                         "    def second(event):\n"
                         "        return stop('settle now')\n";
    fixture x = fixture_new_ext(0, source, 0);
    char err[256];
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    int visible_followups = 0, terminals = 0;
    tny_owned_event *ev = NULL;
    for (;;) {
        tny_engine_next next = tny_engine_next_event(x.engine, 50, &ev, err, sizeof err);
        if (next == TNY_ENGINE_NEXT_DRAINED) break;
        ASSERT_EQ(TNY_ENGINE_NEXT_EVENT, next);
        if (ev->ev.kind == TNY_EV_USER_MESSAGE) visible_followups++;
        if (ev->ev.kind == TNY_EV_TURN_END) terminals++;
        tny_owned_event_free(ev);
    }
    ASSERT_EQ(1, x.fake->sends);
    ASSERT_EQ(0, visible_followups);
    ASSERT_EQ(1, terminals);
    fixture_free(&x);
    PASS();
}

TEST runtime_prompt_transform_and_block_precede_send_and_persistence(void) {
    const char *transform_source = "from tny_ext import UserPromptSubmitEvent, transform_prompt\n"
                                   "def setup(api):\n"
                                   "    @api.on(UserPromptSubmitEvent)\n"
                                   "    def transform(event):\n"
                                   "        return transform_prompt('effective prompt')\n";
    fixture x = fixture_new_ext(0, transform_source, 0);
    char err[256];
    ASSERT_EQ(0, tny_engine_start(x.engine, "submitted prompt", NULL, err, sizeof err));
    ASSERT(drain_engine(x.engine, NULL) > 0);
    ASSERT_EQ(1, x.fake->sends);
    ASSERT_STR_EQ("effective prompt", x.fake->prompts[0]);
    ASSERT_STR_EQ("submitted prompt", session_title(x.session));
    char *stored = jwrite(x.session->doc);
    ASSERT(stored);
    ASSERT(strstr(stored, "submitted prompt"));
    ASSERT(strstr(stored, "effective prompt"));
    ASSERT(strstr(stored, "extension_audit"));
    free(stored);
    fixture_free(&x);

    const char *block_source = "from tny_ext import UserPromptSubmitEvent, block_prompt\n"
                               "def setup(api):\n"
                               "    @api.on(UserPromptSubmitEvent)\n"
                               "    def block(event):\n"
                               "        return block_prompt('policy')\n";
    x = fixture_new_ext(0, block_source, 0);
    ASSERT_EQ(0, tny_engine_start(x.engine, "must not persist", NULL, err, sizeof err));
    tny_stop_reason stop = TNY_STOP_ERROR;
    ASSERT(drain_engine(x.engine, &stop) > 0);
    ASSERT_EQ(TNY_STOP_DENIED, stop);
    ASSERT_EQ(0, x.fake->sends);
    ASSERT_FALSE(session_title(x.session));
    stored = jwrite(x.session->doc);
    ASSERT(stored);
    ASSERT(strstr(stored, "\"blocked\":true"));
    ASSERT_FALSE(strstr(stored, "must not persist"));
    free(stored);
    fixture_free(&x);
    PASS();
}

TEST runtime_lifecycle_order_and_session_rebind_are_stable(void) {
    const char *source =
        "import os\n"
        "def setup(api):\n"
        "    @api.on('*')\n"
        "    def record(event):\n"
        "        reason = getattr(event, 'reason', '')\n"
        "        with open(os.environ['TNY_TEST_LIFECYCLE_LOG'], 'a', encoding='utf-8') as f:\n"
        "            f.write(event.type + (':' + reason if reason else '') + '\\n')\n";
    char log_path[] = "/tmp/tny-runtime-lifecycle-XXXXXX";
    int fd = mkstemp(log_path);
    ASSERT(fd >= 0);
    close(fd);
    setenv("TNY_TEST_LIFECYCLE_LOG", log_path, 1);
    fixture x = fixture_new_ext(0, source, 0);
    char err[256];
    ASSERT_EQ(0, tny_engine_start(x.engine, "first", NULL, err, sizeof err));
    ASSERT(drain_engine(x.engine, NULL) > 0);

    tny_engine_preserve_session_on_free(x.engine);
    tny_engine_free(x.engine);
    x.engine = tny_engine_new(x.ctx, x.session, x.perm, NULL, NULL);
    ASSERT(x.engine);
    ASSERT_EQ(0, tny_engine_prepare(x.engine, fake_backend(0, &x.fake), TNY_ENGINE_PREPARE_FRESH,
                                    err, sizeof err));
    ASSERT_EQ(0, tny_engine_start(x.engine, "second", NULL, err, sizeof err));
    ASSERT(drain_engine(x.engine, NULL) > 0);
    tny_engine_end_session(x.engine, "exit");

    size_t n = 0;
    char *log = file_slurp(log_path, &n);
    ASSERT(log);
    ASSERT_EQ(1, count_text(log, "session_start:new"));
    ASSERT_EQ(1, count_text(log, "session_end:exit"));
    ASSERT_EQ(2, count_text(log, "user_prompt_submit"));
    ASSERT_EQ(2, count_text(log, "turn_start"));
    ASSERT_EQ(2, count_text(log, "message_start"));
    ASSERT_EQ(2, count_text(log, "message_update"));
    ASSERT_EQ(2, count_text(log, "message_end"));
    const char *session = strstr(log, "session_start:new");
    const char *prompt = strstr(log, "user_prompt_submit");
    const char *before = strstr(log, "before_agent_start");
    const char *agent = strstr(log, "agent_start");
    const char *turn = strstr(log, "turn_start");
    const char *message = strstr(log, "message_start");
    const char *delta = strstr(log, "text_delta");
    const char *update = strstr(log, "message_update");
    const char *message_end = strstr(log, "message_end");
    const char *turn_end = strstr(log, "turn_end");
    const char *agent_end = strstr(log, "agent_end");
    const char *settled = strstr(log, "agent_settled");
    ASSERT(session < prompt && prompt < before && before < agent && agent < turn);
    ASSERT(turn < message && message < delta && delta < update);
    ASSERT(update < message_end && message_end < turn_end && turn_end < agent_end);
    ASSERT(agent_end < settled);
    free(log);
    unlink(log_path);
    unsetenv("TNY_TEST_LIFECYCLE_LOG");
    fixture_free(&x);
    PASS();
}

TEST runtime_transformed_steer_requeues_without_replaying_hook(void) {
    const char *source = "from tny_ext import UserPromptSubmitEvent, transform_prompt\n"
                         "def setup(api):\n"
                         "    @api.on(UserPromptSubmitEvent)\n"
                         "    def transform(event):\n"
                         "        if event.prompt == 'steer original':\n"
                         "            return transform_prompt('steer effective')\n"
                         "        if event.prompt == 'steer effective':\n"
                         "            return transform_prompt('HOOK-REPLAYED')\n";
    fixture x = fixture_new_ext(4, source, 0);
    char err[256];
    ASSERT_EQ(0, tny_engine_start(x.engine, "initial", NULL, err, sizeof err));
    ASSERT_EQ(0, tny_engine_steer(x.engine, "steer original", err, sizeof err));
    ASSERT_EQ(1, x.fake->steer_count);
    ASSERT_STR_EQ("steer effective", x.fake->steers[0]);
    char *requeued = NULL;
    tny_owned_event *ev = NULL;
    while ((ev = tny_engine_pop_event(x.engine))) {
        if (ev->ev.kind == TNY_EV_STEER_REJECTED) requeued = xstrndup(ev->ev.text, ev->ev.text_len);
        tny_owned_event_free(ev);
    }
    ASSERT_STR_EQ("steer effective", requeued);
    ASSERT(drain_engine(x.engine, NULL) > 0);
    ASSERT_EQ(0, tny_engine_start(x.engine, requeued, NULL, err, sizeof err));
    free(requeued);
    ASSERT(drain_engine(x.engine, NULL) > 0);
    ASSERT_EQ(2, x.fake->sends);
    ASSERT_STR_EQ("steer effective", x.fake->prompts[1]);
    ASSERT_FALSE(strstr(x.fake->prompts[1], "HOOK-REPLAYED"));
    fixture_free(&x);
    PASS();
}

TEST runtime_compaction_selection_instructions_and_workspace_events(void) {
    const char *source =
        "import os\n"
        "WATCH = {'instructions_change', 'pre_compact', 'post_compact', "
        "'model_change', 'effort_change', 'workspace_change'}\n"
        "def setup(api):\n"
        "    @api.on('*')\n"
        "    def record(event):\n"
        "        if event.type in WATCH:\n"
        "            with open(os.environ['TNY_TEST_CHANGE_LOG'], 'a', encoding='utf-8') as f:\n"
        "                f.write(event.type + '\\n')\n";
    char log_path[] = "/tmp/tny-runtime-changes-XXXXXX";
    int fd = mkstemp(log_path);
    ASSERT(fd >= 0);
    close(fd);
    setenv("TNY_TEST_CHANGE_LOG", log_path, 1);
    fixture x = fixture_new_ext(0, source, 0);
    x.ctx->backend = TNY_BK_OPENAI;
    tny_extensions_set_provider(x.ctx->extensions, TNY_BK_OPENAI);
    char *agents = path_join(x.ctx->cwd, "AGENTS.md");
    ASSERT_EQ(0, file_write_atomic(agents, "test instructions\n", 18));
    ASSERT_EQ(0, instructions_refresh(x.ctx));
    free(agents);

    char err[256];
    ASSERT_EQ(0, tny_engine_start(x.engine, "first", NULL, err, sizeof err));
    ASSERT(drain_engine(x.engine, NULL) > 0);
    for (int i = 0; i < 8; i++) session_bump_turns(x.session);
    session_add_text(x.session, "user", "old request");
    session_add_text(x.session, "assistant", "old answer");
    session_add_text(x.session, "user", "latest request");
    ASSERT_EQ(1, tny_engine_compact(x.engine, true, "manual"));
    tny_engine_model_changed(x.engine, "old-model", "new-model", "test");
    tny_engine_effort_changed(x.engine, "low", "high", "test");
    char *extra = path_join(x.ctx->cwd, "extra");
    ASSERT_EQ(0, mkdir_p(extra));
    ASSERT_EQ(0, tny_workspace_add(x.ctx, extra));
    ASSERT_EQ(1, x.ctx->n_extra_dirs);
    ASSERT_STR_EQ(extra, x.ctx->extra_dirs[0]);
    tny_engine_workspace_changed(x.engine, "add", extra);
    free(extra);

    size_t n = 0;
    char *log = file_slurp(log_path, &n);
    ASSERT(log);
    ASSERT_EQ(1, count_text(log, "instructions_change"));
    ASSERT_EQ(1, count_text(log, "pre_compact"));
    ASSERT_EQ(1, count_text(log, "post_compact"));
    ASSERT_EQ(1, count_text(log, "model_change"));
    ASSERT_EQ(1, count_text(log, "effort_change"));
    ASSERT_EQ(1, count_text(log, "workspace_change"));
    ASSERT(strstr(log, "pre_compact") < strstr(log, "post_compact"));
    free(log);
    unlink(log_path);
    unsetenv("TNY_TEST_CHANGE_LOG");
    fixture_free(&x);
    PASS();
}

TEST runtime_permission_fold_is_correlated_suppressed_and_deny_sticky(void) {
    const char *allow_source = "from tny_ext import PermissionRequestEvent, decide_permission\n"
                               "def setup(api):\n"
                               "    @api.on(PermissionRequestEvent)\n"
                               "    def allow(event):\n"
                               "        return decide_permission('allow_once')\n";
    fixture x = fixture_new_ext(5, allow_source, 0);
    x.ctx->backend = TNY_BK_OPENAI;
    tny_extensions_set_provider(x.ctx->extensions, TNY_BK_OPENAI);
    char err[128];
    ASSERT_EQ(0, tny_engine_start(x.engine, "permission", NULL, err, sizeof err));
    tny_stop_reason stop = TNY_STOP_ERROR;
    int visible_permissions = -1;
    ASSERT(drain_engine_kind(x.engine, &stop, TNY_EV_PERMISSION, &visible_permissions) > 0);
    ASSERT_EQ(TNY_STOP_DONE, stop);
    ASSERT_EQ(0, visible_permissions);
    ASSERT_EQ(1, x.fake->permission_responses);
    ASSERT_EQ(TNY_PERM_DECISION_ALLOW, x.fake->permission_decision);
    fixture_free(&x);

    const char *deny_source = "from tny_ext import PermissionRequestEvent, decide_permission\n"
                              "def setup(api):\n"
                              "    @api.on(PermissionRequestEvent)\n"
                              "    def allow(event):\n"
                              "        return decide_permission('allow_once')\n"
                              "    @api.on(PermissionRequestEvent)\n"
                              "    def deny(event):\n"
                              "        return decide_permission('deny', 'policy')\n";
    x = fixture_new_ext(5, deny_source, 0);
    x.ctx->backend = TNY_BK_OPENAI;
    tny_extensions_set_provider(x.ctx->extensions, TNY_BK_OPENAI);
    ASSERT_EQ(0, tny_engine_start(x.engine, "permission", NULL, err, sizeof err));
    stop = TNY_STOP_ERROR;
    ASSERT(drain_engine_kind(x.engine, &stop, TNY_EV_PERMISSION, &visible_permissions) > 0);
    ASSERT_EQ(TNY_STOP_DENIED, stop);
    ASSERT_EQ(0, visible_permissions);
    ASSERT_EQ(1, x.fake->permission_responses);
    ASSERT_EQ(TNY_PERM_DECISION_DENY, x.fake->permission_decision);
    fixture_free(&x);

    const char *stop_source = "from tny_ext import PermissionRequestEvent, stop\n"
                              "def setup(api):\n"
                              "    @api.on(PermissionRequestEvent)\n"
                              "    def halt(event):\n"
                              "        return stop('cancel permission')\n";
    x = fixture_new_ext(5, stop_source, 0);
    x.ctx->backend = TNY_BK_OPENAI;
    tny_extensions_set_provider(x.ctx->extensions, TNY_BK_OPENAI);
    ASSERT_EQ(0, tny_engine_start(x.engine, "permission", NULL, err, sizeof err));
    stop = TNY_STOP_ERROR;
    ASSERT(drain_engine_kind(x.engine, &stop, TNY_EV_PERMISSION, &visible_permissions) > 0);
    ASSERT_EQ(TNY_STOP_INTERRUPTED, stop);
    ASSERT_EQ(0, visible_permissions);
    ASSERT_EQ(0, x.fake->permission_responses);
    ASSERT_EQ(1, x.fake->cancels);
    fixture_free(&x);
    PASS();
}

/* docs/adr/0089: the shared image gate sits at the top of tny_engine_start,
 * so every caller — TUI, one-shot CLI, detached runner, library and native
 * subagents — refuses a configured-false image turn before prompt, event or
 * session state changes. The queue entry point refuses with the same
 * configuration reason, ahead of its transport check. */
TEST runtime_refuses_image_turns_when_image_input_is_configured_off(void) {
    fixture x = fixture_new(0);
    char err[192];
    const char *images[] = {"unread.png", NULL};

    x.ctx->image_input = TNY_IMAGE_INPUT_CONFIGURED_UNSUPPORTED;
    int messages = session_message_count(x.session);
    uint64_t sequence = x.session->extension_agent_sequence;
    err[0] = '\0';
    ASSERT_EQ(-1, tny_engine_start(x.engine, "look at this", images, err, sizeof err));
    ASSERT_STR_EQ(TNY_IMAGE_INPUT_REFUSAL, err);
    ASSERT_EQ(0, x.fake->sends);
    ASSERT_EQ(messages, session_message_count(x.session));
    ASSERT_EQ(sequence, x.session->extension_agent_sequence);

    err[0] = '\0';
    ASSERT_EQ(-1, tny_engine_queue_image(x.engine, "unread.png", err, sizeof err));
    ASSERT_STR_EQ(TNY_IMAGE_INPUT_REFUSAL, err);

    /* a text turn on the same refused provider is untouched, and the refused
     * call left no event of its own behind (this turn's two events only) */
    ASSERT_EQ(0, tny_engine_start(x.engine, "no images here", NULL, err, sizeof err));
    ASSERT_EQ(2, drain_engine(x.engine, NULL));
    ASSERT_EQ(1, x.fake->sends);
    ASSERT_STR_EQ("no images here", x.fake->prompts[0]);

    /* unknown and configured-supported keep the existing explicit path; the
     * unsupported ACP transport still refuses on its own terms */
    x.ctx->image_input = TNY_IMAGE_INPUT_UNKNOWN;
    ASSERT_EQ(0, tny_engine_start(x.engine, "look again", images, err, sizeof err));
    ASSERT(drain_engine(x.engine, NULL) >= 0);
    ASSERT_EQ(2, x.fake->sends);
    x.ctx->image_input = TNY_IMAGE_INPUT_CONFIGURED_SUPPORTED;
    err[0] = '\0';
    ASSERT_EQ(-1, tny_engine_queue_image(x.engine, "unread.png", err, sizeof err));
    ASSERT_STR_EQ("image attach is unavailable on this backend", err);
    fixture_free(&x);
    PASS();
}

/* docs/adr/0096: the engine entry point for an explicit preview answers with a
 * status, never a bare failure. A non-native backend or an idle session is
 * unavailable_session, a provider configured off is unsupported, and neither
 * touches the queue. `tny ask --image` and manual attach are unchanged. */
TEST runtime_preview_needs_a_native_owning_turn(void) {
    fixture x = fixture_new(0);
    char err[192];
    const char *code = NULL;
    char hex[65];
    memset(hex, 'a', 64);
    hex[64] = '\0';

    /* idle: no turn at all */
    ASSERT_EQ(TNY_IMAGE_PREVIEW_UNAVAILABLE_SESSION,
              tny_engine_queue_image_preview(x.engine, "shot.png", hex, &code, err, sizeof err));
    ASSERT(code);
    ASSERT_STR_EQ(TNY_IMAGE_PREVIEW_CODE_NO_SESSION, code);
    ASSERT_EQ(0, x.fake->sends);

    /* an active turn on a backend with no native pending-image queue */
    ASSERT_EQ(0, tny_engine_start(x.engine, "hello", NULL, err, sizeof err));
    ASSERT_EQ(TNY_IMAGE_PREVIEW_UNAVAILABLE_SESSION,
              tny_engine_queue_image_preview(x.engine, "shot.png", hex, &code, err, sizeof err));
    ASSERT(code);
    ASSERT_STR_EQ(TNY_IMAGE_PREVIEW_CODE_NO_SESSION, code);

    /* the configuration refusal precedes the transport answer */
    x.ctx->image_input = TNY_IMAGE_INPUT_CONFIGURED_UNSUPPORTED;
    err[0] = '\0';
    ASSERT_EQ(TNY_IMAGE_PREVIEW_UNSUPPORTED,
              tny_engine_queue_image_preview(x.engine, "shot.png", hex, &code, err, sizeof err));
    ASSERT_STR_EQ(TNY_IMAGE_INPUT_REFUSAL, err);
    ASSERT(code);
    ASSERT_STR_EQ(TNY_IMAGE_PREVIEW_CODE_CAPABILITY, code);

    /* the manual queue keeps its own established answer */
    x.ctx->image_input = TNY_IMAGE_INPUT_CONFIGURED_SUPPORTED;
    err[0] = '\0';
    ASSERT_EQ(-1, tny_engine_queue_image(x.engine, "shot.png", err, sizeof err));
    ASSERT_STR_EQ("image attach is unavailable on this backend", err);
    ASSERT(drain_engine(x.engine, NULL) >= 0);
    fixture_free(&x);
    PASS();
}

/* ---- canonical JSONL lines and the checked stdout seam (ADR 0090) ---- */

#define CALLER_FL (O_NONBLOCK | O_APPEND)

static tny_owned_event jsonl_event(tny_event_kind kind) {
    tny_owned_event ev = {0};
    ev.ev.kind = kind;
    ev.sequence = 7;
    ev.timestamp_ms = 1234;
    ev.provider = (char *)"openai";
    ev.session_id = (char *)"abc123";
    ev.turn_id = (char *)"abc123:1:0";
    return ev;
}

TEST event_jsonl_writes_the_public_envelope_and_payload(void) {
    buf_t out;
    buf_init(&out);
    tny_owned_event ev = jsonl_event(TNY_EV_TEXT_DELTA);
    ev.ev.text = "he said \"hi\"\n\x01";
    ev.ev.text_len = strlen(ev.ev.text);
    tny_event_jsonl_append(&out, &ev);
    ASSERT_STR_EQ("{\"schema_version\":1,\"sequence\":7,\"timestamp_ms\":1234,"
                  "\"provider\":\"openai\",\"session_id\":\"abc123\","
                  "\"turn_id\":\"abc123:1:0\",\"type\":\"text_delta\",\"kind\":0,"
                  "\"text\":\"he said \\\"hi\\\"\\n\\u0001\",\"message_id\":\"\"}\n",
                  out.data);

    /* An empty ephemeral envelope value keeps its key. */
    buf_clear(&out);
    ev = jsonl_event(TNY_EV_TURN_END);
    ev.session_id = (char *)"";
    ev.turn_id = (char *)"";
    ev.ev.stop = TNY_STOP_INTERRUPTED;
    tny_event_jsonl_append(&out, &ev);
    ASSERT_STR_EQ("{\"schema_version\":1,\"sequence\":7,\"timestamp_ms\":1234,"
                  "\"provider\":\"openai\",\"session_id\":\"\",\"turn_id\":\"\","
                  "\"type\":\"turn_end\",\"kind\":7,\"stop_reason\":1}\n",
                  out.data);

    buf_clear(&out);
    ev = jsonl_event(TNY_EV_USAGE);
    ev.ev.in_tokens = 11;
    ev.ev.out_tokens = 2;
    ev.ev.context_used = 11;
    tny_event_jsonl_append(&out, &ev);
    ASSERT(strstr(out.data, "\"cost\":null,\"has_cost\":false") != NULL);
    ASSERT(strstr(out.data, "\"input_tokens\":11,\"output_tokens\":2") != NULL);

    buf_clear(&out);
    ev = jsonl_event(TNY_EV_TOOL_END);
    ev.ev.tool_name = "list_files";
    ev.ev.tool_id = "call_1";
    ev.ev.tool_ok = true;
    tny_event_jsonl_append(&out, &ev);
    ASSERT(strstr(out.data, "\"tool_name\":\"list_files\",\"tool_id\":\"call_1\","
                            "\"tool_detail\":\"\",\"tool_ok\":true}") != NULL);

    /* The public error code, not the private category number. */
    buf_clear(&out);
    ev = jsonl_event(TNY_EV_ERROR);
    ev.ev.text = "provider said no";
    ev.ev.text_len = strlen(ev.ev.text);
    ev.ev.error_code = TNY_EVENT_ERROR_AUTH;
    tny_event_jsonl_append(&out, &ev);
    ASSERT(strstr(out.data, "\"error_code\":-6}") != NULL);
    buf_free(&out);
    PASS();
}

TEST ask_exit_status_never_defaults_to_done(void) {
    /* A delivered terminal decides the ordinary outcomes. */
    ASSERT_EQ(0, cli_ask_exit_status(TNY_EVENT_WRITE_OK, true, TNY_STOP_DONE));
    ASSERT_EQ(130, cli_ask_exit_status(TNY_EVENT_WRITE_OK, true, TNY_STOP_INTERRUPTED));
    ASSERT_EQ(2, cli_ask_exit_status(TNY_EVENT_WRITE_OK, true, TNY_STOP_DENIED));
    ASSERT_EQ(2, cli_ask_exit_status(TNY_EVENT_WRITE_OK, true, TNY_STOP_STEP_LIMIT));
    ASSERT_EQ(2, cli_ask_exit_status(TNY_EVENT_WRITE_OK, true, TNY_STOP_ERROR));
    /* No terminal was observed: the zeroed stop reason must not read as DONE. */
    ASSERT_EQ(2, cli_ask_exit_status(TNY_EVENT_WRITE_OK, false, TNY_STOP_DONE));
    /* A stdout failure outranks a turn that really finished. */
    ASSERT_EQ(2, cli_ask_exit_status(TNY_EVENT_WRITE_IO, true, TNY_STOP_DONE));
    ASSERT_EQ(130, cli_ask_exit_status(TNY_EVENT_WRITE_CANCELLED, true, TNY_STOP_DONE));
    PASS();
}

static int jsonl_probe_calls = 0;
static int jsonl_probe_flips_at = -1;
static bool jsonl_probe(void *ud) {
    (void)ud;
    jsonl_probe_calls++;
    return jsonl_probe_flips_at >= 0 && jsonl_probe_calls > jsonl_probe_flips_at;
}

TEST event_jsonl_writer_checks_every_write(void) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    int before = fcntl(fds[1], F_GETFL);
    tny_event_writer w;
    tny_event_writer_init(&w, fds[1], NULL, NULL);
    tny_owned_event ev = jsonl_event(TNY_EV_TEXT_DELTA);
    ev.ev.text = "delivered";
    ev.ev.text_len = strlen(ev.ev.text);
    ASSERT_EQ(TNY_EVENT_WRITE_OK, tny_event_writer_emit(&w, &ev));
    char seen[512] = {0};
    ssize_t n = read(fds[0], seen, sizeof seen - 1);
    ASSERT(n > 0);
    ASSERT(strstr(seen, "\"text\":\"delivered\"") != NULL);
    /* The caller's open file description keeps the flags it came with. Only
     * the flags a caller can set are compared: Darwin reports a private
     * kernel bit in F_GETFL after the first write to a pipe. */
    ASSERT_EQ(before & CALLER_FL, fcntl(fds[1], F_GETFL) & CALLER_FL);

    /* A consumer that hung up is an I/O failure, never a silent success. */
    close(fds[0]);
    signal(SIGPIPE, SIG_IGN);
    tny_event_write_rc rc = TNY_EVENT_WRITE_OK;
    for (int i = 0; i < 64 && rc == TNY_EVENT_WRITE_OK; i++) rc = tny_event_writer_emit(&w, &ev);
    ASSERT_EQ(TNY_EVENT_WRITE_IO, rc);
    ASSERT_EQ(EPIPE, w.last_errno);
    tny_event_writer_free(&w);
    close(fds[1]);
    PASS();
}

TEST event_jsonl_writer_yields_to_cancellation_when_the_pipe_is_full(void) {
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    int before = fcntl(fds[1], F_GETFL);
    /* Fill the pipe so the next write cannot proceed. */
    if (fcntl(fds[1], F_SETFL, before | O_NONBLOCK) != 0) FAILm("cannot arm the pipe");
    char block[4096];
    memset(block, 'x', sizeof block);
    while (write(fds[1], block, sizeof block) > 0) {}
    ASSERT_EQ(0, fcntl(fds[1], F_SETFL, before));

    jsonl_probe_calls = 0;
    jsonl_probe_flips_at = 2; /* stall twice, then the user interrupts */
    tny_event_writer w;
    tny_event_writer_init(&w, fds[1], jsonl_probe, NULL);
    tny_owned_event ev = jsonl_event(TNY_EV_TEXT_DELTA);
    ev.ev.text = "blocked";
    ev.ev.text_len = strlen(ev.ev.text);
    int64_t started = now_ms();
    ASSERT_EQ(TNY_EVENT_WRITE_CANCELLED, tny_event_writer_emit(&w, &ev));
    ASSERT(now_ms() - started < 5000); /* prompt, not on the reader's schedule */
    ASSERT(jsonl_probe_calls > 1);     /* the stall keeps re-checking */
    ASSERT_EQ(before & CALLER_FL, fcntl(fds[1], F_GETFL) & CALLER_FL);
    tny_event_writer_free(&w);
    jsonl_probe_flips_at = -1;
    close(fds[0]);
    close(fds[1]);
    PASS();
}

static int64_t jsonl_probe_flips_after_ms = 0;
static bool jsonl_late_probe(void *ud) {
    (void)ud;
    return now_ms() >= jsonl_probe_flips_after_ms;
}

/* Fill the pipe before the writer ever sees it, then interrupt long after
 * any grace period a writer might keep: the seam reports "not writable" for
 * the whole time, which is backpressure — never a licence to fall back to a
 * blocking write(2) that no signal can reach (signal() restarts it). The emit
 * runs in a child, so a writer that ignores the interrupt is a failed
 * assertion here instead of a hung suite; the child reports through a pipe
 * and is killed rather than exiting, because a leak-checked run instruments
 * every exit it can see. */
TEST event_jsonl_writer_yields_to_a_late_interrupt_on_an_initially_full_pipe(void) {
    int fds[2], answer[2];
    ASSERT_EQ(0, pipe(fds));
    ASSERT_EQ(0, pipe(answer));
    int before = fcntl(fds[1], F_GETFL);
    if (fcntl(fds[1], F_SETFL, before | O_NONBLOCK) != 0) FAILm("cannot arm the pipe");
    char block[4096];
    memset(block, 'x', sizeof block);
    while (write(fds[1], block, sizeof block) > 0) {}
    ASSERT_EQ(0, fcntl(fds[1], F_SETFL, before));

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        close(fds[0]); /* nobody drains this pipe, in this process or any other */
        close(answer[0]);
        jsonl_probe_flips_after_ms = now_ms() + 1200;
        tny_event_writer w;
        tny_event_writer_init(&w, fds[1], jsonl_late_probe, NULL);
        tny_owned_event ev = jsonl_event(TNY_EV_TEXT_DELTA);
        ev.ev.text = "blocked";
        ev.ev.text_len = strlen(ev.ev.text);
        char rc = tny_event_writer_emit(&w, &ev) == TNY_EVENT_WRITE_CANCELLED ? 'c' : 'x';
        tny_event_writer_free(&w);
        while (write(answer[1], &rc, 1) < 0 && errno == EINTR) {}
        for (;;) pause(); /* the parent ends this child */
    }
    close(answer[1]);
    struct pollfd waiting = {answer[0], POLLIN, 0};
    char rc = 0;
    if (poll(&waiting, 1, 10000) == 1) {
        while (read(answer[0], &rc, 1) < 0 && errno == EINTR) {}
    }
    kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    close(answer[0]);
    ASSERT_EQ(before & CALLER_FL, fcntl(fds[1], F_GETFL) & CALLER_FL);
    close(fds[0]);
    close(fds[1]);
    if (rc == 'x') FAILm("the stalled writer returned something other than cancelled");
    if (rc != 'c') FAILm("the interrupt never reached a writer stalled on an already-full pipe");
    PASS();
}

SUITE(runtime_suite) {
    RUN_TEST(runtime_copies_events_and_suppresses_duplicate_terminal);
    RUN_TEST(runtime_system_prompt_prefixes_only_the_first_user_message);
    RUN_TEST(runtime_task_precedes_explicit_system_prompt_on_host_first_turn);
    RUN_TEST(runtime_skill_mention_rides_ahead_of_the_user_text);
    RUN_TEST(runtime_system_prompt_skips_resumed_host_sessions);
    RUN_TEST(runtime_synthesizes_transport_error_and_terminal);
    RUN_TEST(runtime_overflow_keeps_error_and_single_terminal);
    RUN_TEST(runtime_cancel_emits_one_interrupted_terminal);
    RUN_TEST(runtime_next_event_waits_without_spinning);
    RUN_TEST(runtime_backend_deadline_caps_long_caller_wait);
    RUN_TEST(runtime_backend_deadline_preserves_caller_timeout);
    RUN_TEST(runtime_backend_due_deadline_dispatches_without_caller_delay);
    RUN_TEST(runtime_oom_uses_reserved_error_and_terminal_once);
    RUN_TEST(runtime_extension_continues_visibly_then_settles);
    RUN_TEST(runtime_extension_positive_continuation_cap_settles);
    RUN_TEST(runtime_extension_failure_is_visible_and_fail_open);
    RUN_TEST(runtime_extension_stop_requests_cancel_at_safe_boundary);
    RUN_TEST(runtime_extension_host_state_persists_across_sessions);
    RUN_TEST(runtime_extension_stop_suppresses_earlier_continuation);
    RUN_TEST(runtime_prompt_transform_and_block_precede_send_and_persistence);
    RUN_TEST(runtime_lifecycle_order_and_session_rebind_are_stable);
    RUN_TEST(runtime_transformed_steer_requeues_without_replaying_hook);
    RUN_TEST(runtime_compaction_selection_instructions_and_workspace_events);
    RUN_TEST(runtime_permission_fold_is_correlated_suppressed_and_deny_sticky);
    RUN_TEST(runtime_refuses_image_turns_when_image_input_is_configured_off);
    RUN_TEST(runtime_preview_needs_a_native_owning_turn);
    RUN_TEST(event_jsonl_writes_the_public_envelope_and_payload);
    RUN_TEST(ask_exit_status_never_defaults_to_done);
    RUN_TEST(event_jsonl_writer_checks_every_write);
    RUN_TEST(event_jsonl_writer_yields_to_cancellation_when_the_pipe_is_full);
    RUN_TEST(event_jsonl_writer_yields_to_a_late_interrupt_on_an_initially_full_pipe);
}
