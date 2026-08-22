/* test_codex.c — unit tests for the codex backend's request/steer accounting
 * (docs/adr/0012): rejected steers must hand their text back, a turn ending
 * with an unanswered steer resolves it before TURN_END, and responses to
 * requests tny no longer tracks can never fail the turn that runs now. */
#include "greatest.h"
#include "backends/codex/codex.h"

#include <stdlib.h>
#include <string.h>

/* ---- event recorder ---- */

#define REC_MAX 16
typedef struct {
    tny_event_kind kind[REC_MAX];
    char *text[REC_MAX];
    int n;
} rec_t;

static void rec_cb(const tny_event *ev, void *ud) {
    rec_t *r = ud;
    if (r->n == REC_MAX) return;
    r->kind[r->n] = ev->kind;
    r->text[r->n] = ev->text ? xstrndup(ev->text, ev->text_len) : NULL;
    r->n++;
}

static void rec_free(rec_t *r) {
    for (int i = 0; i < r->n; i++) free(r->text[i]);
    memset(r, 0, sizeof *r);
}

static void cx_free_state(cx_impl *o) {
    for (int i = 0; i < CX_MAX_PENDING; i++) cx_pending_clear(&o->pending[i]);
    for (int i = 0; i < o->n_out; i++) free(o->outq[i]);
    free(o->outq);
    memset(o, 0, sizeof *o);
}

/* ---- cx_request slot exhaustion (docs/adr/0012) ---- */

TEST tracked_request_with_no_free_slot_refuses_and_sends_nothing(void) {
    cx_impl o;
    memset(&o, 0, sizeof o);
    for (int i = 0; i < CX_MAX_PENDING; i++)
        ASSERT(cx_request(&o, "turn/steer", "{}", CXR_STEER) >= 0);
    int before = o.n_out;
    ASSERT_EQ(-1, cx_request(&o, "turn/steer", "{}", CXR_STEER));
    ASSERT_EQ(before, o.n_out); /* the unmatchable frame was never queued */
    /* untracked requests still go out */
    ASSERT(cx_request(&o, "turn/interrupt", "{}", CXR_FREE) >= 0);
    ASSERT_EQ(before + 1, o.n_out);
    cx_free_state(&o);
    PASS();
}

/* ---- turn-end sweep for unanswered steers (docs/adr/0012) ---- */

TEST end_turn_resolves_unanswered_steer_before_turn_end(void) {
    cx_impl o;
    memset(&o, 0, sizeof o);
    rec_t r;
    memset(&r, 0, sizeof r);
    o.cb = rec_cb;
    o.ud = &r;
    o.turn_active = true;
    int id = cx_request(&o, "turn/steer", "{}", CXR_STEER);
    ASSERT(id >= 0);
    cx_pending *p = cx_pending_find(&o, id);
    ASSERT(p != NULL);
    p->steer_text = xstrdup("hold this");

    cx_end_turn(&o, TNY_STOP_DONE);

    ASSERT_EQ(2, r.n);
    ASSERT_EQ(TNY_EV_STEER_REJECTED, r.kind[0]); /* strictly before TURN_END */
    ASSERT_STR_EQ("hold this", r.text[0]);
    ASSERT_EQ(TNY_EV_TURN_END, r.kind[1]);
    ASSERT(cx_pending_find(&o, id) == NULL); /* resolved, not leaked */
    rec_free(&r);
    cx_free_state(&o);
    PASS();
}

TEST end_turn_without_pending_steer_emits_only_turn_end(void) {
    cx_impl o;
    memset(&o, 0, sizeof o);
    rec_t r;
    memset(&r, 0, sizeof r);
    o.cb = rec_cb;
    o.ud = &r;
    o.turn_active = true;
    int id = cx_request(&o, "turn/start", "{}", CXR_TURN);
    ASSERT(id >= 0);

    cx_end_turn(&o, TNY_STOP_DONE);

    ASSERT_EQ(1, r.n);
    ASSERT_EQ(TNY_EV_TURN_END, r.kind[0]);
    ASSERT(cx_pending_find(&o, id) != NULL); /* non-steer pendings survive */
    rec_free(&r);
    cx_free_state(&o);
    PASS();
}

/* ---- response routing (codex_msg.c) ---- */

static void feed(cx_impl *o, const char *json) {
    cx_on_ws_msg(json, strlen(json), o);
}

TEST steer_error_response_rejects_with_the_pending_text(void) {
    cx_impl o;
    memset(&o, 0, sizeof o);
    rec_t r;
    memset(&r, 0, sizeof r);
    o.cb = rec_cb;
    o.ud = &r;
    o.wait_id = -1;
    o.turn_active = true;
    int id = cx_request(&o, "turn/steer", "{}", CXR_STEER);
    cx_pending *p = cx_pending_find(&o, id);
    ASSERT(p != NULL);
    p->steer_text = xstrdup("second thought");

    char msg[128];
    snprintf(msg, sizeof msg,
             "{\"id\":%d,\"error\":{\"code\":-32600,\"message\":\"not steerable\"}}", id);
    feed(&o, msg);

    ASSERT(o.turn_active); /* a steer rejection never ends the turn */
    bool saw = false;
    for (int i = 0; i < r.n; i++) {
        if (r.kind[i] == TNY_EV_TURN_END) FAILm("steer rejection ended the turn");
        if (r.kind[i] == TNY_EV_STEER_REJECTED) {
            saw = true;
            ASSERT_STR_EQ("second thought", r.text[i]);
        }
    }
    ASSERT(saw);
    ASSERT(cx_pending_find(&o, id) == NULL);
    rec_free(&r);
    cx_free_state(&o);
    PASS();
}

TEST untracked_error_response_does_not_fail_the_running_turn(void) {
    cx_impl o;
    memset(&o, 0, sizeof o);
    rec_t r;
    memset(&r, 0, sizeof r);
    o.cb = rec_cb;
    o.ud = &r;
    o.wait_id = -1;
    o.turn_active = true; /* turn 2 is live when the stale response lands */

    feed(&o, "{\"id\":991,\"error\":{\"code\":-32600,\"message\":\"stale\"}}");

    ASSERT(o.turn_active);
    for (int i = 0; i < r.n; i++) {
        if (r.kind[i] == TNY_EV_TURN_END) FAILm("stale response ended the turn");
        if (r.kind[i] == TNY_EV_ERROR) FAILm("stale response raised an error");
    }
    rec_free(&r);
    cx_free_state(&o);
    PASS();
}

SUITE(codex_suite) {
    RUN_TEST(tracked_request_with_no_free_slot_refuses_and_sends_nothing);
    RUN_TEST(end_turn_resolves_unanswered_steer_before_turn_end);
    RUN_TEST(end_turn_without_pending_steer_emits_only_turn_end);
    RUN_TEST(steer_error_response_rejects_with_the_pending_text);
    RUN_TEST(untracked_error_response_does_not_fail_the_running_turn);
}
