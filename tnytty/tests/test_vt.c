/* test_vt.c — the VT core: grid ops, SGR, UTF-8 widths, scrollback,
 * alt screen, responses, kitty APC, and the every-split-boundary rule. */
#include "greatest.h"
#include "vt/vt.h"

#include <stdlib.h>
#include <string.h>

static char line_buf[1024];

static const char *row(vt *t, int y) {
    vt_line_text(t, y, line_buf, sizeof line_buf);
    return line_buf;
}

TEST plain_text_lands_in_cells(void) {
    vt *t = vt_new(10, 3, 0);
    vt_feed(t, "hi", 2);
    ASSERT_STR_EQ("hi", row(t, 0));
    ASSERT_EQ(2, vt_cursor_x(t));
    ASSERT_EQ(0, vt_cursor_y(t));
    const vt_cell *c = vt_line(t, 0);
    ASSERT_EQ((uint32_t)'h', c[0].cp);
    vt_free(t);
    PASS();
}

TEST crlf_and_wrap(void) {
    vt *t = vt_new(5, 3, 0);
    vt_feed(t, "abcdefgh", 8);
    ASSERT_STR_EQ("abcde", row(t, 0));
    ASSERT_STR_EQ("fgh", row(t, 1));
    vt_feed(t, "\r\nx", 3);
    ASSERT_STR_EQ("x", row(t, 2));
    vt_free(t);
    PASS();
}

TEST deferred_wrap_holds_last_column(void) {
    vt *t = vt_new(3, 2, 0);
    vt_feed(t, "abc", 3);
    /* cursor visually parked on the last column, no wrap yet */
    ASSERT_EQ(2, vt_cursor_x(t));
    ASSERT_EQ(0, vt_cursor_y(t));
    vt_feed(t, "\r\n", 2);
    ASSERT_STR_EQ("abc", row(t, 0));
    vt_free(t);
    PASS();
}

TEST scroll_pushes_scrollback(void) {
    vt *t = vt_new(10, 2, 8);
    vt_feed(t, "one\r\ntwo\r\nthree", 15);
    ASSERT_STR_EQ("two", row(t, 0));
    ASSERT_STR_EQ("three", row(t, 1));
    ASSERT_EQ(1, vt_scrollback_len(t));
    int cols = 0;
    const vt_cell *sb = vt_scrollback_line(t, 0, &cols);
    ASSERT(sb != NULL);
    ASSERT_EQ(10, cols);
    ASSERT_EQ((uint32_t)'o', sb[0].cp);
    vt_free(t);
    PASS();
}

TEST cup_ed_el(void) {
    vt *t = vt_new(10, 4, 0);
    vt_feed(t, "aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc", 34);
    vt_feed(t, "\x1b[2;3H", 6); /* row 2, col 3 */
    ASSERT_EQ(1, vt_cursor_y(t));
    ASSERT_EQ(2, vt_cursor_x(t));
    vt_feed(t, "\x1b[K", 3); /* erase to EOL */
    ASSERT_STR_EQ("bb", row(t, 1));
    vt_feed(t, "\x1b[2J\x1b[H", 7);
    ASSERT_STR_EQ("", row(t, 0));
    ASSERT_EQ(0, vt_cursor_x(t));
    ASSERT_EQ(0, vt_cursor_y(t));
    vt_free(t);
    PASS();
}

TEST sgr_colors_and_attrs(void) {
    vt *t = vt_new(10, 2, 0);
    const char *seq = "\x1b[1;31mX\x1b[0m\x1b[38;5;196mY\x1b[38;2;1;2;3m\x1b[42mZ";
    vt_feed(t, seq, strlen(seq));
    const vt_cell *c = vt_line(t, 0);
    ASSERT_EQ(VT_ATTR_BOLD, c[0].attrs);
    ASSERT_EQ(VT_COLOR_IDX | 1u, c[0].fg);
    ASSERT_EQ(VT_COLOR_IDX | 196u, c[1].fg);
    ASSERT_EQ(0, c[1].attrs);
    ASSERT_EQ(VT_COLOR_RGB | 0x010203u, c[2].fg);
    ASSERT_EQ(VT_COLOR_IDX | 2u, c[2].bg);
    vt_free(t);
    PASS();
}

