/* render.h — CPU cell rasterizer (docs/adr/0005).
 *
 * Reads the headless VT grid (docs/adr/0001) and paints it into an RGBA
 * framebuffer, redrawing only rows whose cells changed. Platform-free:
 * glyph coverage masks arrive through a caller-supplied lookup, so this
 * file rasterizes identically on macOS and in the Linux unit suite. */
#ifndef TNYTTY_UI_RENDER_H
#define TNYTTY_UI_RENDER_H

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

tt_render *tt_render_new(const tt_render_config *cfg, int px_w, int px_h);
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
int tt_render_width(const tt_render *r);
int tt_render_height(const tt_render *r);
/* Row-major 0xAARRGGBB (alpha always 0xff), tt_render_width() per row. */
const uint32_t *tt_render_pixels(const tt_render *r);

/* Paint every dirty row of t, plus the status bar when its text
 * changed. Returns the number of lines painted (the bar counts as one);
 * *y0 and *y1 (may be NULL) get the touched device-pixel band, y1
 * exclusive. Returns 0 and an empty band when nothing changed. */
int tt_render_frame(tt_render *r, const vt *t, bool focused, int *y0, int *y1);

#endif
