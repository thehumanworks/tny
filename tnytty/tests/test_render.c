/* test_render.c — the CPU cell rasterizer (docs/adr/0005): cell-to-pixel
 * mapping, dirty-row tracking, colors, and glyph placement. The glyph
 * source is a stub here, which is the point of the seam: no CoreText, so
 * this runs on every CI host. */
#include "greatest.h"
#include "ui/render.h"
#include "vt/vt.h"

#include <stdlib.h>
#include <string.h>

#define CELL_W 8
#define CELL_H 16

/* A solid cell-sized block for every codepoint, so "did this cell get a
 * glyph" is a single pixel test. */
static uint8_t solid[CELL_W * CELL_H];

static bool stub_glyph(void *user, uint32_t cp, uint16_t attrs, tt_glyph *out) {
    (void)user;
    (void)attrs;
    if (cp == ' ' || cp == 0) return false;
    out->alpha = solid;
    out->w = CELL_W;
    out->h = CELL_H;
    out->left = 0;
    out->top = 0;
    return true;
}

static tt_render_config base_cfg(void) {
    tt_config defaults;
    tt_config_defaults(&defaults);
    tt_render_config c;
    memset(&c, 0, sizeof c);
    memcpy(c.palette, defaults.palette, sizeof c.palette);
    c.bold_brightens = defaults.bold_brightens;
    c.cell_w = CELL_W;
    c.cell_h = CELL_H;
    c.pad_left = c.pad_right = c.pad_bottom = 4;
    c.pad_top = 30; /* the transparent-titlebar inset */
    c.fg = 0xffffff;
    c.bg = 0x000000;
    c.glyph = stub_glyph;
    return c;
}

static uint32_t px_at(tt_render *r, int x, int y) {
    return tt_render_pixels(r)[(size_t)y * (size_t)tt_render_width(r) + (size_t)x] & 0xffffffu;
}

TEST grid_fits_inside_the_padding(void) {
    tt_render_config c = base_cfg();
    int cols = 0, rows = 0;
    /* 4 + 10*8 + 4 wide, 30 + 5*16 + 4 tall, plus slack that must not
     * buy an extra cell. */
    tt_render_grid_for(&c, 4 + 10 * CELL_W + 4 + 7, 30 + 5 * CELL_H + 4 + 15, &cols, &rows);
    ASSERT_EQ(10, cols);
    ASSERT_EQ(5, rows);
    /* A surface smaller than one cell still reports a 1x1 grid. */
    tt_render_grid_for(&c, 1, 1, &cols, &rows);
    ASSERT_EQ(1, cols);
    ASSERT_EQ(1, rows);
    PASS();
}

TEST cell_origin_offsets_by_padding_and_titlebar(void) {
    tt_render_config c = base_cfg();
    int x = 0, y = 0;
    tt_render_cell_origin(&c, 0, 0, &x, &y);
    ASSERT_EQ(4, x);
    ASSERT_EQ(30, y); /* row 0 starts below the traffic lights */
    tt_render_cell_origin(&c, 3, 2, &x, &y);
    ASSERT_EQ(4 + 3 * CELL_W, x);
    ASSERT_EQ(30 + 2 * CELL_H, y);
    PASS();
}

TEST colors_resolve_by_tag(void) {
    tt_render_config c = base_cfg();
    ASSERT_EQ(0x123456u, tt_render_color(&c, VT_COLOR_DEFAULT, 0x123456));
    ASSERT_EQ(0xaabbccu, tt_render_color(&c, VT_COLOR_RGB | 0xaabbcc, 0x000000));
    /* 0..15 come from the configurable palette... */
    ASSERT_EQ(c.palette[4], tt_render_color(&c, VT_COLOR_IDX | 4, 0xffffff));
    ASSERT_EQ(c.palette[12], tt_render_color(&c, VT_COLOR_IDX | 12, 0xffffff));
    /* ...16..255 are the fixed xterm cube and grayscale ramp. */
    ASSERT_EQ(0x0000ffu, tt_render_color(&c, VT_COLOR_IDX | 21, 0));  /* cube 5,0,0 -> blue */
    ASSERT_EQ(0x080808u, tt_render_color(&c, VT_COLOR_IDX | 232, 0)); /* first gray */
    PASS();
}

