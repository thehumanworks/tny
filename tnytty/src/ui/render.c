#include "ui/render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct tt_render {
    tt_render_config cfg;
    tt_fb own;       /* pixels, when this rasterizer allocated them */
    const tt_fb *fb; /* where painting lands: &own, or the caller's */
    tt_rect area;    /* this pane's rectangle of fb, device pixels */
    int cols, rows;
    vt_cell *shadow; /* cols * rows; what the framebuffer currently shows */
    bool *shadow_ok; /* per row: shadow holds a painted row */
    /* A glyph may overhang its cell (render.h), so a row's ink can land
     * in the band above or below it. These record where the last paint
     * of each row put ink outside its own band, so a repaint can take
     * the affected neighbour with it instead of stranding the overhang
     * (a ghost) or clipping it away. */
    bool *bleed_up, *bleed_down;
    bool *dirty;
    bool full; /* next frame repaints everything */
    int cur_x, cur_y;
    bool cur_vis, cur_focus;
    tt_selection sel, sel_prev;
    int dirty_row0, dirty_row1;
    char status[TT_STATUS_MAX];       /* what the caller asked for */
    char status_shown[TT_STATUS_MAX]; /* what the framebuffer holds */
};

/* ---- framebuffer primitives ------------------------------------------ */

int tt_fb_alloc(tt_fb *fb, int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    uint32_t *px = calloc((size_t)w * (size_t)h, sizeof *px);
    if (!px) return -1;
    free(fb->px);
    fb->px = px;
    fb->w = w;
    fb->h = h;
    return 0;
}

void tt_fb_free(tt_fb *fb) {
    free(fb->px);
    fb->px = NULL;
    fb->w = fb->h = 0;
}

/* Solid rectangle in absolute framebuffer coordinates, clipped to both
 * `clip` and the framebuffer itself. */
static void fill_clip(const tt_fb *fb, tt_rect clip, int x, int y, int w, int h, uint32_t rgb) {
    if (!fb || !fb->px) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < clip.x) x0 = clip.x;
    if (y0 < clip.y) y0 = clip.y;
    if (x1 > clip.x + clip.w) x1 = clip.x + clip.w;
    if (y1 > clip.y + clip.h) y1 = clip.y + clip.h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb->w) x1 = fb->w;
    if (y1 > fb->h) y1 = fb->h;
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t v = 0xff000000u | rgb;
    for (int row = y0; row < y1; row++) {
        uint32_t *p = fb->px + (size_t)row * (size_t)fb->w;
        for (int i = x0; i < x1; i++) p[i] = v;
    }
}

void tt_fb_fill(const tt_fb *fb, int x, int y, int w, int h, uint32_t rgb) {
    tt_rect all = {0, 0, fb ? fb->w : 0, fb ? fb->h : 0};
    fill_clip(fb, all, x, y, w, h, rgb);
}

/* ---- pure geometry / color ------------------------------------------- */

void tt_render_grid_for(const tt_render_config *cfg, int px_w, int px_h, int *cols, int *rows) {
    int cw = cfg->cell_w > 0 ? cfg->cell_w : 1;
    int ch = cfg->cell_h > 0 ? cfg->cell_h : 1;
    int usable_w = px_w - cfg->pad_left - cfg->pad_right;
    int usable_h = px_h - cfg->pad_top - cfg->pad_bottom - cfg->status_h;
    int c = usable_w > 0 ? usable_w / cw : 0;
    int r = usable_h > 0 ? usable_h / ch : 0;
    if (cols) *cols = c < 1 ? 1 : c;
    if (rows) *rows = r < 1 ? 1 : r;
}

void tt_render_cell_origin(const tt_render_config *cfg, int col, int row, int *x, int *y) {
    if (x) *x = cfg->pad_left + col * cfg->cell_w;
    if (y) *y = cfg->pad_top + row * cfg->cell_h;
}

void tt_render_cell_at(const tt_render_config *cfg, int px, int py, int cols, int rows, int *col,
                       int *row) {
    int cw = cfg->cell_w > 0 ? cfg->cell_w : 1;
    int ch = cfg->cell_h > 0 ? cfg->cell_h : 1;
    int c = (px - cfg->pad_left) / cw;
    int r = (py - cfg->pad_top) / ch;
    if (px < cfg->pad_left) c = 0;
    if (py < cfg->pad_top) r = 0;
    if (c < 0) c = 0;
    if (r < 0) r = 0;
    if (c > cols - 1) c = cols - 1;
    if (r > rows - 1) r = rows - 1;
    if (col) *col = c;
    if (row) *row = r;
}

