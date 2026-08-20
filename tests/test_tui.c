/* test_tui.c — unit tests for the TUI's pure-logic seams: the ANSI-aware
 * row writer, the overlay row budget, overlay lifecycle, and the pre-warm
 * handoff (docs/adr/0002) driven by a stub backend. */
#include "greatest.h"
#include "tui/tui.h"
#include "core/config.h"

#include <poll.h>
#include <stdlib.h>
#include <string.h>

/* ---- tui_push_ansi ---- */

static char *pushed(const char *s, int maxw, int *w) {
    buf_t b;
    buf_init(&b);
    int cols = tui_push_ansi(&b, s, strlen(s), maxw);
    if (w) *w = cols;
    return b.data ? b.data : xstrdup("");
}

TEST push_ansi_plain_truncates(void) {
    int w = 0;
    char *out = pushed("hello world", 5, &w);
    ASSERT_STR_EQ("hello", out);
    ASSERT_EQ(5, w);
    free(out);
    PASS();
}

TEST push_ansi_sgr_is_zero_width(void) {
    int w = 0;
    char *out = pushed("\x1b[1mab\x1b[0m", 80, &w);
    ASSERT_STR_EQ("\x1b[1mab\x1b[0m", out);
    ASSERT_EQ(2, w);
    free(out);
    PASS();
}

TEST push_ansi_reset_survives_the_cut(void) {
    /* the reset lands after the width limit; dropping it would leak bold
     * into every following row */
    int w = 0;
    char *out = pushed("\x1b[1mabcdef\x1b[0m", 3, &w);
    ASSERT_STR_EQ("\x1b[1mabc\x1b[0m", out);
    ASSERT_EQ(3, w);
    free(out);
    PASS();
}

TEST push_ansi_drops_controls_keeps_utf8(void) {
    int w = 0;
    char *out = pushed("a\tb\ncé", 80, &w); /* tab->space, newline dropped */
    ASSERT_STR_EQ("a bcé", out);
    ASSERT_EQ(5, w); /* é is one column */
    free(out);
    PASS();
}

TEST push_ansi_non_sgr_escape_dropped(void) {
    int w = 0;
    char *out = pushed("x\x1b[2Ay", 80, &w); /* cursor-up must not leak */
    ASSERT_STR_EQ("xy", out);
    ASSERT_EQ(2, w);
    free(out);
    PASS();
}

/* ---- overlay budget ---- */

static void mk_tui(tui *t, int rows) {
    memset(t, 0, sizeof *t);
    buf_init(&t->out);
    buf_init(&t->partial);
    buf_init(&t->input);
    buf_init(&t->overlay);
    buf_init(&t->note);
    t->rows = rows;
    t->cols = 100;
    t->tty = true;
}

static void free_tui(tui *t) {
    buf_free(&t->out);
    buf_free(&t->partial);
    buf_free(&t->input);
    buf_free(&t->overlay);
    buf_free(&t->note);
    tui_items_clear(t);
}

TEST overlay_budget_accounts_for_the_block(void) {
    tui t;
    mk_tui(&t, 24);
    /* status 1 + slack 1 + composer 1 -> 21 rows for the overlay */
    ASSERT_EQ(21, tui_overlay_budget(&t));

    buf_appends(&t.partial, "streaming…");
    ASSERT_EQ(20, tui_overlay_budget(&t));
    buf_clear(&t.partial);

    buf_appends(&t.input, "one\ntwo\nthree");
    ASSERT_EQ(19, tui_overlay_budget(&t)); /* composer now 3 rows */
    buf_clear(&t.input);

    t.pick = PICK_CMD;
    for (int i = 0; i < 20; i++) tui_items_add(&t, "x", NULL);
    ASSERT_EQ(21 - TUI_POP_ROWS, tui_overlay_budget(&t)); /* popover clamps at 8 */

    t.approval = true; /* approval owns the composer: exactly one row */
    ASSERT_EQ(21 - TUI_POP_ROWS, tui_overlay_budget(&t));

    t.rows = 4; /* pathologically small: never negative */
    ASSERT_EQ(0, tui_overlay_budget(&t));
    free_tui(&t);
    PASS();
}