TEST first_frame_paints_everything_then_idles(void) {
    tt_render_config c = base_cfg();
    tt_render *r = tt_render_new(&c, 4 + 10 * CELL_W + 4, 30 + 4 * CELL_H + 4);
    vt *t = vt_new(tt_render_cols(r), tt_render_rows(r), 0);
    ASSERT_EQ(10, tt_render_cols(r));
    ASSERT_EQ(4, tt_render_rows(r));

    int y0 = -1, y1 = -1;
    ASSERT_EQ(4, tt_render_frame(r, t, true, &y0, &y1));
    ASSERT_EQ(0, y0);
    ASSERT_EQ(tt_render_height(r), y1);

    /* Nothing moved: no rows, no band, so the window presents nothing. */
    ASSERT_EQ(0, tt_render_frame(r, t, true, &y0, &y1));
    ASSERT_EQ(0, y1 - y0);
    vt_free(t);
    tt_render_free(r);
    PASS();
}

TEST only_changed_rows_repaint(void) {
    tt_render_config c = base_cfg();
    tt_render *r = tt_render_new(&c, 4 + 10 * CELL_W + 4, 30 + 4 * CELL_H + 4);
    vt *t = vt_new(10, 4, 0);
    tt_render_frame(r, t, true, NULL, NULL);

    /* Write on row 2 without moving the cursor off it afterwards: row 0
     * (cursor was there) and row 2 (cursor + new text) are the dirt. */
    vt_feed(t, "\x1b[3;1Hxy", 8);
    int y0 = 0, y1 = 0;
    ASSERT_EQ(2, tt_render_frame(r, t, true, &y0, &y1));
    ASSERT_EQ(30 + 0 * CELL_H, y0);
    ASSERT_EQ(30 + 3 * CELL_H, y1);

    /* Typing one more character keeps the caret on row 2: one row. */
    vt_feed(t, "z", 1);
    ASSERT_EQ(1, tt_render_frame(r, t, true, &y0, &y1));
    ASSERT_EQ(30 + 2 * CELL_H, y0);
    ASSERT_EQ(30 + 3 * CELL_H, y1);
    vt_free(t);
    tt_render_free(r);
    PASS();
}

TEST losing_focus_repaints_only_the_caret_row(void) {
    tt_render_config c = base_cfg();
    tt_render *r = tt_render_new(&c, 4 + 10 * CELL_W + 4, 30 + 4 * CELL_H + 4);
    vt *t = vt_new(10, 4, 0);
    tt_render_frame(r, t, true, NULL, NULL);
    ASSERT_EQ(1, tt_render_frame(r, t, false, NULL, NULL));
    ASSERT_EQ(0, tt_render_frame(r, t, false, NULL, NULL));
    vt_free(t);
    tt_render_free(r);
    PASS();
}

TEST glyphs_and_backgrounds_land_on_the_right_pixels(void) {
    tt_render_config c = base_cfg();
    tt_render *r = tt_render_new(&c, 4 + 10 * CELL_W + 4, 30 + 4 * CELL_H + 4);
    vt *t = vt_new(10, 4, 0);
    /* Red on blue in cell (0,0), then a plain space in cell (1,0). */
    vt_feed(t, "\x1b[38;2;255;0;0m\x1b[48;2;0;0;255mA ", 32);
    tt_render_frame(r, t, false, NULL, NULL);

    int x0 = 0, y0 = 0;
    tt_render_cell_origin(&c, 0, 0, &x0, &y0);
    ASSERT_EQ(0xff0000u, px_at(r, x0 + 1, y0 + 1)); /* glyph mask -> fg */
    int x1 = 0;
    tt_render_cell_origin(&c, 1, 0, &x1, NULL);
    ASSERT_EQ(0x0000ffu, px_at(r, x1 + 1, y0 + 1)); /* space -> bg only */
    /* Padding stays the default background. */
    ASSERT_EQ(0x000000u, px_at(r, 0, 0));
    ASSERT_EQ(0x000000u, px_at(r, 0, y0 + 1));
    vt_free(t);
    tt_render_free(r);
    PASS();
}

