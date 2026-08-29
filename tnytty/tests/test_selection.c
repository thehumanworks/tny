/* test_selection.c — mouse selection over the grid (docs/adr/0005):
 * ranges, word boundaries, wide-cell rounding, text extraction. All
 * platform-free, so it runs wherever the suite runs. */
#include "greatest.h"
#include "ui/selection.h"
#include "vt/vt.h"

#include <string.h>

static char text_buf[512];

static const char *sel_text(const tt_selection *s, const vt *t) {
    tt_sel_text(s, t, text_buf, sizeof text_buf);
    return text_buf;
}

static vt *grid(const char *feed, int cols, int rows) {
    vt *t = vt_new(cols, rows, 0);
    vt_feed(t, feed, strlen(feed));
    return t;
}

TEST drag_selects_a_character_range(void) {
    vt *t = grid("hello world", 20, 3);
    tt_selection s;
    tt_sel_begin(&s, t, 0, 0, 1);
    tt_sel_extend(&s, t, 4, 0);
    ASSERT(tt_sel_active(&s));
    ASSERT_STR_EQ("hello", sel_text(&s, t));
    int c0 = 0, c1 = 0;
    ASSERT(tt_sel_row_span(&s, 0, 20, &c0, &c1));
    ASSERT_EQ(0, c0);
    ASSERT_EQ(5, c1);
    ASSERT(!tt_sel_row_span(&s, 1, 20, &c0, &c1));
    vt_free(t);
    PASS();
}

TEST dragging_backwards_normalizes(void) {
    vt *t = grid("hello world", 20, 3);
    tt_selection s;
    tt_sel_begin(&s, t, 10, 0, 1);
    tt_sel_extend(&s, t, 6, 0);
    ASSERT_STR_EQ("world", sel_text(&s, t));
    vt_free(t);
    PASS();
}

TEST multi_row_selection_joins_with_newlines(void) {
    vt *t = grid("one\r\ntwo\r\nthree", 20, 4);
    tt_selection s;
    tt_sel_begin(&s, t, 1, 0, 1);
    tt_sel_extend(&s, t, 2, 2);
    ASSERT_STR_EQ("ne\ntwo\nthr", sel_text(&s, t));
    vt_free(t);
    PASS();
}

/* Every row is padded to the grid width; the padding is not content. */
TEST trailing_blanks_are_trimmed_per_row(void) {
    vt *t = grid("ab\r\ncd", 20, 3);
    tt_selection s;
    tt_sel_begin(&s, t, 0, 0, 3); /* triple click: whole lines */
    tt_sel_extend(&s, t, 0, 1);
    ASSERT_STR_EQ("ab\ncd", sel_text(&s, t));
    vt_free(t);
    PASS();
}

TEST double_click_selects_a_word(void) {
    vt *t = grid("run ~/src/main.c now", 30, 2);
    tt_selection s;
    tt_sel_begin(&s, t, 1, 0, 2);
    ASSERT_STR_EQ("run", sel_text(&s, t));
    /* Path characters are word material, so a path selects whole. */
    tt_sel_begin(&s, t, 8, 0, 2);
    ASSERT_STR_EQ("~/src/main.c", sel_text(&s, t));
    vt_free(t);
    PASS();
}

/* A separator is its own one-cell "word". Its text comes out empty
 * because trailing blanks are trimmed off every row -- copying a run of
 * spaces should not put whitespace on the pasteboard -- but interior
 * spaces inside a wider selection survive. */
TEST double_click_on_a_separator_selects_just_it(void) {
    vt *t = grid("a b c", 10, 2);
    tt_selection s;
    tt_sel_begin(&s, t, 1, 0, 2);
    int c0 = 0, c1 = 0;
    ASSERT(tt_sel_row_span(&s, 0, 10, &c0, &c1));
    ASSERT_EQ(1, c0);
    ASSERT_EQ(2, c1);
    ASSERT_STR_EQ("", sel_text(&s, t));
    tt_sel_begin(&s, t, 0, 0, 1);
    tt_sel_extend(&s, t, 4, 0);
    ASSERT_STR_EQ("a b c", sel_text(&s, t));
    vt_free(t);
    PASS();
}

TEST word_boundaries_report_half_open_columns(void) {
    vt *t = grid("ab cd", 10, 2);
    int c0 = 0, c1 = 0;
    tt_sel_word_at(t, 4, 0, &c0, &c1);
    ASSERT_EQ(3, c0);
    ASSERT_EQ(5, c1);
    tt_sel_word_at(t, 0, 0, &c0, &c1);
    ASSERT_EQ(0, c0);
    ASSERT_EQ(2, c1);
    vt_free(t);
    PASS();
}