bool tt_render_cell_hit(const tt_render_config *cfg, int px, int py, int cols, int rows, int *col,
                        int *row) {
    int cw = cfg->cell_w > 0 ? cfg->cell_w : 1;
    int ch = cfg->cell_h > 0 ? cfg->cell_h : 1;
    tt_render_cell_at(cfg, px, py, cols, rows, col, row);
    return px >= cfg->pad_left && px < cfg->pad_left + cols * cw && py >= cfg->pad_top &&
           py < cfg->pad_top + rows * ch;
}

/* xterm's 256-color space: the low 16 are the configurable palette
 * (docs/config.md), then a 6x6x6 cube and 24 grays. */
static uint32_t palette(const tt_render_config *cfg, unsigned idx) {
    static const unsigned step[6] = {0, 95, 135, 175, 215, 255};
    if (idx < TT_PALETTE_LEN) return cfg->palette[idx];
    if (idx < 232) {
        unsigned n = idx - 16;
        return (step[n / 36] << 16) | (step[(n / 6) % 6] << 8) | step[n % 6];
    }
    if (idx < 256) {
        unsigned g = 8 + 10 * (idx - 232);
        return (g << 16) | (g << 8) | g;
    }
    return 0;
}

uint32_t tt_render_color(const tt_render_config *cfg, uint32_t color, uint32_t dflt) {
    switch (VT_COLOR_TAG(color)) {
    case VT_COLOR_IDX: return palette(cfg, VT_COLOR_VAL(color) & 0xff);
    case VT_COLOR_RGB: return VT_COLOR_VAL(color);
    default: return dflt;
    }
}

static uint32_t mix(uint32_t a, uint32_t b, unsigned t) {
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        int ca = (int)((a >> shift) & 0xffu), cb = (int)((b >> shift) & 0xffu);
        int c = ca + ((cb - ca) * (int)t + 127) / 255;
        if (c < 0) c = 0;
        if (c > 255) c = 255;
        out |= (uint32_t)c << shift;
    }
    return out;
}

uint32_t tt_render_mix(uint32_t a, uint32_t b, unsigned t) { return mix(a, b, t); }

/* ---- glyph blending --------------------------------------------------- */

/* cx/cy are the cell's top-left in absolute framebuffer coordinates. */
static void blend_clip(const tt_fb *fb, tt_rect clip, const tt_glyph *g, int cx, int cy, int cell_h,
                       uint32_t fg, bool *up, bool *down) {
    if (up && g->top < 0) *up = true;
    if (down && g->top + g->h > cell_h) *down = true;
    int cx0 = clip.x, cy0 = clip.y, cx1 = clip.x + clip.w, cy1 = clip.y + clip.h;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > fb->w) cx1 = fb->w;
    if (cy1 > fb->h) cy1 = fb->h;
    for (int gy = 0; gy < g->h; gy++) {
        int y = cy + g->top + gy;
        if (y < cy0 || y >= cy1) continue;
        const uint8_t *src = g->alpha + (size_t)gy * (size_t)g->w;
        uint32_t *dst = fb->px + (size_t)y * (size_t)fb->w;
        for (int gx = 0; gx < g->w; gx++) {
            unsigned a = src[gx];
            if (!a) continue;
            int x = cx + g->left + gx;
            if (x < cx0 || x >= cx1) continue;
            dst[x] = 0xff000000u | (a == 255 ? fg : mix(dst[x] & 0xffffffu, fg, a));
        }
    }
}

/* ---- lifecycle -------------------------------------------------------- */