TEST reverse_video_swaps_fg_and_bg(void) {
    tt_render_config c = base_cfg();
    tt_render *r = tt_render_new(&c, 4 + 10 * CELL_W + 4, 30 + 4 * CELL_H + 4);
    vt *t = vt_new(10, 4, 0);
    vt_feed(t, "\x1b[7mA", 5);
    tt_render_frame(r, t, false, NULL, NULL);
    int x = 0, y = 0;
    tt_render_cell_origin(&c, 0, 0, &x, &y);
    ASSERT_EQ(0x000000u, px_at(r, x + 1, y + 1)); /* glyph painted in bg */
    vt_free(t);
    tt_render_free(r);
    PASS();
}

TEST resize_reflows_the_grid_and_forces_a_full_paint(void) {
    tt_render_config c = base_cfg();
    tt_render *r = tt_render_new(&c, 4 + 10 * CELL_W + 4, 30 + 4 * CELL_H + 4);
    vt *t = vt_new(10, 4, 0);
    tt_render_frame(r, t, true, NULL, NULL);
    ASSERT_EQ(0, tt_render_resize(r, 4 + 20 * CELL_W + 4, 30 + 6 * CELL_H + 4));
    ASSERT_EQ(20, tt_render_cols(r));
    ASSERT_EQ(6, tt_render_rows(r));
    vt_resize(t, 20, 6);
    ASSERT_EQ(6, tt_render_frame(r, t, true, NULL, NULL));
    tt_render_invalidate(r);
    ASSERT_EQ(6, tt_render_frame(r, t, true, NULL, NULL));
    vt_free(t);
    tt_render_free(r);
    PASS();
}

/* Nerd-font glyphs are one cell in the model (ADR 0004) but may overhang
 * when drawn; the blend must clip to the surface, not scribble past it
 * (ASan in the debug build is the second half of this assertion). */
static uint8_t wide_mask[CELL_W * 3 * CELL_H];

static bool wide_glyph(void *user, uint32_t cp, uint16_t attrs, tt_glyph *out) {
    (void)user;
    (void)attrs;
    if (cp == ' ' || cp == 0) return false;
    out->alpha = wide_mask;
    out->w = CELL_W * 3;
    out->h = CELL_H;
    out->left = -CELL_W; /* one cell of overhang on each side */
    out->top = 0;
    return true;
}

/* One cell wide, but tall enough to spill into the rows above and
 * below -- the vertical counterpart of the overhang above. */
static bool tall_glyph(void *user, uint32_t cp, uint16_t attrs, tt_glyph *out) {
    (void)user;
    (void)attrs;
    if (cp == ' ' || cp == 0) return false;
    out->alpha = wide_mask;
    out->w = CELL_W;
    out->h = CELL_H * 2;
    out->left = 0;
    out->top = -CELL_H / 2;
    return true;
}

TEST overhanging_glyphs_clip_to_the_surface(void) {
    memset(wide_mask, 0xff, sizeof wide_mask);
    tt_render_config c = base_cfg();
    c.glyph = wide_glyph;
    tt_render *r = tt_render_new(&c, 4 + 3 * CELL_W + 4, 30 + 2 * CELL_H + 4);
    vt *t = vt_new(3, 2, 0);
    ASSERT_EQ(1, vt_cp_width(0xe0a0)); /* PUA is width 1 (ADR 0004) */
    vt_feed(t, "\xee\x82\xa0", 3);     /* U+E0A0 in cell (0,0) */
    tt_render_frame(r, t, false, NULL, NULL);

    int x = 0, y = 0;
    tt_render_cell_origin(&c, 0, 0, &x, &y);
    /* Overhang reaches into the left padding... */
    ASSERT_EQ(0xffffffu, px_at(r, x - 1, y + 1));
    /* ...and past column 0 into the next cell. */
    ASSERT_EQ(0xffffffu, px_at(r, x + CELL_W + 1, y + 1));
    /* ...but never above the first row, and never off the surface. */
    ASSERT_EQ(0x000000u, px_at(r, 0, 0));
    vt_free(t);
    tt_render_free(r);
    PASS();
}