TEST utf8_wide_and_combining(void) {
    vt *t = vt_new(10, 2, 0);
    vt_feed(t, "\xe6\xb1\x89x", 4); /* 汉 (wide) then x */
    const vt_cell *c = vt_line(t, 0);
    ASSERT_EQ(0x6C49u, c[0].cp);
    ASSERT(c[0].attrs & VT_ATTR_WIDE);
    ASSERT(c[1].attrs & VT_ATTR_WIDE_CONT);
    ASSERT_EQ((uint32_t)'x', c[2].cp);
    ASSERT_EQ(3, vt_cursor_x(t));
    /* e + combining acute lands in one cell */
    vt_feed(t, "\r\ne\xcc\x81", 5);
    c = vt_line(t, 1);
    ASSERT_EQ((uint32_t)'e', c[0].cp);
    ASSERT_EQ(0x0301u, c[0].combine);
    ASSERT_EQ(1, vt_cursor_x(t));
    vt_free(t);
    PASS();
}

TEST nerd_font_pua_is_single_width(void) {
    ASSERT_EQ(1, vt_cp_width(0xE0B0));  /* powerline separator */
    ASSERT_EQ(1, vt_cp_width(0xF0001)); /* plane-15 PUA */
    ASSERT_EQ(2, vt_cp_width(0x6C49));  /* CJK */
    ASSERT_EQ(2, vt_cp_width(0x1F600)); /* emoji */
    ASSERT_EQ(0, vt_cp_width(0x0301));  /* combining */
    vt *t = vt_new(4, 1, 0);
    vt_feed(t, "\xee\x82\xb0x", 4); /* U+E0B0 then x */
    ASSERT_EQ(2, vt_cursor_x(t));
    const vt_cell *c = vt_line(t, 0);
    ASSERT_EQ(0xE0B0u, c[0].cp);
    ASSERT_FALSE(c[0].attrs & VT_ATTR_WIDE);
    vt_free(t);
    PASS();
}

TEST alt_screen_round_trip(void) {
    vt *t = vt_new(10, 3, 0);
    vt_feed(t, "main", 4);
    vt_feed(t, "\x1b[?1049h", 8);
    ASSERT(vt_alt_screen(t));
    ASSERT_STR_EQ("", row(t, 0));
    vt_feed(t, "alt!", 4);
    ASSERT_STR_EQ("alt!", row(t, 0));
    vt_feed(t, "\x1b[?1049l", 8);
    ASSERT_FALSE(vt_alt_screen(t));
    ASSERT_STR_EQ("main", row(t, 0));
    ASSERT_EQ(4, vt_cursor_x(t));
    vt_free(t);
    PASS();
}

TEST scroll_region_stbm(void) {
    vt *t = vt_new(10, 4, 0);
    vt_feed(t, "top\r\naaa\r\nbbb\r\nbot", 18);
    vt_feed(t, "\x1b[2;3r", 6);   /* region rows 2..3 */
    vt_feed(t, "\x1b[3;1H\n", 7); /* LF at region bottom scrolls region only */
    ASSERT_STR_EQ("top", row(t, 0));
    ASSERT_STR_EQ("bbb", row(t, 1));
    ASSERT_STR_EQ("", row(t, 2));
    ASSERT_STR_EQ("bot", row(t, 3));
    vt_free(t);
    PASS();
}