static int alloc_grid(tt_render *r) {
    int cols = 0, rows = 0;
    tt_render_grid_for(&r->cfg, r->area.w, r->area.h, &cols, &rows);
    vt_cell *shadow = calloc((size_t)cols * (size_t)rows, sizeof *shadow);
    bool *ok = calloc((size_t)rows, sizeof *ok);
    bool *bu = calloc((size_t)rows, sizeof *bu);
    bool *bd = calloc((size_t)rows, sizeof *bd);
    bool *dirty = calloc((size_t)rows, sizeof *dirty);
    if (!shadow || !ok || !bu || !bd || !dirty) {
        free(shadow);
        free(ok);
        free(bu);
        free(bd);
        free(dirty);
        return -1;
    }
    free(r->shadow);
    free(r->shadow_ok);
    free(r->bleed_up);
    free(r->bleed_down);
    free(r->dirty);
    r->shadow = shadow;
    r->shadow_ok = ok;
    r->bleed_up = bu;
    r->bleed_down = bd;
    r->dirty = dirty;
    r->cols = cols;
    r->rows = rows;
    /* The grid moved under the caret: forget where it was, so the next
     * frame paints one caret and no ghost of the old geometry. */
    r->cur_x = r->cur_y = -1;
    r->cur_vis = false;
    r->full = true;
    return 0;
}

static tt_render *render_new(const tt_render_config *cfg) {
    tt_render *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->cfg = *cfg;
    if (r->cfg.cell_w < 1) r->cfg.cell_w = 1;
    if (r->cfg.cell_h < 1) r->cfg.cell_h = 1;
    r->cur_x = r->cur_y = -1;
    return r;
}

tt_render *tt_render_new(const tt_render_config *cfg, int px_w, int px_h) {
    tt_render *r = render_new(cfg);
    if (!r) return NULL;
    if (px_w < 1) px_w = 1;
    if (px_h < 1) px_h = 1;
    if (tt_fb_alloc(&r->own, px_w, px_h) != 0) {
        free(r);
        return NULL;
    }
    r->fb = &r->own;
    r->area = (tt_rect){0, 0, px_w, px_h};
    if (alloc_grid(r) != 0) {
        tt_render_free(r);
        return NULL;
    }
    return r;
}

tt_render *tt_render_new_in(const tt_render_config *cfg, const tt_fb *fb, tt_rect area) {
    tt_render *r = render_new(cfg);
    if (!r) return NULL;
    r->fb = fb;
    r->area = area;
    if (r->area.w < 1) r->area.w = 1;
    if (r->area.h < 1) r->area.h = 1;
    if (alloc_grid(r) != 0) {
        tt_render_free(r);
        return NULL;
    }
    return r;
}

int tt_render_set_area(tt_render *r, tt_rect area) {
    if (area.w < 1) area.w = 1;
    if (area.h < 1) area.h = 1;
    r->area = area;
    return alloc_grid(r);
}

tt_rect tt_render_area(const tt_render *r) { return r->area; }

void tt_render_free(tt_render *r) {
    if (!r) return;
    tt_fb_free(&r->own);
    free(r->shadow);
    free(r->shadow_ok);
    free(r->bleed_up);
    free(r->bleed_down);
    free(r->dirty);
    free(r);
}

int tt_render_configure(tt_render *r, const tt_render_config *cfg) {
    r->cfg = *cfg;
    if (r->cfg.cell_w < 1) r->cfg.cell_w = 1;
    if (r->cfg.cell_h < 1) r->cfg.cell_h = 1;
    return alloc_grid(r);
}

int tt_render_resize(tt_render *r, int px_w, int px_h) {
    if (px_w < 1) px_w = 1;
    if (px_h < 1) px_h = 1;
    if (px_w == r->fb->w && px_h == r->fb->h) {
        r->full = true;
        return 0;
    }
    if (r->fb != &r->own) return -1; /* a pane moves with tt_render_set_area */
    if (tt_fb_alloc(&r->own, px_w, px_h) != 0) return -1;
    return tt_render_set_area(r, (tt_rect){0, 0, px_w, px_h});
}

void tt_render_invalidate(tt_render *r) { r->full = true; }

void tt_render_set_selection(tt_render *r, const tt_selection *sel) {
    if (sel) r->sel = *sel;
    else tt_sel_clear(&r->sel);
}

void tt_render_set_status(tt_render *r, const char *text) {
    snprintf(r->status, sizeof r->status, "%s", text ? text : "");
}

void tt_render_dirty_rows(const tt_render *r, int *row0, int *row1) {
    if (row0) *row0 = r->dirty_row0;
    if (row1) *row1 = r->dirty_row1;
}