/* SGR 1 on an indexed 0..7 foreground picks the bright half, which is
 * contrast-checked in test_config.c -- the readable-bold-blue fix. */
TEST bold_brightens_indexed_foregrounds(void) {
    tt_render_config c = base_cfg();
    tt_render *r = tt_render_new(&c, 4 + 10 * CELL_W + 4, 30 + 4 * CELL_H + 4);
    vt *t = vt_new(10, 4, 0);
    vt_feed(t, "\x1b[1;34mA", 8); /* bold blue */
    tt_render_frame(r, t, false, NULL, NULL);
    int x = 0, y = 0;
    tt_render_cell_origin(&c, 0, 0, &x, &y);
    ASSERT_EQ(c.palette[12], px_at(r, x + 1, y + 1)); /* bright blue, not palette[4] */
    ASSERT(c.palette[12] != c.palette[4]);

    /* With bold-brightens off, bold keeps the base color (the bold face
     * still carries the weight -- the glyph lookup sees the attr). */
    c.bold_brightens = false;
    tt_render_configure(r, &c);
    tt_render_frame(r, t, false, NULL, NULL);
    ASSERT_EQ(c.palette[4], px_at(r, x + 1, y + 1));
    vt_free(t);
    tt_render_free(r);
    PASS();
}

TEST truecolor_and_default_foregrounds_ignore_bold_brightening(void) {
    tt_render_config c = base_cfg();
    tt_render *r = tt_render_new(&c, 4 + 10 * CELL_W + 4, 30 + 4 * CELL_H + 4);
    vt *t = vt_new(10, 4, 0);
    vt_feed(t, "\x1b[1;38;2;18;52;86mA", 19);
    tt_render_frame(r, t, false, NULL, NULL);
    int x = 0, y = 0;
    tt_render_cell_origin(&c, 0, 0, &x, &y);
    ASSERT_EQ(0x123456u, px_at(r, x + 1, y + 1));
    vt_free(t);
    tt_render_free(r);
    PASS();
}

TEST selection_inverts_its_cells_and_dirties_only_those_rows(void) {
    tt_render_config c = base_cfg();
    tt_render *r = tt_render_new(&c, 4 + 10 * CELL_W + 4, 30 + 4 * CELL_H + 4);
    vt *t = vt_new(10, 4, 0);
    vt_feed(t, "ab\r\ncd", 6);
    tt_render_frame(r, t, false, NULL, NULL);

    tt_selection sel;
    tt_sel_begin(&sel, t, 0, 0, 1);
    tt_sel_extend(&sel, t, 2, 0); /* 'a', 'b', and one blank cell */
    tt_render_set_selection(r, &sel);
    /* Only row 0 carries the highlight, so only row 0 repaints. */
    ASSERT_EQ(1, tt_render_frame(r, t, false, NULL, NULL));
    int row0 = -1, row1 = -1;
    tt_render_dirty_rows(r, &row0, &row1);
    ASSERT_EQ(0, row0);
    ASSERT_EQ(1, row1);

    int x = 0, y = 0;
    tt_render_cell_origin(&c, 0, 0, &x, &y);
    /* Inverted: glyph ink becomes the background color, and a selected
     * blank cell is filled with the foreground. */
    ASSERT_EQ(c.bg, px_at(r, x + 1, y + 1));
    int blank_x = 0;
    tt_render_cell_origin(&c, 2, 0, &blank_x, NULL);
    ASSERT_EQ(c.fg, px_at(r, blank_x + 1, y + 1));

    /* Clearing it repaints the same row and nothing else. */
    tt_render_set_selection(r, NULL);
    ASSERT_EQ(1, tt_render_frame(r, t, false, NULL, NULL));
    ASSERT_EQ(c.fg, px_at(r, x + 1, y + 1));
    vt_free(t);
    tt_render_free(r);
    PASS();
}