TEST insert_delete_lines_and_chars(void) {
    vt *t = vt_new(8, 3, 0);
    vt_feed(t, "abcd\r\nwxyz", 10);
    vt_feed(t, "\x1b[1;1H\x1b[2@", 10); /* insert 2 cells at a */
    ASSERT_STR_EQ("  abcd", row(t, 0));
    vt_feed(t, "\x1b[2P", 4); /* delete them again */
    ASSERT_STR_EQ("abcd", row(t, 0));
    vt_feed(t, "\x1b[1L", 4); /* insert line above */
    ASSERT_STR_EQ("", row(t, 0));
    ASSERT_STR_EQ("abcd", row(t, 1));
    vt_feed(t, "\x1b[1M", 4);
    ASSERT_STR_EQ("abcd", row(t, 0));
    ASSERT_STR_EQ("wxyz", row(t, 1));
    vt_free(t);
    PASS();
}

static char resp_buf[128];
static size_t resp_len;
static void capture_resp(void *user, const char *bytes, size_t len) {
    (void)user;
    if (resp_len + len < sizeof resp_buf) {
        memcpy(resp_buf + resp_len, bytes, len);
        resp_len += len;
        resp_buf[resp_len] = '\0';
    }
}

TEST dsr_reports_cursor(void) {
    vt *t = vt_new(20, 5, 0);
    vt_set_respond(t, capture_resp, NULL);
    resp_len = 0;
    vt_feed(t, "\x1b[3;5H\x1b[6n", 10);
    ASSERT_STR_EQ("\x1b[3;5R", resp_buf);
    resp_len = 0;
    vt_feed(t, "\x1b[c", 3);
    ASSERT_STR_EQ("\x1b[?6c", resp_buf);
    vt_free(t);
    PASS();
}

TEST osc_title_bel_and_st(void) {
    vt *t = vt_new(10, 2, 0);
    vt_feed(t, "\x1b]0;hello\x07", 10);
    ASSERT_STR_EQ("hello", vt_title(t));
    vt_feed(t, "\x1b]2;there\x1b\\", 11);
    ASSERT_STR_EQ("there", vt_title(t));
    vt_free(t);
    PASS();
}

static char gfx_buf[512];
static size_t gfx_len;
static void capture_gfx(void *user, const char *bytes, size_t len) {
    (void)user;
    if (gfx_len + len < sizeof gfx_buf) {
        memcpy(gfx_buf + gfx_len, bytes, len);
        gfx_len += len;
        gfx_buf[gfx_len] = '\0';
    }
}

TEST kitty_apc_recorded_and_passed_through(void) {
    vt *t = vt_new(10, 2, 0);
    vt_set_graphics(t, capture_gfx, NULL);
    gfx_len = 0;
    const char *seq = "\x1b_Ga=T,f=100,m=0;QUJD\x1b\\";
    vt_feed(t, seq, strlen(seq));
    ASSERT_EQ(1, vt_graphics_count(t));
    ASSERT_STR_EQ(seq, gfx_buf);
    /* screen content untouched by graphics */
    ASSERT_STR_EQ("", row(t, 0));
    vt_free(t);
    PASS();
}

TEST resize_preserves_content(void) {
    vt *t = vt_new(10, 3, 0);
    vt_feed(t, "keep\r\nme", 8);
    vt_resize(t, 20, 5);
    ASSERT_EQ(20, vt_cols(t));
    ASSERT_EQ(5, vt_rows(t));
    ASSERT_STR_EQ("keep", row(t, 0));
    ASSERT_STR_EQ("me", row(t, 1));
    vt_resize(t, 3, 2);
    ASSERT_STR_EQ("kee", row(t, 0));
    vt_free(t);
    PASS();
}

TEST tabs_and_backspace(void) {
    vt *t = vt_new(20, 2, 0);
    vt_feed(t, "a\tb", 3);
    ASSERT_EQ(9, vt_cursor_x(t));
    const vt_cell *c = vt_line(t, 0);
    ASSERT_EQ((uint32_t)'b', c[8].cp);
    vt_feed(t, "\b\bX", 3);
    ASSERT_EQ((uint32_t)'X', vt_line(t, 0)[7].cp);
    vt_free(t);
    PASS();
}