int tt_render_cols(const tt_render *r) { return r->cols; }
int tt_render_rows(const tt_render *r) { return r->rows; }
int tt_render_width(const tt_render *r) { return r->fb->w; }
int tt_render_height(const tt_render *r) { return r->fb->h; }
const uint32_t *tt_render_pixels(const tt_render *r) { return r->fb->px; }

/* ---- painting --------------------------------------------------------- */

/* Pane-relative rectangle, clipped to the pane. */
static void fill_rect(tt_render *r, int x, int y, int w, int h, uint32_t rgb) {
    fill_clip(r->fb, r->area, r->area.x + x, r->area.y + y, w, h, rgb);
}

static void blend_glyph(tt_render *r, const tt_glyph *g, int cx, int cy, uint32_t fg, bool *up,
                        bool *down) {
    blend_clip(r->fb, r->area, g, r->area.x + cx, r->area.y + cy, r->cfg.cell_h, fg, up, down);
}

static bool cell_eq(const vt_cell *a, const vt_cell *b) {
    return a->cp == b->cp && a->combine == b->combine && a->fg == b->fg && a->bg == b->bg &&
           a->attrs == b->attrs;
}

/* cursor_col < 0 = no cursor on this row; cursor_solid = the pane has
 * focus, so the caret is a filled block rather than a hollow box. */
static void paint_row(tt_render *r, const vt_cell *line, int cols, int row, int cursor_col,
                      bool cursor_solid, int sel_c0, int sel_c1, bool *up, bool *down) {
    int rx = 0, ry = 0;
    *up = *down = false;
    tt_render_cell_origin(&r->cfg, 0, row, &rx, &ry);
    /* Clear the whole band, padding columns included, so last frame's
     * overhang cannot survive under the new text. */
    fill_rect(r, 0, ry, r->area.w, r->cfg.cell_h, r->cfg.bg);

    for (int col = 0; col < cols; col++) {
        const vt_cell *c = &line[col];
        if (c->attrs & VT_ATTR_WIDE_CONT) continue;
        int span = (c->attrs & VT_ATTR_WIDE) ? 2 : 1;
        int x = rx + col * r->cfg.cell_w;
        uint32_t cell_fg = c->fg;
        /* Bold is a heavier face (the glyph lookup sees the attr) and,
         * when bold-brightens is on, the bright half of the palette --
         * every bright entry is contrast-checked, so this cannot make
         * text less readable (docs/config.md). */
        if ((c->attrs & VT_ATTR_BOLD) && r->cfg.bold_brightens &&
            VT_COLOR_TAG(cell_fg) == VT_COLOR_IDX && VT_COLOR_VAL(cell_fg) < 8)
            cell_fg = VT_COLOR_IDX | (VT_COLOR_VAL(cell_fg) + 8);
        uint32_t fg = tt_render_color(&r->cfg, cell_fg, r->cfg.fg);
        uint32_t bg = tt_render_color(&r->cfg, c->bg, r->cfg.bg);
        if (c->attrs & VT_ATTR_REVERSE) {
            uint32_t t = fg;
            fg = bg;
            bg = t;
        }
        if (c->attrs & VT_ATTR_FAINT) fg = mix(bg, fg, 176);
        bool selected = col >= sel_c0 && col < sel_c1;
        bool cursor = cursor_solid && col == cursor_col;
        if (selected != cursor) { /* selection and caret both invert; both = no-op */
            uint32_t t = fg;
            fg = bg;
            bg = t;
        }
        if (bg != r->cfg.bg || cursor || selected)
            fill_rect(r, x, ry, r->cfg.cell_w * span, r->cfg.cell_h, bg);

        if (!(c->attrs & VT_ATTR_HIDDEN) && c->cp && c->cp != ' ' && r->cfg.glyph) {
            tt_glyph g;
            if (r->cfg.glyph(r->cfg.glyph_user, c->cp, c->attrs, &g) && g.alpha && g.w > 0)
                blend_glyph(r, &g, x, ry, fg, up, down);
            if (c->combine && r->cfg.glyph(r->cfg.glyph_user, c->combine, c->attrs, &g) &&
                g.alpha && g.w > 0)
                blend_glyph(r, &g, x, ry, fg, up, down);
        }
        int thick = r->cfg.cell_h >= 24 ? 2 : 1;
        if (c->attrs & VT_ATTR_UNDERLINE)
            fill_rect(r, x, ry + r->cfg.cell_h - thick, r->cfg.cell_w * span, thick, fg);
        if (c->attrs & VT_ATTR_STRIKE)
            fill_rect(r, x, ry + r->cfg.cell_h / 2, r->cfg.cell_w * span, thick, fg);
    }
    if (!cursor_solid && cursor_col >= 0 && cursor_col < cols) {
        /* Unfocused: hollow box, so the caret is visible but not solid. */
        int x = rx + cursor_col * r->cfg.cell_w;
        uint32_t fg = tt_render_color(&r->cfg, line[cursor_col].fg, r->cfg.fg);
        fill_rect(r, x, ry, r->cfg.cell_w, 1, fg);
        fill_rect(r, x, ry + r->cfg.cell_h - 1, r->cfg.cell_w, 1, fg);
        fill_rect(r, x, ry, 1, r->cfg.cell_h, fg);
        fill_rect(r, x + r->cfg.cell_w - 1, ry, 1, r->cfg.cell_h, fg);
    }
}