TEST overlay_linef_and_clear(void) {
    tui t;
    mk_tui(&t, 24);
    tui_overlay_linef(&t, "line %d", 1);
    tui_overlay_linef(&t, "line %d", 2);
    ASSERT(t.dirty);
    ASSERT_STR_EQ("line 1\nline 2\n", t.overlay.data);
    ASSERT_EQ(0, (int)t.out.len); /* never committed to the transcript */

    t.dirty = false;
    tui_overlay_clear(&t);
    ASSERT_EQ(0, (int)t.overlay.len);
    ASSERT(t.dirty);

    t.dirty = false;
    tui_overlay_clear(&t); /* idempotent: no spurious repaint */
    ASSERT_FALSE(t.dirty);
    free_tui(&t);
    PASS();
}

TEST overlay_linef_falls_back_without_a_tty(void) {
    tui t;
    mk_tui(&t, 24);
    t.tty = false;
    tui_overlay_linef(&t, "help text");
    ASSERT_EQ(0, (int)t.overlay.len);
    ASSERT(t.out.len > 0); /* went to the scrolling transcript instead */
    ASSERT(strstr(t.out.data, "help text"));
    free_tui(&t);
    PASS();
}

/* ---- pre-warm handoff ---- */

typedef struct {
    volatile int connects, disconnects, destroys;
    int connect_rc;
    int delay_ms;
} stub_state;

static int stub_connect(tny_backend *b, char *err, size_t errlen) {
    stub_state *s = b->impl;
    if (s->delay_ms) poll(NULL, 0, s->delay_ms);
    s->connects++;
    if (s->connect_rc != 0) snprintf(err, errlen, "stub connect failed");
    return s->connect_rc;
}

static void stub_disconnect(tny_backend *b) {
    stub_state *s = b->impl;
    s->disconnects++;
}

static void stub_destroy(tny_backend *b) {
    stub_state *s = b->impl;
    s->destroys++;
    free(b);
}

static tny_backend *stub_backend(stub_state *s) {
    tny_backend *b = calloc(1, sizeof *b);
    b->id = TNY_BK_CODEX;
    b->impl = s;
    b->connect = stub_connect;
    b->disconnect = stub_disconnect;
    b->destroy = stub_destroy;
    return b;
}

static void wait_for(volatile int *flag, int ms) {
    for (int i = 0; i < ms / 10 && !*flag; i++) poll(NULL, 0, 10);
}

TEST prewarm_take_returns_connected_backend(void) {
    tui t;
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&t, 0, sizeof t);
    t.ctx = &ctx;
    ctx.backend = TNY_BK_CODEX;

    stub_state s = {0};
    s.delay_ms = 30;
    ASSERT_EQ(0, tui_prewarm_launch(&t, stub_backend(&s), TNY_BK_CODEX));
    ASSERT(t.prewarm != NULL);

    tny_backend *bk = tui_prewarm_take(&t); /* blocks until connect() lands */
    ASSERT(bk != NULL);
    ASSERT(t.prewarm == NULL);
    ASSERT_EQ(1, s.connects);
    ASSERT_EQ(0, s.destroys);
    bk->disconnect(bk);
    bk->destroy(bk);
    ASSERT_EQ(1, s.destroys);
    PASS();
}

TEST prewarm_failed_connect_is_silent_and_discarded(void) {
    tui t;
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&t, 0, sizeof t);
    t.ctx = &ctx;
    ctx.backend = TNY_BK_CODEX;

    stub_state s = {0};
    s.connect_rc = -1;
    ASSERT_EQ(0, tui_prewarm_launch(&t, stub_backend(&s), TNY_BK_CODEX));
    ASSERT(tui_prewarm_take(&t) == NULL); /* caller falls back to lazy path */
    ASSERT_EQ(1, s.connects);
    ASSERT_EQ(1, s.destroys);
    PASS();
}

