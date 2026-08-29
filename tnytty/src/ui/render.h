/* render.h — CPU cell rasterizer (docs/adr/0005).
 *
 * Reads the headless VT grid (docs/adr/0001) and paints it into an RGBA
 * framebuffer, redrawing only rows whose cells changed. Platform-free:
 * glyph coverage masks arrive through a caller-supplied lookup, so this
 * file rasterizes identically on macOS and in the Linux unit suite. */
#ifndef TNYTTY_UI_RENDER_H
#define TNYTTY_UI_RENDER_H

#include "ui/layout.h"
#include "ui/selection.h"
#include "ui/status.h"
#include "util/config.h"
#include "vt/vt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One glyph's 8-bit coverage mask. left/top are device-pixel offsets
 * from the top-left of the cell and may be negative or push past the
 * cell width: nerd-font glyphs are one cell wide by model (ADR 0004) but
 * are allowed to overhang when drawn. */
typedef struct {
    const uint8_t *alpha;
    int w, h;
    int left, top;
} tt_glyph;

/* Fill out for (cp, attrs) — attrs carries VT_ATTR_BOLD/ITALIC so the
 * platform can pick a face. Returns false when nothing should be drawn. */
typedef bool (*tt_glyph_fn)(void *user, uint32_t cp, uint16_t attrs, tt_glyph *out);

typedef struct {
    int cell_w, cell_h; /* device pixels; must be >= 1 */
    int pad_left, pad_top, pad_right, pad_bottom;
    uint32_t fg, bg;                  /* default colors, 0x00RRGGBB */
    uint32_t palette[TT_PALETTE_LEN]; /* SGR indices 0..15 */
    bool bold_brightens;              /* bold on an indexed 0..7 fg picks 8..15 */
    /* One-line status bar along the bottom edge (docs/config.md). Its
     * height is in device pixels and comes out of the grid area, so the
     * session's rows shrink to fit; 0 hides it entirely. */
    int status_h;
    uint32_t status_fg, status_bg;
    tt_glyph_fn glyph;
    void *glyph_user;
} tt_render_config;

typedef struct tt_render tt_render;

/* A framebuffer several rasterizers can share: with split panes
 * (docs/adr/0006) every pane paints into its own rectangle of the one
 * bitmap the window blits. Row-major 0xAARRGGBB, `w` pixels per row. */
typedef struct {
    uint32_t *px;
    int w, h;
} tt_fb;

/* Allocate/free a framebuffer's pixels. tt_fb_new zeroes them. */
int tt_fb_alloc(tt_fb *fb, int w, int h);
void tt_fb_free(tt_fb *fb);
/* Solid rectangle, clipped to the framebuffer. Used for the divider
 * rules between panes and the blank ground behind them. */
void tt_fb_fill(const tt_fb *fb, int x, int y, int w, int h, uint32_t rgb);

/* Cells that fit a surface of px_w x px_h device pixels (>= 1 each). */
void tt_render_grid_for(const tt_render_config *cfg, int px_w, int px_h, int *cols, int *rows);
/* Top-left device pixel of a cell. Pure; no clamping. */
void tt_render_cell_origin(const tt_render_config *cfg, int col, int row, int *x, int *y);
/* Resolve a tagged vt color (VT_COLOR_*) to 0x00RRGGBB. Indices 0..15
 * come from cfg->palette; 16..255 are the xterm cube and grayscale. */
uint32_t tt_render_color(const tt_render_config *cfg, uint32_t color, uint32_t dflt);
/* Linear blend of two 0x00RRGGBB colors; t = 0 gives a, 255 gives b. */
uint32_t tt_render_mix(uint32_t a, uint32_t b, unsigned t);
/* Inverse of tt_render_cell_origin, clamped into a cols x rows grid. */
void tt_render_cell_at(const tt_render_config *cfg, int px, int py, int cols, int rows, int *col,
                       int *row);
/* Same mapping, but says whether the point is actually on the grid: a
 * point in the padding, in the titlebar inset or past the last row or
 * column returns false, with the clamped cell still written out. A press
 * outside the grid must not start a selection -- clamping one onto the
 * last cell is what painted a phantom caret in the corner (ADR 0006). */
bool tt_render_cell_hit(const tt_render_config *cfg, int px, int py, int cols, int rows, int *col,
                        int *row);

/* A rasterizer that owns a framebuffer of its own (one pane filling the
 * window; also what the unit suite uses). */
tt_render *tt_render_new(const tt_render_config *cfg, int px_w, int px_h);
/* A rasterizer for one pane: it paints into `area` of the caller's
 * framebuffer, which must outlive it. Cell (0,0) sits at the area's
 * top-left plus the config padding, so every geometry helper below stays
 * pane-relative and single-pane behaviour is unchanged. */
tt_render *tt_render_new_in(const tt_render_config *cfg, const tt_fb *fb, tt_rect area);
/* Move/resize a pane rasterizer inside its framebuffer. Recomputes the
 * grid and forces a full repaint. Returns 0 or -1 (OOM). */
int tt_render_set_area(tt_render *r, tt_rect area);
tt_rect tt_render_area(const tt_render *r);
void tt_render_free(tt_render *r);
/* Change metrics/colors/glyph source. Forces a full redraw. */
int tt_render_configure(tt_render *r, const tt_render_config *cfg);
/* Resize the framebuffer. Forces a full redraw. Returns 0 or -1 (OOM). */
int tt_render_resize(tt_render *r, int px_w, int px_h);
void tt_render_invalidate(tt_render *r);
/* Highlight this selection on the next frames; NULL clears it. Rows
 * whose highlighted span changed become dirty. */
void tt_render_set_selection(tt_render *r, const tt_selection *sel);
/* Text for the status bar; NULL or "" leaves it blank. The bar is
 * repainted only when the text actually changes. */
void tt_render_set_status(tt_render *r, const char *text);
/* Cell rows [*row0, *row1) painted by the last tt_render_frame. */
void tt_render_dirty_rows(const tt_render *r, int *row0, int *row1);

int tt_render_cols(const tt_render *r);
int tt_render_rows(const tt_render *r);
/* The framebuffer's size, not the pane's: what the window blits. */
int tt_render_width(const tt_render *r);
int tt_render_height(const tt_render *r);
/* Row-major 0xAARRGGBB (alpha always 0xff), tt_render_width() per row. */
const uint32_t *tt_render_pixels(const tt_render *r);

/* Paint the one-line status bar along the bottom edge of `fb` using
 * cfg->status_h/status_fg/status_bg and cfg->glyph. Split panes leave the
 * bar to the window (it belongs to no pane), so it is exported. */
void tt_render_status_bar(const tt_fb *fb, const tt_render_config *cfg, const char *text);

/* Paint every dirty row of t, plus the status bar when its text
 * changed. Returns the number of lines painted (the bar counts as one);
 * *y0 and *y1 (may be NULL) get the touched device-pixel band in
 * framebuffer coordinates, y1 exclusive. Returns 0 and an empty band
 * when nothing changed. */
int tt_render_frame(tt_render *r, const vt *t, bool focused, int *y0, int *y1);

#endif