/* Per-row selection span, or an empty span when the row has none. */
static void row_sel(const tt_selection *sel, int row, int cols, int *c0, int *c1) {
    if (!tt_sel_row_span(sel, row, cols, c0, c1)) *c0 = *c1 = 0;
}

/* Minimal UTF-8 decode for status text (which we produced ourselves, so
 * it is well-formed); returns the codepoint and advances *p. */
static uint32_t next_cp(const unsigned char **p) {
    const unsigned char *s = *p;
    uint32_t cp = *s++;
    int extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
    static const uint32_t lead_mask[4] = {0x7f, 0x1f, 0x0f, 0x07};
    cp &= lead_mask[extra];
    for (int i = 0; i < extra && (*s & 0xc0) == 0x80; i++) cp = (cp << 6) | (*s++ & 0x3f);
    *p = s;
    return cp;
}

/* The status bar sits flush along the bottom edge, outside every grid:
 * grid_for() already took its height out of the rows. */
void tt_render_status_bar(const tt_fb *fb, const tt_render_config *cfg, const char *text) {
    int h = cfg->status_h;
    if (h <= 0 || !fb || !fb->px) return;
    tt_rect all = {0, 0, fb->w, fb->h};
    int by = fb->h - h;
    fill_clip(fb, all, 0, by, fb->w, h, cfg->status_bg);
    if (!text || !text[0] || !cfg->glyph) return;
    int ty = by + (h - cfg->cell_h) / 2;
    int x = cfg->pad_left;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        uint32_t cp = next_cp(&p);
        if (x + cfg->cell_w > fb->w - cfg->pad_right) break;
        if (cp != ' ') {
            tt_glyph g;
            if (cfg->glyph(cfg->glyph_user, cp, 0, &g) && g.alpha && g.w > 0)
                blend_clip(fb, all, &g, x, ty, cfg->cell_h, cfg->status_fg, NULL, NULL);
        }
        x += cfg->cell_w;
    }
}

