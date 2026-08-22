/* test_tui.c — unit tests for the TUI's pure-logic seams: the ANSI-aware
 * row writer, the overlay row budget, overlay lifecycle, and the pre-warm
 * handoff (docs/adr/0002) driven by a stub backend. */
#include "greatest.h"
#include "tui/tui.h"
#include "core/config.h"

#include <poll.h>
#include <pthread.h>
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

/* ---- transcript writes ---- */

TEST write_strips_nul_bytes(void) {
    tui t;
    mk_tui(&t, 24);
    tui_write(&t, "ab\0cd\nef", 8); /* embedded NUL must never reach stdout */
    ASSERT_EQ(5, (int)t.out.len);
    ASSERT(memcmp(t.out.data, "abcd\n", 5) == 0);
    ASSERT_STR_EQ("ef", t.partial.data);
    free_tui(&t);
    PASS();
}

/* ---- dim streaming (reasoning traces, docs/adr/0012) ---- */

TEST write_dim_reopens_after_newline(void) {
    /* A reasoning delta crossing a newline: the flushed line and the tail
     * left in `partial` must each be SGR-self-contained. Before the fix the
     * opening \x1b[2m stayed on the flushed line and the tail repainted in
     * the default color — the "first letters are white" bug. */
    tui t;
    mk_tui(&t, 24);
    t.color = true;
    tui_write_dim(&t, "end of line\nSta", 15);
    ASSERT_STR_EQ("\x1b[2mend of line\x1b[0m\n", t.out.data);
    ASSERT_STR_EQ("\x1b[2mSta\x1b[0m", t.partial.data);

    /* the rest of the line streams in a later delta and stays dim */
    tui_write_dim(&t, "rt", 2);
    ASSERT_STR_EQ("\x1b[2mSta\x1b[0m\x1b[2mrt\x1b[0m", t.partial.data);
    free_tui(&t);
    PASS();
}

TEST write_dim_blank_lines_carry_no_sgr(void) {
    tui t;
    mk_tui(&t, 24);
    t.color = true;
    tui_write_dim(&t, "a\n\nb", 4); /* empty segment: no \x1b[2m\x1b[0m noise */
    ASSERT_STR_EQ("\x1b[2ma\x1b[0m\n\n", t.out.data);
    ASSERT_STR_EQ("\x1b[2mb\x1b[0m", t.partial.data);
    free_tui(&t);
    PASS();
}

TEST write_dim_without_color_is_plain(void) {
    tui t;
    mk_tui(&t, 24);
    t.color = false;
    tui_write_dim(&t, "abc\nde", 6);
    ASSERT_STR_EQ("abc\n", t.out.data);
    ASSERT_STR_EQ("de", t.partial.data);
    free_tui(&t);
    PASS();
}

/* ---- pre-warm handoff ---- */

typedef struct {
    volatile int connects, disconnects, destroys, resumes;
    int connect_rc, resume_rc;
    int delay_ms;
    char resume_ptr[128];     /* "(null)" when called with NULL */
    pthread_t resume_thread;
} stub_state;

static int stub_connect(tny_backend *b, char *err, size_t errlen) {
    stub_state *s = b->impl;
    if (s->delay_ms) poll(NULL, 0, s->delay_ms);
    s->connects++;
    if (s->connect_rc != 0) snprintf(err, errlen, "stub connect failed");
    return s->connect_rc;
}

static int stub_resume(tny_backend *b, const char *ptr, char *err, size_t errlen) {
    stub_state *s = b->impl;
    s->resume_thread = pthread_self();
    snprintf(s->resume_ptr, sizeof s->resume_ptr, "%s", ptr ? ptr : "(null)");
    s->resumes++;
    if (s->resume_rc != 0) snprintf(err, errlen, "stub resume failed");
    return s->resume_rc;
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
    ASSERT_EQ(0, tui_prewarm_launch(&t, stub_backend(&s), TNY_BK_CODEX, NULL));
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
    ASSERT_EQ(0, tui_prewarm_launch(&t, stub_backend(&s), TNY_BK_CODEX, NULL));
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
    ASSERT_EQ(0, tui_prewarm_launch(&t, stub_backend(&s), TNY_BK_CODEX, NULL));
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
    ASSERT_EQ(0, tui_prewarm_launch(&t, stub_backend(&s), TNY_BK_CODEX, NULL));
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
    ASSERT_EQ(0, tui_prewarm_launch(&t, stub_backend(&s), TNY_BK_CURSOR, NULL));
    tui_prewarm *kept = t.prewarm;
    tui_prewarm_start(&t);
    ASSERT_EQ(kept, t.prewarm); /* same pending warm-up, untouched */
    ASSERT_EQ(0, s.destroys);
    tui_prewarm_drop(&t);
    wait_for(&s.destroys, 2000);
    ASSERT_EQ(1, s.destroys);
    PASS();
}