TEST triple_click_takes_the_whole_row(void) {
    vt *t = grid("first\r\nsecond", 20, 3);
    tt_selection s;
    tt_sel_begin(&s, t, 3, 1, 3);
    ASSERT_STR_EQ("second", sel_text(&s, t));
    int c0 = 0, c1 = 0;
    ASSERT(tt_sel_row_span(&s, 1, 20, &c0, &c1));
    ASSERT_EQ(0, c0);
    ASSERT_EQ(20, c1);
    vt_free(t);
    PASS();
}

/* A wide glyph is a lead cell plus a continuation cell; a selection must
 * never cut one in half or emit the continuation as a second character. */
TEST wide_cells_round_to_whole_glyphs(void) {
    vt *t = grid("\xe6\xbc\xa2x", 10, 2); /* U+6F22, width 2, then 'x' */
    ASSERT_EQ(2, vt_cp_width(0x6f22));
    tt_selection s;
    /* Clicking the continuation cell selects the whole glyph. */
    tt_sel_begin(&s, t, 1, 0, 1);
    ASSERT_STR_EQ("\xe6\xbc\xa2", sel_text(&s, t));
    int c0 = 0, c1 = 0;
    ASSERT(tt_sel_row_span(&s, 0, 10, &c0, &c1));
    ASSERT_EQ(0, c0);
    ASSERT_EQ(2, c1);
    /* Starting on the lead cell includes the continuation too. */
    tt_sel_begin(&s, t, 0, 0, 1);
    tt_sel_extend(&s, t, 2, 0);
    ASSERT_STR_EQ("\xe6\xbc\xa2x", sel_text(&s, t));
    vt_free(t);
    PASS();
}

TEST a_click_without_a_drag_clears_the_selection(void) {
    vt *t = grid("hi", 10, 2);
    tt_selection s;
    /* Clicking blank space selects nothing and dismisses. */
    tt_sel_begin(&s, t, 5, 1, 1);
    ASSERT(!tt_sel_finish(&s, t));
    ASSERT(!tt_sel_active(&s));
    /* Clicking on a character is a one-cell selection, which stands. */
    tt_sel_begin(&s, t, 0, 0, 1);
    ASSERT(tt_sel_finish(&s, t));
    ASSERT_STR_EQ("h", sel_text(&s, t));
    vt_free(t);
    PASS();
}

TEST text_reports_the_length_a_buffer_needs(void) {
    vt *t = grid("one\r\ntwo", 10, 3);
    tt_selection s;
    tt_sel_begin(&s, t, 0, 0, 3);
    tt_sel_extend(&s, t, 0, 1);
    size_t need = tt_sel_text(&s, t, NULL, 0);
    ASSERT_EQ(strlen("one\ntwo"), need);
    char small[4] = {0};
    tt_sel_text(&s, t, small, sizeof small);
    ASSERT_EQ('\0', small[3]); /* never overruns, always terminates */
    vt_free(t);
    PASS();
}

TEST clearing_and_empty_selections_are_inert(void) {
    vt *t = grid("hi", 10, 2);
    tt_selection s;
    tt_sel_clear(&s);
    ASSERT(!tt_sel_active(&s));
    ASSERT_EQ(0u, tt_sel_text(&s, t, text_buf, sizeof text_buf));
    int c0 = 0, c1 = 0;
    ASSERT(!tt_sel_row_span(&s, 0, 10, &c0, &c1));
    tt_sel_extend(&s, t, 3, 0); /* extending nothing stays nothing */
    ASSERT(!tt_sel_active(&s));
    vt_free(t);
    PASS();
}

TEST clicks_beyond_the_grid_clamp(void) {
    vt *t = grid("hi", 10, 2);
    tt_selection s;
    tt_sel_begin(&s, t, 999, 999, 1);
    tt_sel_extend(&s, t, -5, -5);
    int c0 = 0, c1 = 0;
    ASSERT(tt_sel_row_span(&s, 0, 10, &c0, &c1));
    vt_free(t);
    PASS();
}

SUITE(selection_suite) {
    RUN_TEST(drag_selects_a_character_range);
    RUN_TEST(dragging_backwards_normalizes);
    RUN_TEST(multi_row_selection_joins_with_newlines);
    RUN_TEST(trailing_blanks_are_trimmed_per_row);
    RUN_TEST(double_click_selects_a_word);
    RUN_TEST(double_click_on_a_separator_selects_just_it);
    RUN_TEST(word_boundaries_report_half_open_columns);
    RUN_TEST(triple_click_takes_the_whole_row);
    RUN_TEST(wide_cells_round_to_whole_glyphs);
    RUN_TEST(a_click_without_a_drag_clears_the_selection);
    RUN_TEST(text_reports_the_length_a_buffer_needs);
    RUN_TEST(clearing_and_empty_selections_are_inert);
    RUN_TEST(clicks_beyond_the_grid_clamp);
}
