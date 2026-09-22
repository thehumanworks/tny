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
#include <unistd.h>

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
    buf_init(&t->agent_filter);
    buf_init(&t->overlay);
    buf_init(&t->note);
    t->rows = rows;
    t->cols = 100;
    t->tty = true;
    t->attr = true; /* a tty gets non-color SGR unless --color=never */
}

static void free_tui(tui *t) {
    buf_free(&t->out);
    buf_free(&t->partial);
    buf_free(&t->input);
    buf_free(&t->agent_filter);
    for (int i = 0; i < t->n_agent_rows; i++) free(t->agent_rows[i].workspace);
    free(t->agent_rows);
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

/* ---- full-screen view transitions ---- */

TEST clear_screen_discards_only_display_text(void) {
    tui t;
    mk_tui(&t, 24);
    t.attr = false; /* --color=never still needs terminal layout controls */
    buf_appends(&t.out, "queued chat\n");
    buf_appends(&t.partial, "unfinished reply");
    buf_appends(&t.input, "keep my draft");
    t.cur = t.input.len;
    t.block_rows = 7;
    t.cur_row = 6;
    t.dirty = false;

    tui_clear_screen(&t);

    ASSERT_STR_EQ("\x1b[H\x1b[2J\x1b[3J", t.out.data);
    ASSERT_EQ(0, (int)t.partial.len);
    ASSERT_EQ(0, t.block_rows);
    ASSERT_EQ(0, t.cur_row);
    ASSERT(t.dirty);
    ASSERT_STR_EQ("keep my draft", t.input.data);
    ASSERT_EQ(t.input.len, t.cur);
    free_tui(&t);
    PASS();
}

TEST clear_screen_preserves_non_tty_output(void) {
    tui t;
    mk_tui(&t, 24);
    t.tty = false;
    buf_appends(&t.out, "queued chat\n");
    buf_appends(&t.partial, "unfinished reply");
    t.dirty = false;

    tui_clear_screen(&t);

    ASSERT_STR_EQ("queued chat\n", t.out.data);
    ASSERT_STR_EQ("unfinished reply", t.partial.data);
    ASSERT_FALSE(t.dirty);
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

TEST write_dim_without_color_stays_dim(void) {
    /* NO_COLOR kills colors only: dim is an attribute and survives, so
     * reasoning traces keep their mute in colorless terminals
     * (docs/adr/0026). */
    tui t;
    mk_tui(&t, 24);
    t.color = false;
    tui_write_dim(&t, "abc\nde", 6);
    ASSERT_STR_EQ("\x1b[2mabc\x1b[0m\n", t.out.data);
    ASSERT_STR_EQ("\x1b[2mde\x1b[0m", t.partial.data);
    free_tui(&t);
    PASS();
}

TEST write_dim_without_attr_is_plain(void) {
    tui t;
    mk_tui(&t, 24);
    t.attr = false; /* --color=never: no SGR at all */
    tui_write_dim(&t, "abc\nde", 6);
    ASSERT_STR_EQ("abc\n", t.out.data);
    ASSERT_STR_EQ("de", t.partial.data);
    free_tui(&t);
    PASS();
}

/* ---- color vs attribute gates (docs/adr/0026) ---- */

TEST attr_gate_is_independent_from_color(void) {
    tui t;
    mk_tui(&t, 24);
    t.color = false; /* NO_COLOR look: colors off, structure on */
    ASSERT_STR_EQ("", tui_c(&t, "\x1b[31m"));
    ASSERT_STR_EQ("\x1b[7m", tui_attr(&t, "\x1b[7m"));
    ASSERT_STR_EQ("\x1b[0m", tui_attr(&t, "\x1b[0m"));
    t.attr = false; /* --color=never: nothing at all */
    ASSERT_STR_EQ("", tui_attr(&t, "\x1b[7m"));
    free_tui(&t);
    PASS();
}

TEST color_resolve_matrix(void) {
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    bool c = false, a = false;
    unsetenv("NO_COLOR");
    unsetenv("CLICOLOR_FORCE");

    tny_color_resolve(&ctx, true, &c, &a); /* plain tty: everything on */
    ASSERT(c);
    ASSERT(a);
    tny_color_resolve(&ctx, false, &c, &a); /* piped: nothing */
    ASSERT_FALSE(c);
    ASSERT_FALSE(a);

    setenv("NO_COLOR", "1", 1); /* colors off, the bar keeps reverse video */
    tny_color_resolve(&ctx, true, &c, &a);
    ASSERT_FALSE(c);
    ASSERT(a);
    setenv("NO_COLOR", "", 1); /* tny honors even an empty NO_COLOR */
    tny_color_resolve(&ctx, true, &c, &a);
    ASSERT_FALSE(c);
    ASSERT(a);

    setenv("CLICOLOR_FORCE", "1", 1); /* force beats NO_COLOR… */
    tny_color_resolve(&ctx, true, &c, &a);
    ASSERT(c);
    ASSERT(a);
    tny_color_resolve(&ctx, false, &c, &a); /* …and works when piped */
    ASSERT(c);
    ASSERT(a);
    setenv("CLICOLOR_FORCE", "0", 1); /* "0" never forces */
    tny_color_resolve(&ctx, true, &c, &a);
    ASSERT_FALSE(c);
    ASSERT(a);
    unsetenv("CLICOLOR_FORCE");

    ctx.force_color = true; /* --color=always, piped */
    tny_color_resolve(&ctx, false, &c, &a);
    ASSERT(c);
    ASSERT(a);
    ctx.no_color = true; /* explicit never beats every force */
    tny_color_resolve(&ctx, true, &c, &a);
    ASSERT_FALSE(c);
    ASSERT_FALSE(a);

    unsetenv("NO_COLOR");
    PASS();
}

TEST status_row_is_a_reverse_bar_with_attrs(void) {
    tui t;
    mk_tui(&t, 24);
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cwd = (char *)"/work";
    t.ctx = &ctx;
    t.color = false; /* the NO_COLOR case from the sandbox screenshot */
    buf_t b;
    buf_init(&b);
    tui_status_row(&t, &b, 39);
    ASSERT(str_starts(b.data, "\x1b[7m"));
    ASSERT(str_ends(b.data, "\x1b[0m"));
    ASSERT_EQ(4 + 39 + 4, (int)b.len); /* padded to the full row width */
    ASSERT(strstr(b.data, "openai  default  ask  /work"));
    buf_free(&b);
    free_tui(&t);
    PASS();
}

TEST status_row_falls_back_to_delimiters_without_sgr(void) {
    tui t;
    mk_tui(&t, 24);
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cwd = (char *)"/work";
    t.ctx = &ctx;
    t.attr = false;
    t.color = false; /* --color=never */
    buf_t b;
    buf_init(&b);
    tui_status_row(&t, &b, 39);
    ASSERT_STR_EQ("── openai  default  ask  /work ──", b.data);
    buf_free(&b);
    free_tui(&t);
    PASS();
}

TEST status_row_shows_one_sided_token_counts(void) {
    /* in_tok/out_tok are independent: either one alone must bring the
     * token segment out (||, not &&) */
    tui t;
    mk_tui(&t, 24);
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cwd = (char *)"/work";
    t.ctx = &ctx;
    buf_t b;
    buf_init(&b);
    t.in_tok = 5;
    tui_status_row(&t, &b, 79);
    ASSERT(strstr(b.data, "5/0 tok"));
    buf_clear(&b);
    t.in_tok = 0;
    t.out_tok = 7;
    tui_status_row(&t, &b, 79);
    ASSERT(strstr(b.data, "0/7 tok"));
    buf_free(&b);
    free_tui(&t);
    PASS();
}

TEST status_row_fallback_truncates_to_width(void) {
    tui t;
    mk_tui(&t, 24);
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cwd = (char *)"/work";
    t.ctx = &ctx;
    t.attr = false;
    buf_t b;
    buf_init(&b);
    tui_status_row(&t, &b, 20); /* "── " + 14 columns + " ──" */
    ASSERT_STR_EQ("── openai  defaul ──", b.data);
    buf_free(&b);
    free_tui(&t);
    PASS();
}

TEST status_row_keeps_session_identity_ahead_of_task(void) {
    tui t;
    mk_tui(&t, 24);
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cwd = (char *)"/work";
    ctx.task_name = (char *)"a-very-long-task-name";
    t.ctx = &ctx;
    t.attr = false;
    tny_session_state session = {0};
    session.id = (char *)"0123456789abcdef";
    t.session = &session;
    buf_t b;
    buf_init(&b);
    tui_status_row(&t, &b, 46);
    ASSERT(strstr(b.data, "0123456789abcdef"));
    const char *sid = strstr(b.data, "0123456789abcdef");
    const char *task = strstr(b.data, "task:");
    ASSERT(!task || sid < task);
    buf_free(&b);
    free_tui(&t);
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

    ctx.backend = TNY_BK_COUNT; /* no fast tier: the command must refuse */
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

TEST ssh_command_rejects_local_custom_task(void) {
    tui t;
    struct tny_ctx ctx;
    cli_globals globals;
    memset(&ctx, 0, sizeof ctx);
    memset(&globals, 0, sizeof globals);
    mk_tui(&t, 24);
    t.ctx = &ctx;
    t.g = &globals;
    ctx.task_name = xstrdup("security-review");
    ctx.task_source = xstrdup("project");
    ctx.task_instructions = xstrdup("local project instructions");

    tui_command(&t, "/ssh example.invalid");
    ASSERT_EQ(NULL, ctx.ssh_host);
    ASSERT(strstr(t.out.data, "custom task presets cannot cross the SSH workspace boundary"));
    ASSERT_STR_EQ("security-review", ctx.task_name);

    free(ctx.task_name);
    free(ctx.task_source);
    free(ctx.task_instructions);
    free_tui(&t);
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
    t.cols = 12;                           /* wrap width 8 */
    buf_appends(&t.input, "abcdefghij");   /* 10 cols → 2 visual rows */
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
    ASSERT_EQ(TUI_K_NEWLINE, dec("\n", 1));    /* Ctrl-J */
    ASSERT_EQ(TUI_K_PASTE, dec("\x16", 1));    /* Ctrl-V */
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

TEST decode_bracketed_paste_markers(void) {
    ASSERT_EQ(TUI_K_PASTE_BEGIN, dec("\x1b[200~", 6));
    ASSERT_EQ(TUI_K_NONE, dec("\x1b[201~", 6)); /* stray end: consumed, no key */
    PASS();
}

TEST paste_scan_normalizes_newlines(void) {
    buf_t out;
    buf_init(&out);
    bool done = false;
    const char in[] = "a\r\nb\rc\nd\x1b[201~";
    size_t used = tui_paste_scan(in, sizeof in - 1, &out, &done);
    ASSERT_EQ((int)(sizeof in - 1), (int)used);
    ASSERT(done);
    ASSERT_STR_EQ("a\nb\nc\nd", out.data);
    buf_free(&out);
    PASS();
}

TEST paste_scan_keeps_stray_esc_literal(void) {
    buf_t out;
    buf_init(&out);
    bool done = false;
    const char in[] = "x\x1b[Ay\x1b[201~";
    size_t used = tui_paste_scan(in, sizeof in - 1, &out, &done);
    ASSERT_EQ((int)(sizeof in - 1), (int)used);
    ASSERT(done);
    ASSERT_STR_EQ("x\x1b[Ay", out.data);
    buf_free(&out);
    PASS();
}

TEST paste_scan_survives_every_split_boundary(void) {
    const char in[] = "ab\r\ncd\r\x1b[201~";
    size_t n = sizeof in - 1;
    for (size_t split = 1; split < n; split++) {
        buf_t out;
        buf_init(&out);
        bool done = false;
        size_t off = 0;
        /* feed the first `split` bytes, then the rest, re-offering the
         * unconsumed tail each round the way decode_all does */
        size_t avail = split;
        for (int rounds = 0; rounds < 8 && !done; rounds++) {
            off += tui_paste_scan(in + off, avail - off, &out, &done);
            avail = n;
        }
        ASSERT(done);
        ASSERT_EQ((int)n, (int)off);
        ASSERT_STR_EQ("ab\ncd\n", out.data);
        buf_free(&out);
    }
    PASS();
}

TEST decode_arrows_unchanged(void) {
    ASSERT_EQ(TUI_K_UP, dec("\x1b[A", 3));
    ASSERT_EQ(TUI_K_DEL, dec("\x1b[3~", 4));
    ASSERT_EQ(TUI_K_HOME, dec("\x1b[H", 3));
    PASS();
}

/* ---- terminal size probe (docs/adr/0054) ---- */

TEST decode_cpr_reports_terminal_size(void) {
    tui_decoded d;
    const char *rep = "\x1b[24;44R";
    ASSERT_EQ(strlen(rep), tui_decode_one(rep, strlen(rep), true, &d));
    ASSERT_EQ(TUI_K_CPR, d.key);
    ASSERT_EQ(24, d.cpr_row);
    ASSERT_EQ(44, d.cpr_col);

    /* xterm's Shift-F3 is ESC[1;2R: consumed, never mistaken for a report */
    const char *f3 = "\x1b[1;2R";
    ASSERT_EQ(strlen(f3), tui_decode_one(f3, strlen(f3), true, &d));
    ASSERT_EQ(TUI_K_NONE, d.key);

    /* split anywhere: the decoder waits for the final byte */
    for (size_t split = 1; split < strlen(rep); split++) {
        ASSERT_EQ(0, (int)tui_decode_one(rep, split, false, &d));
    }
    PASS();
}

TEST size_report_applies_only_real_changes(void) {
    tui t;
    mk_tui(&t, 24);
    t.cols = 80;
    tui_size_report(&t, 24, 44);
    ASSERT_EQ(44, t.cols);
    ASSERT_EQ(24, t.rows);
    ASSERT(t.dirty);

    t.dirty = false;
    tui_size_report(&t, 24, 44); /* same answer: no spurious repaint */
    ASSERT_FALSE(t.dirty);

    tui_size_report(&t, 1, 1); /* not a corner report */
    ASSERT_EQ(44, t.cols);
    ASSERT_FALSE(t.dirty);

    tui_size_report(&t, 30, 5); /* floor matches tui_size */
    ASSERT_EQ(20, t.cols);
    ASSERT_EQ(30, t.rows);
    free_tui(&t);
    PASS();
}

/* A tiny VT model: enough of a screen to see what erase_block's cursor-up
 * arithmetic leaves behind. Rows are cell arrays of code points (stored as
 * the first byte, ASCII is all the block prints here); DECAWM honoured. */
#define SCR_W 44
#define SCR_H 12
typedef struct {
    char cell[SCR_H][SCR_W + 1];
    int r, c;
    bool wrap, pend;
} scr;

static void scr_init(scr *s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < SCR_H; i++) memset(s->cell[i], ' ', SCR_W);
    s->wrap = true;
}

static void scr_lf(scr *s) {
    s->pend = false;
    if (s->r + 1 < SCR_H) {
        s->r++;
        return;
    }
    memmove(s->cell[0], s->cell[1], sizeof s->cell[0] * (SCR_H - 1));
    memset(s->cell[SCR_H - 1], ' ', SCR_W);
}

static void scr_put(scr *s, char ch) {
    if (s->pend && s->wrap) {
        s->c = 0;
        scr_lf(s);
    }
    s->cell[s->r][s->c] = ch;
    if (s->c + 1 < SCR_W) s->c++;
    else s->pend = true; /* last column: wrap (if enabled) on the next glyph */
}

static void scr_feed(scr *s, const char *p, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char ch = (unsigned char)p[i];
        if (ch == '\r') {
            s->c = 0;
            s->pend = false;
            i++;
        } else if (ch == '\n') { /* raw mode keeps ONLCR: LF arrives as CR LF */
            s->c = 0;
            scr_lf(s);
            i++;
        } else if (ch == 0x1b && i + 1 < n && p[i + 1] == '[') {
            size_t j = i + 2;
            bool priv = j < n && p[j] == '?';
            if (priv) j++;
            int arg = 0;
            while (j < n && p[j] >= '0' && p[j] <= '9') arg = arg * 10 + (p[j++] - '0');
            while (j < n && !((unsigned char)p[j] >= 0x40 && (unsigned char)p[j] <= 0x7e)) j++;
            char fin = j < n ? p[j] : 0;
            if (fin == 'A') s->r -= arg ? arg : 1, s->pend = false;
            else if (fin == 'C') s->c += arg ? arg : 1;
            else if (fin == 'J') {
                memset(s->cell[s->r] + s->c, ' ', (size_t)(SCR_W - s->c));
                for (int k = s->r + 1; k < SCR_H; k++) memset(s->cell[k], ' ', SCR_W);
            } else if (priv && arg == 7) s->wrap = fin == 'h';
            if (s->r < 0) s->r = 0;
            if (s->c >= SCR_W) s->c = SCR_W - 1;
            i = j + 1;
        } else if (ch == 0x1b) {
            i += 2; /* ESC 7 / ESC 8 and friends: not used by the block */
        } else {
            size_t len = 1;
            if (ch >= 0xF0) len = 4;
            else if (ch >= 0xE0) len = 3;
            else if (ch >= 0xC0) len = 2;
            scr_put(s, (char)ch);
            i += len;
        }
    }
}

static int scr_count(const scr *s, const char *needle) {
    int hits = 0;
    for (int i = 0; i < SCR_H; i++) {
        char row[SCR_W + 1];
        memcpy(row, s->cell[i], SCR_W);
        row[SCR_W] = 0;
        if (strstr(row, needle)) hits++;
    }
    return hits;
}

/* Render three times (the user typing) into a capture of stdout. */
static char *render_capture(tui *t, size_t *len) {
    int fds[2];
    if (pipe(fds) != 0) return NULL;
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);
    for (int i = 0; i < 3; i++) {
        buf_appends(&t->input, "x");
        t->cur = t->input.len;
        t->dirty = true;
        tui_render(t);
    }
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    buf_t b;
    buf_init(&b);
    char tmp[4096];
    ssize_t n;
    while ((n = read(fds[0], tmp, sizeof tmp)) > 0) buf_append(&b, tmp, (size_t)n);
    close(fds[0]);
    *len = b.len;
    return b.data ? b.data : xstrdup("");
}

/* ---- leading whitespace of a streamed reply / reasoning block ---- */

static void mk_turn_tui(tui *t, struct tny_ctx *ctx) {
    mk_tui(t, SCR_H);
    memset(ctx, 0, sizeof *ctx);
    ctx->cwd = (char *)"/workdir";
    ctx->model = (char *)"m";
    t->ctx = ctx;
    t->color = false;
    t->attr = false; /* plain bytes: "· " and the text stay greppable */
    t->cols = SCR_W;
    t->gap = 1; /* as after the echoed prompt: one blank line, then output */
    t->turn_active = true;
}

static void ev_text(tui *t, tny_event_kind kind, const char *s) {
    tny_backend_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    ev.text = s;
    ev.text_len = strlen(s);
    tui_handle_backend_event(t, &ev);
}

/* Render into the screen emulator and free the capture. */
static bool paint(tui *t, scr *s) {
    size_t n = 0;
    char *out = render_capture(t, &n);
    if (!out) return false;
    scr_init(s);
    scr_feed(s, out, n);
    free(out);
    return true;
}

/* Row text with trailing blanks trimmed (scr keeps a glyph's first byte). */
static bool row_is(const scr *s, int i, const char *want) {
    char row[SCR_W + 1];
    memcpy(row, s->cell[i], SCR_W);
    row[SCR_W] = 0;
    for (int k = SCR_W; k > 0 && row[k - 1] == ' '; k--) row[k - 1] = 0;
    return strcmp(row, want) == 0;
}

static void free_turn_tui(tui *t) {
    buf_free(&t->last_reply);
    free_tui(t);
}

/* Some models open the answer with blank lines ("\n\n\n\nHi there!"). The
 * transcript starts at the first visible byte; the wire keeps the newlines. */
TEST render_drops_leading_newlines_of_the_reply(void) {
    tui t;
    struct tny_ctx ctx;
    mk_turn_tui(&t, &ctx);
    ev_text(&t, TNY_EV_TEXT_DELTA, "\n\n\n\nanswer");
    scr s;
    ASSERT(paint(&t, &s));
    ASSERT(row_is(&s, 0, "")); /* the one gap line after the prompt */
    ASSERT(row_is(&s, 1, "answer"));
    ASSERT_STR_EQ("answer", t.last_reply.data); /* /copy sees the same */
    free_turn_tui(&t);
    PASS();
}

/* The same newlines split across deltas: "\n", "\n\nans", "wer". */
TEST render_drops_leading_whitespace_split_across_deltas(void) {
    tui t;
    struct tny_ctx ctx;
    mk_turn_tui(&t, &ctx);
    ev_text(&t, TNY_EV_TEXT_DELTA, "\n");
    ev_text(&t, TNY_EV_TEXT_DELTA, "\n\nans");
    ev_text(&t, TNY_EV_TEXT_DELTA, "wer");
    scr s;
    ASSERT(paint(&t, &s));
    ASSERT(row_is(&s, 0, ""));
    ASSERT(row_is(&s, 1, "answer"));
    free_turn_tui(&t);
    PASS();
}

/* An empty / whitespace-only reasoning stream paints nothing: no "· "
 * marker, no blank line, and the text still lands right after the gap. */
TEST render_skips_empty_thinking_block(void) {
    tui t;
    struct tny_ctx ctx;
    mk_turn_tui(&t, &ctx);
    ev_text(&t, TNY_EV_THINKING, "");
    ev_text(&t, TNY_EV_THINKING, "\n\n");
    ev_text(&t, TNY_EV_THINKING, "  \n");
    ASSERT_FALSE(t.in_thinking);
    ev_text(&t, TNY_EV_TEXT_DELTA, "answer");
    scr s;
    ASSERT(paint(&t, &s));
    ASSERT_EQ(0, scr_count(&s, "\xc2")); /* no "·" anywhere */
    ASSERT(row_is(&s, 0, ""));
    ASSERT(row_is(&s, 1, "answer"));
    free_turn_tui(&t);
    PASS();
}

/* Real reasoning still renders: marker, text, then the answer on its own
 * line. Leading whitespace of the block is dropped, inner whitespace kept. */
TEST render_keeps_real_thinking(void) {
    tui t;
    struct tny_ctx ctx;
    mk_turn_tui(&t, &ctx);
    ev_text(&t, TNY_EV_THINKING, "\n\n");
    ev_text(&t, TNY_EV_THINKING, "plan a");
    ev_text(&t, TNY_EV_THINKING, "\nthen b");
    ev_text(&t, TNY_EV_TEXT_DELTA, "answer");
    scr s;
    ASSERT(paint(&t, &s));
    ASSERT(row_is(&s, 0, ""));
    ASSERT(row_is(&s, 1, "\xc2 plan a")); /* "·" keeps its first byte */
    ASSERT(row_is(&s, 2, "then b"));
    ASSERT(row_is(&s, 3, "answer"));
    free_turn_tui(&t);
    PASS();
}

/* Only the leading run goes: whitespace inside and after the first visible
 * byte (code blocks, paragraph breaks) is untouched. */
TEST render_keeps_whitespace_after_first_visible_byte(void) {
    tui t;
    struct tny_ctx ctx;
    mk_turn_tui(&t, &ctx);
    ev_text(&t, TNY_EV_TEXT_DELTA, " \nline one\n\n");
    ev_text(&t, TNY_EV_TEXT_DELTA, "\n    indented\n\n");
    ev_text(&t, TNY_EV_TEXT_DELTA, "  tail  ");
    ASSERT_STR_EQ("line one\n\n\n    indented\n\n  tail  ", t.last_reply.data);
    scr s;
    ASSERT(paint(&t, &s));
    ASSERT(row_is(&s, 0, ""));
    ASSERT(row_is(&s, 1, "line one"));
    ASSERT(row_is(&s, 2, ""));
    ASSERT(row_is(&s, 3, ""));
    ASSERT(row_is(&s, 4, "    indented"));
    ASSERT(row_is(&s, 5, ""));
    ASSERT(row_is(&s, 6, "  tail"));
    free_turn_tui(&t);
    PASS();
}

/* The screenshot bug: the pty says 80 columns, the phone terminal has 44.
 * The reverse-video status row padded to 79 cells soft-wrapped onto two
 * physical rows, erase_block moved up one row too few, and every keypress
 * left one more copy of the status row on screen. */
TEST render_narrow_terminal_leaves_one_status_row(void) {
    tui t;
    mk_tui(&t, SCR_H);
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cwd = (char *)"/workdir";
    ctx.model = (char *)"claude-sonnet-4-6";
    t.ctx = &ctx;
    t.color = false;
    t.cols = 80; /* what TIOCGWINSZ claims */

    size_t n = 0;
    char *out = render_capture(&t, &n);
    ASSERT(out);
    scr s;
    scr_init(&s);
    scr_feed(&s, out, n);
    ASSERT_EQ(1, scr_count(&s, "claude-sonnet-4-6"));
    ASSERT_EQ(1, scr_count(&s, "> xxx"));
    ASSERT(strstr(out, "\x1b[?7l")); /* autowrap off around the block */
    ASSERT(strstr(out, "\x1b[?7h")); /* ... and back on for the transcript */
    ASSERT(s.wrap);                  /* so transcript lines still soft-wrap */
    free(out);

    /* once the CPR answer lands the row is cut to the real width and the
     * picture is identical with autowrap on */
    tui_size_report(&t, SCR_H, SCR_W);
    out = render_capture(&t, &n);
    ASSERT(out);
    scr_init(&s);
    scr_feed(&s, out, n);
    ASSERT_EQ(1, scr_count(&s, "claude-sonnet-4-6"));
    ASSERT_EQ(1, scr_count(&s, "> xxxxxx"));
    free(out);
    free_tui(&t);
    PASS();
}

/* A streaming line whose glyphs the column counter thinks fit — the emoji
 * in "Hello! 👋" is two cells wide on screen but one to tui_push_ansi — must
 * not wrap either: the same DECAWM guard clips it. */
TEST render_partial_line_at_the_margin_does_not_duplicate(void) {
    tui t;
    mk_tui(&t, SCR_H);
    struct tny_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cwd = (char *)"/w";
    t.ctx = &ctx;
    t.color = false;
    t.cols = SCR_W + 2; /* rows one cell wider than the screen: each spills */
    buf_appends(&t.partial, "Hello! How can I help you today? Whether you");

    size_t n = 0;
    char *out = render_capture(&t, &n);
    ASSERT(out);
    scr s;
    scr_init(&s);
    scr_feed(&s, out, n);
    ASSERT_EQ(1, scr_count(&s, "Hello! How can"));
    ASSERT_EQ(1, scr_count(&s, "openai"));
    free(out);
    free_tui(&t);
    PASS();
}

TEST agents_unreadable_workspace_preserves_context(void) {
    const char *names[] = {"cursor", "acp", "acp@fixture", "acp:fixture"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        tui t;
        mk_tui(&t, 24);
        tny_ctx ctx = {0};
        ctx.provider_name = "gateway";
        ctx.api_key = "fixture-key";
        session_meta meta = {0};
        meta.backend = (char *)names[i];
        meta.workspace = "/must-not-load";
        cli_globals g = {0};
        t.g = &g;
        t.ctx = &ctx;
        t.agents = &meta;
        t.n_agents = 1;
        tui_agents_select(&t);
        ASSERT_EQ(&ctx, t.ctx);
        ASSERT_STR_EQ("gateway", ctx.provider_name);
        ASSERT_STR_EQ("fixture-key", ctx.api_key);
        ASSERT_EQ(NULL, t.session);
        ASSERT(t.out.data && strstr(t.out.data, "cannot load the saved session's workspace"));
        free_tui(&t);
    }
    PASS();
}

TEST agents_group_filter_and_selection_mapping(void) {
    tui t;
    mk_tui(&t, 24);
    tny_ctx ctx = {0};
    ctx.cwd = (char *)"/tmp/current";
    cli_globals g = {0};
    t.ctx = &ctx;
    t.g = &g;
    session_meta entries[] = {
        {.id = "new-other", .workspace = "/tmp/QOther", .updated = "2026-01-04"},
        {.id = "new-current", .workspace = "/tmp/current", .updated = "2026-01-03"},
        {.id = "old-other", .workspace = "/tmp/QOther", .updated = "2026-01-02"},
        {.id = "old-current", .workspace = "/tmp/current", .updated = "2026-01-01"},
    };
    t.agents = entries;
    t.n_agents = 4;
    tui_agents_rebuild(&t);
    ASSERT_EQ(4, t.n_agent_rows);
    /* The fixture is in newest-first order within each path. */
    ASSERT_STR_EQ("new-current", entries[t.agent_rows[0].session_index].id);
    ASSERT_STR_EQ("old-current", entries[t.agent_rows[1].session_index].id);
    ASSERT_STR_EQ("new-other", entries[t.agent_rows[2].session_index].id);
    ASSERT(t.overlay.data && strstr(t.overlay.data, "/tmp/current\n"));
    ASSERT(strstr(t.overlay.data, "/tmp/QOther\n"));
    t.rows = 7; /* four overlay rows: header, heading, selected row, spare */
    t.agent_selected = 3;
    tui_agents_rebuild(&t);
    ASSERT(strstr(t.overlay.data, "/tmp/QOther\n"));
    ASSERT(strstr(t.overlay.data, "> old-other"));
    int lines = 0;
    for (const char *p = t.overlay.data; *p; p++)
        if (*p == '\n') lines++;
    ASSERT(lines <= tui_overlay_budget(&t));
    t.agent_selected = 2;
    buf_appends(&t.agent_filter, "qtr"); /* case-insensitive subsequence of QOther */
    tui_agents_rebuild(&t);
    ASSERT_EQ(2, t.n_agent_rows);
    ASSERT_EQ(0, t.agent_selected);
    ASSERT_STR_EQ("new-other", entries[t.agent_rows[t.agent_selected].session_index].id);
    buf_clear(&t.agent_filter);
    buf_appends(&t.agent_filter, "nowhere");
    tui_agents_rebuild(&t);
    ASSERT_EQ(0, t.n_agent_rows);
    ASSERT(strstr(t.overlay.data, "No matching workspaces."));
    free_tui(&t);
    PASS();
}

TEST agents_filter_cap_keeps_utf8_codepoints_whole(void) {
    buf_t filter;
    buf_init(&filter);
    char prefix[4096];
    memset(prefix, 'x', sizeof prefix);
    tui_filter_append_utf8(&filter, prefix, 4095);
    tui_filter_append_utf8(&filter, "\xc3", 1);
    tui_filter_append_utf8(&filter, "\xa9", 1);
    ASSERT_EQ(4095, filter.len); /* no room for all of é */
    ASSERT_EQ('x', filter.data[filter.len - 1]);
    buf_clear(&filter);
    tui_filter_append_utf8(&filter, prefix, 4094);
    tui_filter_append_utf8(&filter, "\xc3", 1);
    tui_filter_append_utf8(&filter, "\xa9", 1);
    ASSERT_EQ(4096, filter.len);
    ASSERT_EQ((unsigned char)0xc3, (unsigned char)filter.data[4094]);
    ASSERT_EQ((unsigned char)0xa9, (unsigned char)filter.data[4095]);
    buf_free(&filter);
    PASS();
}

TEST agents_current_bucket_legacy_rows_share_cwd_group(void) {
    tui t;
    mk_tui(&t, 4); /* one overlay row: compact selected session and workspace */
    tny_ctx ctx = {0};
    ctx.cwd = (char *)"/tmp/current";
    snprintf(ctx.ws_hash, sizeof ctx.ws_hash, "%s", "1111111111111111");
    cli_globals g = {0};
    t.ctx = &ctx;
    t.g = &g;
    session_meta entries[] = {
        {.id = "foreign", .workspace = NULL, .ws_hash = "2222222222222222"},
        {.id = "known", .workspace = "/tmp/current", .ws_hash = "1111111111111111"},
        {.id = "legacy", .workspace = NULL, .ws_hash = "1111111111111111"},
    };
    t.agents = entries;
    t.n_agents = 3;
    buf_appends(&t.agent_filter, "/TMP/CUR");
    tui_agents_rebuild(&t);
    ASSERT_EQ(2, t.n_agent_rows);
    ASSERT_STR_EQ("/tmp/current", t.agent_rows[0].workspace);
    ASSERT_STR_EQ("/tmp/current", t.agent_rows[1].workspace);
    ASSERT_STR_EQ("known", entries[t.agent_rows[0].session_index].id);
    ASSERT_STR_EQ("legacy", entries[t.agent_rows[1].session_index].id);
    ASSERT(strstr(t.overlay.data, "  > known  /tmp/current"));
    ASSERT_EQ(1, tui_overlay_budget(&t));
    free_tui(&t);
    PASS();
}

/* The dashboard transition cancels setup; submission must also fail closed
 * if another transition ever leaves a stale wizard in either kind of replica. */
TEST saved_view_discards_stale_provider_wizard(void) {
    for (int attached = 0; attached < 2; attached++) {
        tui t;
        mk_tui(&t, 24);
        tny_ctx ctx = {0};
        ctx.provider_name = "fixture";
        ctx.model = "saved-model";
        ctx.api_key = "synthetic-only";
        t.ctx = &ctx;
        t.session_readonly = !attached;
        t.background_view = attached;
        t.wiz_step = 2;
        t.wiz_name = xstrdup("wizard-leak");
        t.wiz_base = xstrdup("http://127.0.0.1:1/v1");
        t.wiz_key_env = xstrdup("WIZARD_LEAK_API_KEY");
        t.wiz_model = xstrdup("wizard-leak-model");

        tui_submit(&t, "/continue");
        ASSERT_EQ(0, t.wiz_step);
        ASSERT_EQ(NULL, t.wiz_name);
        ASSERT_EQ(NULL, t.wiz_base);
        ASSERT_EQ(NULL, t.wiz_key_env);
        ASSERT_EQ(NULL, t.wiz_model);
        ASSERT_STR_EQ("fixture", ctx.provider_name);
        ASSERT_STR_EQ("saved-model", ctx.model);
        ASSERT_STR_EQ("synthetic-only", ctx.api_key);
        ASSERT_EQ(1, t.n_hist); /* reached command routing, not the wizard */
        ASSERT_STR_EQ("/continue", t.hist[0]);
        tui_hist_free(&t);
        free_tui(&t);
    }
    PASS();
}

SUITE(tui_suite) {
    RUN_TEST(agents_unreadable_workspace_preserves_context);
    RUN_TEST(agents_group_filter_and_selection_mapping);
    RUN_TEST(agents_filter_cap_keeps_utf8_codepoints_whole);
    RUN_TEST(agents_current_bucket_legacy_rows_share_cwd_group);
    RUN_TEST(saved_view_discards_stale_provider_wizard);
    RUN_TEST(push_ansi_plain_truncates);
    RUN_TEST(push_ansi_sgr_is_zero_width);
    RUN_TEST(push_ansi_reset_survives_the_cut);
    RUN_TEST(push_ansi_drops_controls_keeps_utf8);
    RUN_TEST(push_ansi_non_sgr_escape_dropped);
    RUN_TEST(overlay_budget_accounts_for_the_block);
    RUN_TEST(overlay_linef_and_clear);
    RUN_TEST(overlay_linef_falls_back_without_a_tty);
    RUN_TEST(clear_screen_discards_only_display_text);
    RUN_TEST(clear_screen_preserves_non_tty_output);
    RUN_TEST(write_strips_nul_bytes);
    RUN_TEST(write_dim_reopens_after_newline);
    RUN_TEST(write_dim_blank_lines_carry_no_sgr);
    RUN_TEST(write_dim_without_color_stays_dim);
    RUN_TEST(write_dim_without_attr_is_plain);
    RUN_TEST(attr_gate_is_independent_from_color);
    RUN_TEST(color_resolve_matrix);
    RUN_TEST(status_row_is_a_reverse_bar_with_attrs);
    RUN_TEST(status_row_falls_back_to_delimiters_without_sgr);
    RUN_TEST(status_row_fallback_truncates_to_width);
    RUN_TEST(status_row_keeps_session_identity_ahead_of_task);
    RUN_TEST(status_row_shows_one_sided_token_counts);
    RUN_TEST(fast_command_is_capability_gated);
    RUN_TEST(ssh_command_rejects_local_custom_task);
    RUN_TEST(wrap_empty_is_one_row);
    RUN_TEST(wrap_soft_break_at_width);
    RUN_TEST(wrap_hard_newline_is_its_own_row);
    RUN_TEST(wrap_width_matches_composer_prefix);
    RUN_TEST(decode_bracketed_paste_markers);
    RUN_TEST(paste_scan_normalizes_newlines);
    RUN_TEST(paste_scan_keeps_stray_esc_literal);
    RUN_TEST(paste_scan_survives_every_split_boundary);
    RUN_TEST(overlay_budget_counts_wrapped_composer);
    RUN_TEST(decode_enter_vs_ctrl_j);
    RUN_TEST(decode_csi_u_shift_enter_and_ctrl_v);
    RUN_TEST(decode_csi_u_survives_every_split_boundary);
    RUN_TEST(decode_arrows_unchanged);
    RUN_TEST(decode_cpr_reports_terminal_size);
    RUN_TEST(size_report_applies_only_real_changes);
    RUN_TEST(render_narrow_terminal_leaves_one_status_row);
    RUN_TEST(render_partial_line_at_the_margin_does_not_duplicate);
    RUN_TEST(render_drops_leading_newlines_of_the_reply);
    RUN_TEST(render_drops_leading_whitespace_split_across_deltas);
    RUN_TEST(render_skips_empty_thinking_block);
    RUN_TEST(render_keeps_real_thinking);
    RUN_TEST(render_keeps_whitespace_after_first_visible_byte);
}