TEST prewarm_runs_create_or_resume_on_the_thread(void) {
    tui t;
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&t, 0, sizeof t);
    t.ctx = &ctx;
    ctx.backend = TNY_BK_CODEX;

    stub_state s = {0};
    s.delay_ms = 20;
    tny_backend *b = stub_backend(&s);
    b->create_or_resume = stub_resume;
    ASSERT_EQ(0, tui_prewarm_launch(&t, b, TNY_BK_CODEX, "thread-42"));

    tny_backend *bk = tui_prewarm_take(&t); /* blocks until the warm-up lands */
    ASSERT(bk != NULL);
    ASSERT_EQ(1, s.connects);
    ASSERT_EQ(1, s.resumes);
    ASSERT_STR_EQ("thread-42", s.resume_ptr); /* the frozen pointer arrived */
    ASSERT_FALSE(pthread_equal(pthread_self(), s.resume_thread));
    bk->disconnect(bk);
    bk->destroy(bk);
    ASSERT_EQ(1, s.destroys);
    PASS();
}

TEST prewarm_failed_resume_is_silent_and_discarded(void) {
    tui t;
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&t, 0, sizeof t);
    t.ctx = &ctx;
    ctx.backend = TNY_BK_CODEX;

    stub_state s = {0};
    s.resume_rc = -1;
    tny_backend *b = stub_backend(&s);
    b->create_or_resume = stub_resume;
    ASSERT_EQ(0, tui_prewarm_launch(&t, b, TNY_BK_CODEX, "thread-42"));
    ASSERT(tui_prewarm_take(&t) == NULL); /* caller falls back to lazy path */
    ASSERT_EQ(1, s.connects);
    ASSERT_EQ(1, s.resumes);
    ASSERT_EQ(1, s.disconnects);
    ASSERT_EQ(1, s.destroys);
    PASS();
}

TEST prewarm_start_restarts_on_a_stale_resume_pointer(void) {
    /* /new after a resumed session (or a session switch) leaves the pending
     * warm-up holding the wrong pointer: it must be dropped and re-kicked,
     * never adopted stale. codex with an unspawnable binary keeps the
     * restarted warm-up real but guarantees its connect() fails fast. */
    tui t;
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&t, 0, sizeof t);
    t.ctx = &ctx;
    ctx.backend = TNY_BK_CODEX;
    ctx.codex_bin = "/nonexistent/codex";

    stub_state s = {0};
    ASSERT_EQ(0, tui_prewarm_launch(&t, stub_backend(&s), TNY_BK_CODEX, "thread-old"));
    tui_prewarm *stale = t.prewarm;
    tui_prewarm_start(&t); /* no session: the pending pointer is now stale */
    ASSERT(t.prewarm != NULL);
    ASSERT(t.prewarm != stale); /* the stub was dropped, a fresh warm-up runs */
    wait_for(&s.destroys, 2000);
    ASSERT_EQ(1, s.destroys);
    ASSERT(tui_prewarm_take(&t) == NULL); /* fresh warm-up fails to connect */
    ASSERT(t.prewarm == NULL);
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

/* /fast is capability-gated (TNY_CAP_FAST), not codex-only: capable
 * providers toggle ctx->service_tier, incapable ones must leave it alone. */