/* The split-boundary rule from the root AGENTS.md: streaming parsers must
 * not care where reads split. Feed a sequence exercising UTF-8, CSI, SGR
 * truecolor, OSC, APC, and wide glyphs — split at every boundary — and
 * demand identical state. */
static int vt_equal(vt *a, vt *b) {
    if (vt_cols(a) != vt_cols(b) || vt_rows(a) != vt_rows(b)) return 0;
    if (vt_cursor_x(a) != vt_cursor_x(b) || vt_cursor_y(a) != vt_cursor_y(b)) return 0;
    if (strcmp(vt_title(a), vt_title(b)) != 0) return 0;
    if (vt_graphics_count(a) != vt_graphics_count(b)) return 0;
    for (int y = 0; y < vt_rows(a); y++)
        if (memcmp(vt_line(a, y), vt_line(b, y), (size_t)vt_cols(a) * sizeof(vt_cell)) != 0)
            return 0;
    return 1;
}

TEST survives_every_split_boundary(void) {
    const char seq[] = "\x1b]0;t\xc3\xaets\x07"               /* OSC title with UTF-8 */
                       "\x1b[1;38;2;12;34;56mhi \xe6\xb1\x89" /* SGR truecolor + wide */
                       "\x1b[0m\r\n\xee\x82\xb0"              /* reset, nerd glyph */
                       "\x1b_Gf=100,m=0;QQ==\x1b\\"           /* kitty APC */
                       "\x1b[2;2H\x1b[Kok\xf0\x9f\x98\x80";   /* CUP, EL, emoji */
    size_t len = sizeof seq - 1;

    vt *whole = vt_new(12, 4, 0);
    vt_feed(whole, seq, len);

    for (size_t cut = 0; cut <= len; cut++) {
        vt *split = vt_new(12, 4, 0);
        vt_feed(split, seq, cut);
        vt_feed(split, seq + cut, len - cut);
        if (!vt_equal(whole, split)) {
            vt_free(split);
            vt_free(whole);
            FAILm("split-boundary state diverged");
        }
        vt_free(split);
    }
    /* and byte-by-byte */
    vt *bytes = vt_new(12, 4, 0);
    for (size_t i = 0; i < len; i++) vt_feed(bytes, seq + i, 1);
    ASSERT(vt_equal(whole, bytes));
    vt_free(bytes);
    vt_free(whole);
    PASS();
}

TEST bracketed_paste_mode_tracked(void) {
    vt *t = vt_new(10, 2, 0);
    ASSERT_FALSE(vt_bracketed_paste(t));
    vt_feed(t, "\x1b[?2004h", 8);
    ASSERT(vt_bracketed_paste(t));
    vt_feed(t, "\x1b[?2004l", 8);
    ASSERT_FALSE(vt_bracketed_paste(t));
    vt_free(t);
    PASS();
}

SUITE(vt_suite) {
    RUN_TEST(plain_text_lands_in_cells);
    RUN_TEST(crlf_and_wrap);
    RUN_TEST(deferred_wrap_holds_last_column);
    RUN_TEST(scroll_pushes_scrollback);
    RUN_TEST(cup_ed_el);
    RUN_TEST(sgr_colors_and_attrs);
    RUN_TEST(utf8_wide_and_combining);
    RUN_TEST(nerd_font_pua_is_single_width);
    RUN_TEST(alt_screen_round_trip);
    RUN_TEST(scroll_region_stbm);
    RUN_TEST(insert_delete_lines_and_chars);
    RUN_TEST(dsr_reports_cursor);
    RUN_TEST(osc_title_bel_and_st);
    RUN_TEST(kitty_apc_recorded_and_passed_through);
    RUN_TEST(resize_preserves_content);
    RUN_TEST(tabs_and_backspace);
    RUN_TEST(survives_every_split_boundary);
    RUN_TEST(bracketed_paste_mode_tracked);
}