int tt_render_frame(tt_render *r, const vt *t, bool focused, int *y0, int *y1) {
    int rows = vt_rows(t) < r->rows ? vt_rows(t) : r->rows;
    int cols = vt_cols(t) < r->cols ? vt_cols(t) : r->cols;
    int cx = vt_cursor_x(t), cy = vt_cursor_y(t);
    bool cvis = vt_cursor_visible(t);
    /* The caret only forces a repaint when it actually changed: an idle
     * terminal must produce zero dirty rows, or the window would
     * re-present on every loop tick. */
    bool cursor_changed =
        cx != r->cur_x || cy != r->cur_y || cvis != r->cur_vis || focused != r->cur_focus;

    /* Pass 1: which rows changed on their own account. */
    for (int row = 0; row < rows; row++) {
        const vt_cell *line = vt_line(t, row);
        if (!line) {
            r->dirty[row] = false;
            continue;
        }
        const vt_cell *shadow = r->shadow + (size_t)row * (size_t)r->cols;
        bool cursor_here = (cvis && cy == row) || (r->cur_vis && r->cur_y == row);
        int c0 = 0, c1 = 0, w0 = 0, w1 = 0;
        row_sel(&r->sel, row, cols, &c0, &c1);
        row_sel(&r->sel_prev, row, cols, &w0, &w1);
        bool dirty = r->full || !r->shadow_ok[row] || (cursor_changed && cursor_here) || c0 != w0 ||
                     c1 != w1;
        if (!dirty)
            for (int col = 0; col < cols; col++)
                if (!cell_eq(&line[col], &shadow[col])) {
                    dirty = true;
                    break;
                }
        r->dirty[row] = dirty;
    }

    /* Pass 2: a glyph may overhang into the neighbouring band (ADR 0004
     * allows nerd-font overhang). Repainting a row clears its whole
     * band, so any neighbour whose ink reaches into it must be repainted
     * too, or the overhang is left clipped in half. One sweep is enough:
     * the glyph mask is bounded to one cell of slack each way. */
    for (int row = 0; row < rows; row++) {
        if (!r->dirty[row]) continue;
        if (row > 0 && (r->bleed_down[row - 1] || r->bleed_up[row])) r->dirty[row - 1] = true;
        if (row + 1 < rows && (r->bleed_up[row + 1] || r->bleed_down[row]))
            r->dirty[row + 1] = true;
    }

    int lo = r->area.y + r->area.h, hi = r->area.y, painted = 0;
    int drow0 = rows, drow1 = 0;
    bool full_frame = r->full;
    if (r->full) {
        /* Padding bands are outside every row: clear once. */
        fill_rect(r, 0, 0, r->area.w, r->area.h, r->cfg.bg);
        lo = r->area.y;
        hi = r->area.y + r->area.h;
    }

    /* Pass 3: paint. Backgrounds first for the whole run of dirty rows
     * would be tidier still, but a repainted row already re-blends its
     * neighbours' overhang because pass 2 pulled them in. */
    for (int row = 0; row < rows; row++) {
        if (!r->dirty[row]) continue;
        const vt_cell *line = vt_line(t, row);
        if (!line) continue;
        vt_cell *shadow = r->shadow + (size_t)row * (size_t)r->cols;
        int sel_c0 = 0, sel_c1 = 0;
        row_sel(&r->sel, row, cols, &sel_c0, &sel_c1);
        int cursor_col = cvis && cy == row && cx >= 0 && cx < cols ? cx : -1;
        bool up = false, down = false;
        paint_row(r, line, cols, row, cursor_col, focused, sel_c0, sel_c1, &up, &down);
        r->bleed_up[row] = up;
        r->bleed_down[row] = down;
        memcpy(shadow, line, (size_t)cols * sizeof *shadow);
        r->shadow_ok[row] = true;
        painted++;

        int ry = 0;
        tt_render_cell_origin(&r->cfg, 0, row, NULL, &ry);
        ry += r->area.y;
        if (ry < lo) lo = ry;
        if (ry + r->cfg.cell_h > hi) hi = ry + r->cfg.cell_h;
        if (row < drow0) drow0 = row;
        if (row + 1 > drow1) drow1 = row + 1;
    }

    if (r->cfg.status_h > 0 && (full_frame || strcmp(r->status, r->status_shown) != 0)) {
        tt_render_status_bar(r->fb, &r->cfg, r->status);
        snprintf(r->status_shown, sizeof r->status_shown, "%s", r->status);
        painted++; /* the bar counts as a painted line: the caller presents */
        int by = r->fb->h - r->cfg.status_h;
        if (by < lo) lo = by;
        if (r->fb->h > hi) hi = r->fb->h;
    }

    r->cur_x = cx;
    r->cur_y = cy;
    r->cur_vis = cvis;
    r->cur_focus = focused;
    r->sel_prev = r->sel;
    r->full = false;
    r->dirty_row0 = drow1 > drow0 ? drow0 : 0;
    r->dirty_row1 = drow1 > drow0 ? drow1 : 0;
    if (hi > r->fb->h) hi = r->fb->h;
    if (lo > hi) lo = hi = 0;
    if (y0) *y0 = lo;
    if (y1) *y1 = hi;
    return painted;
}