TEST fast_command_is_capability_gated(void) {
    tui t;
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    memset(&t, 0, sizeof t);
    t.ctx = &ctx;
    unsetenv("CURSOR_API_KEY"); /* keep the cursor warm-up out of the test */

    ctx.backend = TNY_BK_ACP; /* no fast tier: the command must refuse */
    tui_command(&t, "/fast");
    ASSERT_EQ(NULL, ctx.service_tier);
    tui_command(&t, "/fast priority");
    ASSERT_EQ(NULL, ctx.service_tier);

    ctx.backend = TNY_BK_OPENAI; /* capable: bare /fast toggles */
    tui_command(&t, "/fast");
    ASSERT(tny_tier_is_fast(ctx.service_tier));
    tui_command(&t, "/fast");
    ASSERT_STR_EQ("default", ctx.service_tier);

    tui_command(&t, "/fast priority"); /* explicit spellings */
    ASSERT(tny_tier_is_fast(ctx.service_tier));
    tui_command(&t, "/fast off");
    ASSERT_STR_EQ("default", ctx.service_tier);
    tui_command(&t, "/fast garbage"); /* usage error keeps the tier */
    ASSERT_STR_EQ("default", ctx.service_tier);

    free(ctx.service_tier);
    buf_free(&t.out);
    buf_free(&t.partial);
    PASS();
}

/* ---- wrap math ---- */

TEST wrap_empty_is_one_row(void) {
    int row = -1, col = -1, total = -1;
    tui_wrap_locate("", 0, 0, 10, &row, &col, &total);
    ASSERT_EQ(0, row);
    ASSERT_EQ(0, col);
    ASSERT_EQ(1, total);
    ASSERT_EQ(0, (int)tui_wrap_index("", 0, 10, 0, 0));
    PASS();
}

TEST wrap_soft_break_at_width(void) {
    const char *s = "abcdef"; /* 6 cols */
    int row, col, total;
    tui_wrap_locate(s, 6, 0, 3, &row, &col, &total);
    ASSERT_EQ(2, total);
    ASSERT_EQ(0, row);
    ASSERT_EQ(0, col);

    tui_wrap_locate(s, 6, 3, 3, &row, &col, &total);
    ASSERT_EQ(0, row); /* caret sits at the wrap point on the first visual row */
    ASSERT_EQ(3, col);

    tui_wrap_locate(s, 6, 4, 3, &row, &col, &total);
    ASSERT_EQ(1, row);
    ASSERT_EQ(1, col);

    ASSERT_EQ(0, (int)tui_wrap_index(s, 6, 3, 0, 0));
    ASSERT_EQ(3, (int)tui_wrap_index(s, 6, 3, 1, 0));
    ASSERT_EQ(5, (int)tui_wrap_index(s, 6, 3, 1, 2));
    PASS();
}

TEST wrap_hard_newline_is_its_own_row(void) {
    const char *s = "ab\ncd";
    int row, col, total;
    tui_wrap_locate(s, 5, 5, 80, &row, &col, &total);
    ASSERT_EQ(2, total);
    ASSERT_EQ(1, row);
    ASSERT_EQ(2, col);
    tui_wrap_locate(s, 5, 3, 80, &row, &col, &total); /* start of "cd" */
    ASSERT_EQ(1, row);
    ASSERT_EQ(0, col);
    ASSERT_EQ(3, (int)tui_wrap_index(s, 5, 80, 1, 0));
    PASS();
}

TEST wrap_width_matches_composer_prefix(void) {
    tui t;
    mk_tui(&t, 24);
    t.cols = 21; /* maxw=20, prefix 2 → 18 */
    ASSERT_EQ(18, tui_wrap_width(&t));
    t.cols = 8; /* floor at 8 */
    ASSERT_EQ(8, tui_wrap_width(&t));
    free_tui(&t);
    PASS();
}

TEST overlay_budget_counts_wrapped_composer(void) {
    tui t;
    mk_tui(&t, 24);
    t.cols = 12; /* wrap width 8 */
    buf_appends(&t.input, "abcdefghij"); /* 10 cols → 2 visual rows */
    ASSERT_EQ(20, tui_overlay_budget(&t)); /* 21 - extra composer row */
    free_tui(&t);
    PASS();
}

/* ---- key decoder ---- */