TEST selection_spans_rows_and_repaints_each(void) {
    tt_render_config c = base_cfg();
    tt_render *r = tt_render_new(&c, 4 + 10 * CELL_W + 4, 30 + 4 * CELL_H + 4);
    vt *t = vt_new(10, 4, 0);
    vt_feed(t, "ab\r\ncd\r\nef", 10);
    tt_render_frame(r, t, false, NULL, NULL);
    tt_selection sel;
    tt_sel_begin(&sel, t, 0, 0, 1);
    tt_sel_extend(&sel, t, 1, 2);
    tt_render_set_selection(r, &sel);
    ASSERT_EQ(3, tt_render_frame(r, t, false, NULL, NULL));
    int row0 = 0, row1 = 0;
    tt_render_dirty_rows(r, &row0, &row1);
    ASSERT_EQ(0, row0);
    ASSERT_EQ(3, row1);
    /* Holding it steady repaints nothing. */
    ASSERT_EQ(0, tt_render_frame(r, t, false, NULL, NULL));
    vt_free(t);
    tt_render_free(r);
    PASS();
}

/* An overhanging glyph paints into the neighbouring band; repainting the
 * neighbour alone would slice the overhang in half, so the row that owns
 * the ink has to come along. */
TEST overhang_pulls_the_neighbouring_row_into_the_repaint(void) {
    memset(wide_mask, 0xff, sizeof wide_mask);
    tt_render_config c = base_cfg();
    c.glyph = tall_glyph;
    tt_render *r = tt_render_new(&c, 4 + 4 * CELL_W + 4, 30 + 4 * CELL_H + 4);
    vt *t = vt_new(4, 4, 0);
    vt_feed(t, "\x1b[?25l", 6);  /* hide the caret: it would dirty rows of its own */
    vt_feed(t, "\x1b[2;1HX", 7); /* row 1 holds the tall glyph */
    tt_render_frame(r, t, false, NULL, NULL);

    /* Touch row 2 only; row 1's ink reaches into it, so both repaint. */
    vt_feed(t, "\x1b[3;1Hy", 7);
    ASSERT_EQ(2, tt_render_frame(r, t, false, NULL, NULL));
    int row0 = 0, row1 = 0;
    tt_render_dirty_rows(r, &row0, &row1);
    ASSERT_EQ(1, row0);
    ASSERT_EQ(3, row1);
    vt_free(t);
    tt_render_free(r);
    PASS();
}

/* The bar is not part of the grid: its height comes out of the rows, so
 * the session never draws under it. */
TEST status_bar_height_comes_out_of_the_grid(void) {
    tt_render_config c = base_cfg();
    int cols = 0, rows = 0;
    int px_h = 30 + 6 * CELL_H + 4;
    tt_render_grid_for(&c, 4 + 10 * CELL_W + 4, px_h, &cols, &rows);
    ASSERT_EQ(6, rows);
    c.status_h = CELL_H + 4;
    tt_render_grid_for(&c, 4 + 10 * CELL_W + 4, px_h, &cols, &rows);
    /* 6 rows of pixels minus a 20px bar leaves room for 4 whole cells. */
    ASSERT_EQ(4, rows);
    ASSERT_EQ(10, cols);
    PASS();
}