TEST prewarm_drop_mid_connect_cleans_up_on_the_thread(void) {
    tui t;
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&t, 0, sizeof t);
    t.ctx = &ctx;
    ctx.backend = TNY_BK_CODEX;

    stub_state s = {0};
    s.delay_ms = 60;
    ASSERT_EQ(0, tui_prewarm_launch(&t, stub_backend(&s), TNY_BK_CODEX));
    tui_prewarm_drop(&t); /* abandon while connect() is still sleeping */
    ASSERT(t.prewarm == NULL);
    wait_for(&s.destroys, 2000);
    ASSERT_EQ(1, s.destroys);
    ASSERT_EQ(1, s.disconnects);
    PASS();
}

TEST prewarm_take_rejects_a_switched_provider(void) {
    tui t;
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&t, 0, sizeof t);
    t.ctx = &ctx;
    ctx.backend = TNY_BK_CODEX;

    stub_state s = {0};
    ASSERT_EQ(0, tui_prewarm_launch(&t, stub_backend(&s), TNY_BK_CODEX));
    ctx.backend = TNY_BK_OPENAI; /* /provider switch happened meanwhile */
    ASSERT(tui_prewarm_take(&t) == NULL);
    ASSERT(t.prewarm == NULL);
    wait_for(&s.destroys, 2000);
    ASSERT_EQ(1, s.destroys);
    PASS();
}

TEST prewarm_start_keeps_a_matching_warmup(void) {
    /* /model, /fast and /resume drop the backend and re-kick the pre-warm;
     * a pending warm-up for the SAME provider must be kept, not respawned.
     * (cursor without a key: if the guard misfires and drops, no new warm-up
     * can start, so t->prewarm going NULL exposes the bug.) */
    tui t;
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&t, 0, sizeof t);
    t.ctx = &ctx;
    ctx.backend = TNY_BK_CURSOR;
    unsetenv("CURSOR_API_KEY");

    stub_state s = {0};
    ASSERT_EQ(0, tui_prewarm_launch(&t, stub_backend(&s), TNY_BK_CURSOR));
    tui_prewarm *kept = t.prewarm;
    tui_prewarm_start(&t);
    ASSERT_EQ(kept, t.prewarm); /* same pending warm-up, untouched */
    ASSERT_EQ(0, s.destroys);
    tui_prewarm_drop(&t);
    wait_for(&s.destroys, 2000);
    ASSERT_EQ(1, s.destroys);
    PASS();
}

TEST prewarm_applicability(void) {
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ASSERT_FALSE(tui_prewarm_applicable(&ctx, TNY_BK_OPENAI));
    ASSERT(tui_prewarm_applicable(&ctx, TNY_BK_CODEX));
    ASSERT_FALSE(tui_prewarm_applicable(&ctx, TNY_BK_ACP)); /* no --agent argv */
    char *argv[] = {"fake-agent", NULL};
    ctx.agent_argv = argv;
    ASSERT(tui_prewarm_applicable(&ctx, TNY_BK_ACP));
    ctx.agent_argv = NULL;

    unsetenv("CURSOR_API_KEY");
    ASSERT_FALSE(tui_prewarm_applicable(&ctx, TNY_BK_CURSOR));
    setenv("CURSOR_API_KEY", "key_test", 1);
    ASSERT(tui_prewarm_applicable(&ctx, TNY_BK_CURSOR));
    unsetenv("CURSOR_API_KEY");
    PASS();
}

SUITE(tui_suite) {
    RUN_TEST(push_ansi_plain_truncates);
    RUN_TEST(push_ansi_sgr_is_zero_width);
    RUN_TEST(push_ansi_reset_survives_the_cut);
    RUN_TEST(push_ansi_drops_controls_keeps_utf8);
    RUN_TEST(push_ansi_non_sgr_escape_dropped);
    RUN_TEST(overlay_budget_accounts_for_the_block);
    RUN_TEST(overlay_linef_and_clear);
    RUN_TEST(overlay_linef_falls_back_without_a_tty);
    RUN_TEST(prewarm_take_returns_connected_backend);
    RUN_TEST(prewarm_failed_connect_is_silent_and_discarded);
    RUN_TEST(prewarm_drop_mid_connect_cleans_up_on_the_thread);
    RUN_TEST(prewarm_take_rejects_a_switched_provider);
    RUN_TEST(prewarm_start_keeps_a_matching_warmup);
    RUN_TEST(prewarm_applicability);
}