static tui_key dec(const char *p, size_t n) {
    tui_decoded d;
    size_t used = tui_decode_one(p, n, true, &d);
    (void)used;
    return d.key;
}

TEST decode_enter_vs_ctrl_j(void) {
    ASSERT_EQ(TUI_K_ENTER, dec("\r", 1));
    ASSERT_EQ(TUI_K_NEWLINE, dec("\n", 1)); /* Ctrl-J */
    ASSERT_EQ(TUI_K_PASTE, dec("\x16", 1)); /* Ctrl-V */
    ASSERT_EQ(TUI_K_NEWLINE, dec("\x1bj", 2)); /* Option-J */
    ASSERT_EQ(TUI_K_NEWLINE, dec("\x1bJ", 2));
    ASSERT_EQ(TUI_K_NEWLINE, dec("\x1b\r", 2)); /* Alt-Enter */
    PASS();
}

TEST decode_csi_u_shift_enter_and_ctrl_v(void) {
    ASSERT_EQ(TUI_K_NEWLINE, dec("\x1b[13;2u", 7));
    ASSERT_EQ(TUI_K_ENTER, dec("\x1b[13u", 5));
    ASSERT_EQ(TUI_K_NEWLINE, dec("\x1b[10;5u", 7));
    ASSERT_EQ(TUI_K_NEWLINE, dec("\x1b[106;3u", 8));
    ASSERT_EQ(TUI_K_PASTE, dec("\x1b[118;5u", 8));
    ASSERT_EQ(TUI_K_NEWLINE, dec("\x1b[27;2;13~", 10));
    ASSERT_EQ(TUI_K_PASTE, dec("\x1b[27;5;118~", 11));
    PASS();
}

TEST decode_csi_u_survives_every_split_boundary(void) {
    const char seq[] = "\x1b[13;2u";
    size_t n = sizeof seq - 1;
    for (size_t split = 1; split < n; split++) {
        tui_decoded d;
        size_t used = tui_decode_one(seq, split, false, &d);
        ASSERT_EQ(0, (int)used); /* needs the rest */
        char buf[16];
        memcpy(buf, seq, n);
        used = tui_decode_one(buf, n, true, &d);
        ASSERT_EQ((int)n, (int)used);
        ASSERT_EQ(TUI_K_NEWLINE, d.key);
    }
    PASS();
}

TEST decode_arrows_unchanged(void) {
    ASSERT_EQ(TUI_K_UP, dec("\x1b[A", 3));
    ASSERT_EQ(TUI_K_DEL, dec("\x1b[3~", 4));
    ASSERT_EQ(TUI_K_HOME, dec("\x1b[H", 3));
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
    RUN_TEST(write_strips_nul_bytes);
    RUN_TEST(write_dim_reopens_after_newline);
    RUN_TEST(write_dim_blank_lines_carry_no_sgr);
    RUN_TEST(write_dim_without_color_is_plain);
    RUN_TEST(prewarm_take_returns_connected_backend);
    RUN_TEST(prewarm_failed_connect_is_silent_and_discarded);
    RUN_TEST(prewarm_drop_mid_connect_cleans_up_on_the_thread);
    RUN_TEST(prewarm_take_rejects_a_switched_provider);
    RUN_TEST(prewarm_start_keeps_a_matching_warmup);
    RUN_TEST(prewarm_runs_create_or_resume_on_the_thread);
    RUN_TEST(prewarm_failed_resume_is_silent_and_discarded);
    RUN_TEST(prewarm_start_restarts_on_a_stale_resume_pointer);
    RUN_TEST(prewarm_applicability);
    RUN_TEST(fast_command_is_capability_gated);
    RUN_TEST(wrap_empty_is_one_row);
    RUN_TEST(wrap_soft_break_at_width);
    RUN_TEST(wrap_hard_newline_is_its_own_row);
    RUN_TEST(wrap_width_matches_composer_prefix);
    RUN_TEST(overlay_budget_counts_wrapped_composer);
    RUN_TEST(decode_enter_vs_ctrl_j);
    RUN_TEST(decode_csi_u_shift_enter_and_ctrl_v);
    RUN_TEST(decode_csi_u_survives_every_split_boundary);
    RUN_TEST(decode_arrows_unchanged);
}