TEST status_bar_paints_only_when_its_text_changes(void) {
    tt_render_config c = base_cfg();
    c.status_h = CELL_H + 4;
    c.status_bg = 0x202430;
    c.status_fg = 0x9aa0ad;
    int px_h = 30 + 4 * CELL_H + 4 + c.status_h;
    tt_render *r = tt_render_new(&c, 4 + 10 * CELL_W + 4, px_h);
    vt *t = vt_new(tt_render_cols(r), tt_render_rows(r), 0);
    ASSERT_EQ(4, tt_render_rows(r));

    tt_render_frame(r, t, false, NULL, NULL);
    /* The bar band is painted in its own background from the first frame. */
    int bar_y = tt_render_height(r) - c.status_h;
    ASSERT_EQ(c.status_bg, px_at(r, 1, bar_y + 1));
    /* Idle: nothing repaints, bar included. */
    ASSERT_EQ(0, tt_render_frame(r, t, false, NULL, NULL));

    /* A message repaints the bar and extends the presented band to the
     * bottom of the surface. */
    tt_render_set_status(r, "Copied 3 characters");
    int y0 = 0, y1 = 0;
    ASSERT_EQ(1, tt_render_frame(r, t, false, &y0, &y1));
    ASSERT_EQ(tt_render_height(r), y1);
    ASSERT(y0 >= bar_y);
    /* The stub glyph is a solid block, so the first character's cell is
     * now the status foreground. */
    ASSERT_EQ(c.status_fg, px_at(r, c.pad_left + 1, bar_y + (c.status_h - CELL_H) / 2 + 1));
    /* Setting the same text again changes nothing. */
    ASSERT_EQ(0, tt_render_frame(r, t, false, NULL, NULL));

    /* Clearing it wipes the bar back to its background. */
    tt_render_set_status(r, "");
    ASSERT_EQ(1, tt_render_frame(r, t, false, NULL, NULL));
    ASSERT_EQ(c.status_bg, px_at(r, c.pad_left + 1, bar_y + (c.status_h - CELL_H) / 2 + 1));
    vt_free(t);
    tt_render_free(r);
    PASS();
}

TEST status_bar_is_absent_when_its_height_is_zero(void) {
    tt_render_config c = base_cfg();
    ASSERT_EQ(0, c.status_h);
    tt_render *r = tt_render_new(&c, 4 + 10 * CELL_W + 4, 30 + 4 * CELL_H + 4);
    vt *t = vt_new(10, 4, 0);
    tt_render_frame(r, t, false, NULL, NULL);
    tt_render_set_status(r, "Copied 9 characters");
    ASSERT_EQ(0, tt_render_frame(r, t, false, NULL, NULL)); /* nothing to draw it on */
    vt_free(t);
    tt_render_free(r);
    PASS();
}

TEST mix_blends_endpoints(void) {
    ASSERT_EQ(0x000000u, tt_render_mix(0x000000, 0xffffff, 0));
    ASSERT_EQ(0xffffffu, tt_render_mix(0x000000, 0xffffff, 255));
    ASSERT_EQ(0x808080u, tt_render_mix(0x000000, 0xffffff, 128));
    PASS();
}

SUITE(render_suite) {
    memset(solid, 0xff, sizeof solid);
    RUN_TEST(grid_fits_inside_the_padding);
    RUN_TEST(cell_origin_offsets_by_padding_and_titlebar);
    RUN_TEST(colors_resolve_by_tag);
    RUN_TEST(first_frame_paints_everything_then_idles);
    RUN_TEST(only_changed_rows_repaint);
    RUN_TEST(losing_focus_repaints_only_the_caret_row);
    RUN_TEST(glyphs_and_backgrounds_land_on_the_right_pixels);
    RUN_TEST(reverse_video_swaps_fg_and_bg);
    RUN_TEST(resize_reflows_the_grid_and_forces_a_full_paint);
    RUN_TEST(overhanging_glyphs_clip_to_the_surface);
    RUN_TEST(overhang_pulls_the_neighbouring_row_into_the_repaint);
    RUN_TEST(bold_brightens_indexed_foregrounds);
    RUN_TEST(truecolor_and_default_foregrounds_ignore_bold_brightening);
    RUN_TEST(selection_inverts_its_cells_and_dirties_only_those_rows);
    RUN_TEST(selection_spans_rows_and_repaints_each);
    RUN_TEST(status_bar_height_comes_out_of_the_grid);
    RUN_TEST(status_bar_paints_only_when_its_text_changes);
    RUN_TEST(status_bar_is_absent_when_its_height_is_zero);
    RUN_TEST(mix_blends_endpoints);
}
