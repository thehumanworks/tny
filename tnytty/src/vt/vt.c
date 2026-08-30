/* vt.c — headless VT engine: incremental parser + cell grid (docs/adr/0001). */
#include "vt/vt.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VT_MAX_PARAMS 16
#define VT_OSC_CAP    512
/* One kitty graphics chunk is <= 4096 raw key bytes plus ~5.5 KiB of
 * base64 payload; 8 KiB holds any conforming APC (docs/adr/0003). */
#define VT_STR_CAP 8192

#define VT_SNAPSHOT_VERSION   1u
#define VT_SNAPSHOT_HEADER    56u
#define VT_SNAPSHOT_CELL      18u
#define VT_SNAPSHOT_MAX_DIM   1000u
#define VT_SNAPSHOT_MAX_CELLS (VT_SNAPSHOT_MAX_DIM * VT_SNAPSHOT_MAX_DIM)

enum parser_state {
    S_GROUND,
    S_ESC,       /* saw ESC */
    S_ESC_INTER, /* ESC + intermediate (charset designators etc.) */
    S_CSI,       /* collecting CSI params */
    S_STR,       /* OSC / APC / DCS / PM / SOS string body */
    S_STR_ESC,   /* saw ESC inside a string (ST is ESC \) */
};

enum str_kind { STR_NONE, STR_OSC, STR_APC, STR_OTHER };

typedef struct {
    vt_cell *cells;
    int cols;
} sb_line;

struct vt {
    int cols, rows;
    vt_cell *main_grid;
    vt_cell *alt_grid;
    vt_cell *grid; /* points at main_grid or alt_grid */
    bool alt_on;

    int cx, cy;
    bool wrap_pending;
    int top, bot; /* scroll region, inclusive */
    uint16_t attrs;
    uint32_t fg, bg;

    int saved_cx, saved_cy;
    uint16_t saved_attrs;
    uint32_t saved_fg, saved_bg;

    bool cursor_visible;
    bool autowrap;
    bool bracketed;
    bool app_cursor; /* DECCKM: arrows send SS3, not CSI */
    char title[256];
    char cwd[VT_OSC_CAP];
    uint64_t generation;

    /* parser */
    int state;
    uint32_t utf_cp;
    int utf_left;
    int params[VT_MAX_PARAMS];
    int nparams;
    bool has_digit;
    bool priv; /* CSI ? */
    char str_buf[VT_STR_CAP];
    size_t str_len;
    bool str_overflow;
    int str_kind;

    /* scrollback ring: sb[(sb_head + i) % sb_max] for i in [0, sb_len) */
    sb_line *sb;
    int sb_max, sb_len, sb_head;

    int gfx_count;

    vt_write_cb respond;
    void *respond_user;
    vt_write_cb gfx_cb;
    void *gfx_user;
};

static void semantic_change(vt *t) {
    if (t->generation != UINT64_MAX) t->generation++;
}

/* ---- width tables (docs/adr/0004) ----------------------------------- */

typedef struct {
    uint32_t lo, hi;
} cp_range;

static const cp_range zero_width[] = {
    {0x0300, 0x036F},   {0x0483, 0x0489}, {0x0591, 0x05C7}, {0x0610, 0x061A}, {0x064B, 0x065F},
    {0x0670, 0x0670},   {0x06D6, 0x06ED}, {0x0711, 0x0711}, {0x0730, 0x074A}, {0x07A6, 0x07B0},
    {0x0900, 0x0902},   {0x093C, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D}, {0x0951, 0x0954},
    {0x0E31, 0x0E31},   {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF},
    {0x200B, 0x200F},   {0x2060, 0x2064}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F},
    {0xE0100, 0xE01EF},
};

static const cp_range wide[] = {
    {0x1100, 0x115F},   {0x2E80, 0x303E},   {0x3041, 0x33FF},   {0x3400, 0x4DBF},
    {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},   {0xA960, 0xA97F},   {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF},   {0xFE30, 0xFE4F},   {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},
    {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F900, 0x1F9FF}, {0x20000, 0x2FFFD},
    {0x30000, 0x3FFFD},
};

static bool in_ranges(uint32_t cp, const cp_range *r, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (cp >= r[i].lo && cp <= r[i].hi) return true;
    return false;
}

int vt_cp_width(uint32_t cp) {
    if (cp == 0) return 1;
    if (in_ranges(cp, zero_width, sizeof zero_width / sizeof zero_width[0])) return 0;
    /* PUA (nerd fonts) is single-width by policy, all three areas. */
    if ((cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFD) ||
        (cp >= 0x100000 && cp <= 0x10FFFD))
        return 1;
    if (in_ranges(cp, wide, sizeof wide / sizeof wide[0])) return 2;
    return 1;
}

/* ---- grid helpers ---------------------------------------------------- */

static vt_cell blank_cell(const vt *t) {
    vt_cell c = {0, 0, VT_COLOR_DEFAULT, t->bg, 0};
    return c;
}

static bool cell_equal(const vt_cell *a, const vt_cell *b) {
    return a->cp == b->cp && a->combine == b->combine && a->fg == b->fg && a->bg == b->bg &&
           a->attrs == b->attrs;
}

static vt_cell *cell_at(vt *t, int y, int x) { return &t->grid[(size_t)y * t->cols + x]; }

static void clear_cells(vt *t, int y, int x0, int x1) {
    vt_cell b = blank_cell(t);
    bool changed = false;
    for (int x = x0; x <= x1 && x < t->cols; x++)
        if (x >= 0) {
            vt_cell *c = cell_at(t, y, x);
            if (!cell_equal(c, &b)) {
                *c = b;
                changed = true;
            }
        }
    if (changed) semantic_change(t);
}

static void clear_rows(vt *t, int y0, int y1) {
    for (int y = y0; y <= y1 && y < t->rows; y++)
        if (y >= 0) clear_cells(t, y, 0, t->cols - 1);
}

static void sb_free_all(vt *t) {
    for (int i = 0; i < t->sb_len; i++) free(t->sb[(t->sb_head + i) % t->sb_max].cells);
    t->sb_len = 0;
    t->sb_head = 0;
}

static void sb_push(vt *t, const vt_cell *row) {
    if (t->sb_max <= 0 || t->alt_on) return;
    vt_cell *copy = malloc((size_t)t->cols * sizeof *copy);
    if (!copy) return;
    memcpy(copy, row, (size_t)t->cols * sizeof *copy);
    if (t->sb_len == t->sb_max) {
        free(t->sb[t->sb_head].cells);
        t->sb[t->sb_head].cells = copy;
        t->sb[t->sb_head].cols = t->cols;
        t->sb_head = (t->sb_head + 1) % t->sb_max;
    } else {
        int slot = (t->sb_head + t->sb_len) % t->sb_max;
        t->sb[slot].cells = copy;
        t->sb[slot].cols = t->cols;
        t->sb_len++;
    }
}

/* Scroll the region [top,bot] up by n. Lines scrolled off the top of a
 * full-screen region go to scrollback only when the scroll is content
 * motion (linefeed, SU) — never for delete-line edits. */
static void scroll_up(vt *t, int n, bool keep) {
    if (n <= 0) return;
    int span = t->bot - t->top + 1;
    if (n > span) n = span;
    if (keep && t->top == 0 && t->bot == t->rows - 1)
        for (int i = 0; i < n; i++) sb_push(t, cell_at(t, t->top + i, 0));
    memmove(cell_at(t, t->top, 0), cell_at(t, t->top + n, 0),
            (size_t)(span - n) * t->cols * sizeof(vt_cell));
    clear_rows(t, t->bot - n + 1, t->bot);
    semantic_change(t);
}

static void scroll_down(vt *t, int n) {
    if (n <= 0) return;
    int span = t->bot - t->top + 1;
    if (n > span) n = span;
    memmove(cell_at(t, t->top + n, 0), cell_at(t, t->top, 0),
            (size_t)(span - n) * t->cols * sizeof(vt_cell));
    clear_rows(t, t->top, t->top + n - 1);
    semantic_change(t);
}

static void linefeed(vt *t) {
    int old_cy = t->cy;
    if (t->cy == t->bot) scroll_up(t, 1, true);
    else if (t->cy < t->rows - 1) t->cy++;
    t->wrap_pending = false;
    if (t->cy != old_cy) semantic_change(t);
}

static void reverse_index(vt *t) {
    int old_cy = t->cy;
    if (t->cy == t->top) scroll_down(t, 1);
    else if (t->cy > 0) t->cy--;
    t->wrap_pending = false;
    if (t->cy != old_cy) semantic_change(t);
}

static void move_cursor(vt *t, int y, int x) {
    int old_x = t->cx, old_y = t->cy;
    if (x < 0) x = 0;
    if (x > t->cols - 1) x = t->cols - 1;
    if (y < 0) y = 0;
    if (y > t->rows - 1) y = t->rows - 1;
    t->cx = x;
    t->cy = y;
    t->wrap_pending = false;
    if (t->cx != old_x || t->cy != old_y) semantic_change(t);
}

/* ---- printable input ------------------------------------------------- */

static void put_cp(vt *t, uint32_t cp) {
    int w = vt_cp_width(cp);
    if (w == 0) {
        /* Attach to the glyph the cursor last wrote. */
        int x = t->cx;
        if (!t->wrap_pending && x > 0) x--;
        else if (t->wrap_pending) x = t->cols - 1;
        vt_cell *c = cell_at(t, t->cy, x);
        if (c->attrs & VT_ATTR_WIDE_CONT && x > 0) c = cell_at(t, t->cy, x - 1);
        if (c->cp && !c->combine) {
            c->combine = cp;
            semantic_change(t);
        }
        return;
    }
    if (t->wrap_pending && t->autowrap) {
        t->cx = 0;
        linefeed(t);
    }
    t->wrap_pending = false;
    if (w == 2 && t->cx >= t->cols - 1) {
        /* Wide glyph at the last column: blank it and wrap first. */
        clear_cells(t, t->cy, t->cx, t->cx);
        if (t->autowrap) {
            t->cx = 0;
            linefeed(t);
        } else if (t->cx > 0) {
            t->cx--;
        }
    }
    vt_cell *c = cell_at(t, t->cy, t->cx);
    /* Overwriting half of an old wide glyph orphans the other half. */
    if (c->attrs & VT_ATTR_WIDE_CONT && t->cx > 0) {
        vt_cell *lead = cell_at(t, t->cy, t->cx - 1);
        lead->cp = 0;
        lead->combine = 0;
        lead->attrs &= (uint16_t)~VT_ATTR_WIDE;
    }
    if (c->attrs & VT_ATTR_WIDE && t->cx + 1 < t->cols) {
        vt_cell *cont = cell_at(t, t->cy, t->cx + 1);
        cont->cp = 0;
        cont->combine = 0;
        cont->attrs &= (uint16_t)~VT_ATTR_WIDE_CONT;
    }
    c->cp = cp;
    c->combine = 0;
    c->fg = t->fg;
    c->bg = t->bg;
    c->attrs = (uint16_t)(t->attrs | (w == 2 ? VT_ATTR_WIDE : 0));
    if (w == 2) {
        vt_cell *cont = cell_at(t, t->cy, t->cx + 1);
        cont->cp = 0;
        cont->combine = 0;
        cont->fg = t->fg;
        cont->bg = t->bg;
        cont->attrs = (uint16_t)(t->attrs | VT_ATTR_WIDE_CONT);
    }
    if (t->cx + w <= t->cols - 1) {
        t->cx += w;
    } else {
        t->cx = t->cols - 1;
        t->wrap_pending = true;
    }
    semantic_change(t);
}

/* ---- responses ------------------------------------------------------- */

static void respond(vt *t, const char *s) {
    if (t->respond) t->respond(t->respond_user, s, strlen(s));
}

/* ---- SGR ------------------------------------------------------------- */

/* 38/48 extended color: consumes params starting after the 38/48 at *i. */
static uint32_t sgr_ext_color(vt *t, int *i, uint32_t fallback) {
    if (*i + 1 >= t->nparams) return fallback;
    int mode = t->params[*i + 1];
    if (mode == 5 && *i + 2 < t->nparams) {
        uint32_t idx = (uint32_t)t->params[*i + 2] & 0xff;
        *i += 2;
        return VT_COLOR_IDX | idx;
    }
    if (mode == 2 && *i + 4 < t->nparams) {
        uint32_t r = (uint32_t)t->params[*i + 2] & 0xff;
        uint32_t g = (uint32_t)t->params[*i + 3] & 0xff;
        uint32_t b = (uint32_t)t->params[*i + 4] & 0xff;
        *i += 4;
        return VT_COLOR_RGB | (r << 16) | (g << 8) | b;
    }
    *i = t->nparams; /* malformed: swallow the rest */
    return fallback;
}

static void do_sgr(vt *t) {
    if (t->nparams == 0) { t->params[t->nparams++] = 0; }
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        switch (p) {
        case 0:
            t->attrs = 0;
            t->fg = VT_COLOR_DEFAULT;
            t->bg = VT_COLOR_DEFAULT;
            break;
        case 1: t->attrs |= VT_ATTR_BOLD; break;
        case 2: t->attrs |= VT_ATTR_FAINT; break;
        case 3: t->attrs |= VT_ATTR_ITALIC; break;
        case 4: t->attrs |= VT_ATTR_UNDERLINE; break;
        case 5:
        case 6: t->attrs |= VT_ATTR_BLINK; break;
        case 7: t->attrs |= VT_ATTR_REVERSE; break;
        case 8: t->attrs |= VT_ATTR_HIDDEN; break;
        case 9: t->attrs |= VT_ATTR_STRIKE; break;
        case 21:
        case 22: t->attrs &= (uint16_t)~(VT_ATTR_BOLD | VT_ATTR_FAINT); break;
        case 23: t->attrs &= (uint16_t)~VT_ATTR_ITALIC; break;
        case 24: t->attrs &= (uint16_t)~VT_ATTR_UNDERLINE; break;
        case 25: t->attrs &= (uint16_t)~VT_ATTR_BLINK; break;
        case 27: t->attrs &= (uint16_t)~VT_ATTR_REVERSE; break;
        case 28: t->attrs &= (uint16_t)~VT_ATTR_HIDDEN; break;
        case 29: t->attrs &= (uint16_t)~VT_ATTR_STRIKE; break;
        case 38: t->fg = sgr_ext_color(t, &i, t->fg); break;
        case 39: t->fg = VT_COLOR_DEFAULT; break;
        case 48: t->bg = sgr_ext_color(t, &i, t->bg); break;
        case 49: t->bg = VT_COLOR_DEFAULT; break;
        default:
            if (p >= 30 && p <= 37) t->fg = VT_COLOR_IDX | (uint32_t)(p - 30);
            else if (p >= 40 && p <= 47) t->bg = VT_COLOR_IDX | (uint32_t)(p - 40);
            else if (p >= 90 && p <= 97) t->fg = VT_COLOR_IDX | (uint32_t)(p - 90 + 8);
            else if (p >= 100 && p <= 107) t->bg = VT_COLOR_IDX | (uint32_t)(p - 100 + 8);
            break;
        }
    }
}

/* ---- modes ----------------------------------------------------------- */

static void enter_alt(vt *t) {
    if (t->alt_on) return;
    t->saved_cx = t->cx;
    t->saved_cy = t->cy;
    t->saved_attrs = t->attrs;
    t->saved_fg = t->fg;
    t->saved_bg = t->bg;
    t->alt_on = true;
    t->grid = t->alt_grid;
    uint32_t bg = t->bg;
    t->bg = VT_COLOR_DEFAULT;
    clear_rows(t, 0, t->rows - 1);
    t->bg = bg;
    move_cursor(t, 0, 0);
    semantic_change(t);
}

static void leave_alt(vt *t) {
    if (!t->alt_on) return;
    t->alt_on = false;
    t->grid = t->main_grid;
    t->cx = t->saved_cx;
    t->cy = t->saved_cy;
    t->attrs = t->saved_attrs;
    t->fg = t->saved_fg;
    t->bg = t->saved_bg;
    t->wrap_pending = false;
    semantic_change(t);
}

static void dec_mode(vt *t, bool set) {
    for (int i = 0; i < t->nparams; i++) {
        switch (t->params[i]) {
        case 1:
            if (t->app_cursor != set) {
                t->app_cursor = set;
                semantic_change(t);
            }
            break;
        case 7:
            if (t->autowrap != set) {
                t->autowrap = set;
                semantic_change(t);
            }
            break;
        case 25:
            if (t->cursor_visible != set) {
                t->cursor_visible = set;
                semantic_change(t);
            }
            break;
        case 47:
        case 1047: set ? enter_alt(t) : leave_alt(t); break;
        case 1049: set ? enter_alt(t) : leave_alt(t); break;
        case 2004:
            if (t->bracketed != set) {
                t->bracketed = set;
                semantic_change(t);
            }
            break;
        default: break; /* mouse/keyboard reporting modes: phase 2 */
        }
    }
}

/* ---- CSI dispatch ---------------------------------------------------- */

static int param(vt *t, int i, int dflt) {
    if (i >= t->nparams) return dflt;
    return t->params[i] ? t->params[i] : dflt;
}

static void insert_cells(vt *t, int n) {
    if (n > t->cols - t->cx) n = t->cols - t->cx;
    vt_cell *row = cell_at(t, t->cy, 0);
    memmove(&row[t->cx + n], &row[t->cx], (size_t)(t->cols - t->cx - n) * sizeof(vt_cell));
    clear_cells(t, t->cy, t->cx, t->cx + n - 1);
    semantic_change(t);
}

static void delete_cells(vt *t, int n) {
    if (n > t->cols - t->cx) n = t->cols - t->cx;
    vt_cell *row = cell_at(t, t->cy, 0);
    memmove(&row[t->cx], &row[t->cx + n], (size_t)(t->cols - t->cx - n) * sizeof(vt_cell));
    clear_cells(t, t->cy, t->cols - n, t->cols - 1);
    semantic_change(t);
}

static void do_csi(vt *t, char final) {
    char buf[64];
    switch (final) {
    case 'A': move_cursor(t, t->cy - param(t, 0, 1), t->cx); break;
    case 'B': move_cursor(t, t->cy + param(t, 0, 1), t->cx); break;
    case 'C': move_cursor(t, t->cy, t->cx + param(t, 0, 1)); break;
    case 'D': move_cursor(t, t->cy, t->cx - param(t, 0, 1)); break;
    case 'E': move_cursor(t, t->cy + param(t, 0, 1), 0); break;
    case 'F': move_cursor(t, t->cy - param(t, 0, 1), 0); break;
    case 'G':
    case '`': move_cursor(t, t->cy, param(t, 0, 1) - 1); break;
    case 'd': move_cursor(t, param(t, 0, 1) - 1, t->cx); break;
    case 'H':
    case 'f': move_cursor(t, param(t, 0, 1) - 1, param(t, 1, 1) - 1); break;
    case 'J':
        switch (t->params[0]) {
        case 0:
            clear_cells(t, t->cy, t->cx, t->cols - 1);
            clear_rows(t, t->cy + 1, t->rows - 1);
            break;
        case 1:
            clear_rows(t, 0, t->cy - 1);
            clear_cells(t, t->cy, 0, t->cx);
            break;
        case 2: clear_rows(t, 0, t->rows - 1); break;
        case 3:
            clear_rows(t, 0, t->rows - 1);
            sb_free_all(t);
            break;
        default: break;
        }
        break;
    case 'K':
        switch (t->params[0]) {
        case 0: clear_cells(t, t->cy, t->cx, t->cols - 1); break;
        case 1: clear_cells(t, t->cy, 0, t->cx); break;
        case 2: clear_cells(t, t->cy, 0, t->cols - 1); break;
        default: break;
        }
        break;
    case 'L':
        if (t->cy >= t->top && t->cy <= t->bot) {
            int save = t->top;
            t->top = t->cy;
            scroll_down(t, param(t, 0, 1));
            t->top = save;
        }
        break;
    case 'M':
        if (t->cy >= t->top && t->cy <= t->bot) {
            int save = t->top;
            t->top = t->cy;
            scroll_up(t, param(t, 0, 1), false);
            t->top = save;
        }
        break;
    case '@': insert_cells(t, param(t, 0, 1)); break;
    case 'P': delete_cells(t, param(t, 0, 1)); break;
    case 'X': {
        int n = param(t, 0, 1);
        clear_cells(t, t->cy, t->cx, t->cx + n - 1);
        break;
    }
    case 'S': scroll_up(t, param(t, 0, 1), true); break;
    case 'T': scroll_down(t, param(t, 0, 1)); break;
    case 'r': {
        int top = param(t, 0, 1) - 1;
        int bot = param(t, 1, t->rows) - 1;
        if (top < 0) top = 0;
        if (bot > t->rows - 1) bot = t->rows - 1;
        if (top < bot) {
            t->top = top;
            t->bot = bot;
            move_cursor(t, 0, 0);
        }
        break;
    }
    case 'm':
        if (!t->priv) do_sgr(t);
        break;
    case 'h':
        if (t->priv) dec_mode(t, true);
        break;
    case 'l':
        if (t->priv) dec_mode(t, false);
        break;
    case 'n':
        if (t->params[0] == 6) {
            snprintf(buf, sizeof buf, "\x1b[%d;%dR", t->cy + 1, t->cx + 1);
            respond(t, buf);
        } else if (t->params[0] == 5) {
            respond(t, "\x1b[0n");
        }
        break;
    case 'c':
        if (!t->priv && t->params[0] == 0) respond(t, "\x1b[?6c");
        break;
    case 's':
        t->saved_cx = t->cx;
        t->saved_cy = t->cy;
        break;
    case 'u': move_cursor(t, t->saved_cy, t->saved_cx); break;
    default: break; /* unimplemented finals are ignored, never fatal */
    }
}

/* ---- string terminators (OSC / APC) --------------------------------- */

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool ascii_equal(const char *s, size_t n, const char *literal) {
    size_t ln = strlen(literal);
    if (n != ln) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char a = (unsigned char)s[i], b = (unsigned char)literal[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

/* OSC 7 is deliberately metadata-only: accept an absolute local file URL,
 * decode percent escapes, and reject anything that could turn the value into
 * a control stream or a non-local path. Invalid input leaves the last valid
 * cwd untouched. */
static void osc_cwd(vt *t, const char *url, size_t len) {
    static const char prefix[] = "file://";
    if (len < sizeof prefix || memcmp(url, prefix, sizeof prefix - 1) != 0) return;
    const char *authority = url + sizeof prefix - 1;
    size_t remain = len - (sizeof prefix - 1);
    const char *slash = memchr(authority, '/', remain);
    if (!slash) return;
    size_t host_len = (size_t)(slash - authority);
    if (host_len != 0 && !ascii_equal(authority, host_len, "localhost")) return;

    char path[VT_OSC_CAP];
    size_t used = 0;
    const char *p = slash, *end = url + len;
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c == '?' || c == '#') return; /* URL query/fragment is not a cwd */
        if (c == '%') {
            if (end - p < 2) return;
            int hi = hex_digit((unsigned char)p[0]);
            int lo = hex_digit((unsigned char)p[1]);
            if (hi < 0 || lo < 0) return;
            c = (unsigned char)((hi << 4) | lo);
            p += 2;
        }
        if (c == 0 || c < 0x20 || c == 0x7f || used + 1 >= sizeof path) return;
        path[used++] = (char)c;
    }
    if (used == 0 || path[0] != '/') return;
    path[used] = '\0';
    if (strcmp(t->cwd, path) != 0) {
        memcpy(t->cwd, path, used + 1);
        semantic_change(t);
    }
}

static void dispatch_string(vt *t) {
    t->str_buf[t->str_len] = '\0';
    if (t->str_kind == STR_OSC) {
        /* OSC 0;title or 2;title */
        if (!t->str_overflow && (t->str_buf[0] == '0' || t->str_buf[0] == '2') &&
            t->str_buf[1] == ';') {
            const char *title = t->str_buf + 2;
            char next[sizeof t->title] = {0};
            snprintf(next, sizeof next, "%s", title);
            if (strcmp(t->title, next) != 0) {
                memcpy(t->title, next, sizeof next);
                semantic_change(t);
            }
        } else if (!t->str_overflow && t->str_buf[0] == '7' && t->str_buf[1] == ';') {
            osc_cwd(t, t->str_buf + 2, t->str_len - 2);
        }
    } else if (t->str_kind == STR_APC && t->str_buf[0] == 'G' && !t->str_overflow) {
        t->gfx_count++;
        semantic_change(t);
        if (t->gfx_cb) {
            t->gfx_cb(t->gfx_user, "\x1b_", 2);
            t->gfx_cb(t->gfx_user, t->str_buf, t->str_len);
            t->gfx_cb(t->gfx_user, "\x1b\\", 2);
        }
    }
    t->str_kind = STR_NONE;
    t->str_len = 0;
    t->str_overflow = false;
}

/* ---- byte pump ------------------------------------------------------- */

static void start_csi(vt *t) {
    t->state = S_CSI;
    t->nparams = 0;
    t->has_digit = false;
    t->priv = false;
    memset(t->params, 0, sizeof t->params);
}

static void start_string(vt *t, int kind) {
    t->state = S_STR;
    t->str_kind = kind;
    t->str_len = 0;
    t->str_overflow = false;
}

static void full_reset(vt *t) {
    bool mode_changed = !t->autowrap || !t->cursor_visible || t->bracketed || t->app_cursor;
    t->attrs = 0;
    t->fg = t->bg = VT_COLOR_DEFAULT;
    t->top = 0;
    t->bot = t->rows - 1;
    t->autowrap = true;
    t->cursor_visible = true;
    t->bracketed = false;
    t->app_cursor = false;
    leave_alt(t);
    clear_rows(t, 0, t->rows - 1);
    move_cursor(t, 0, 0);
    if (mode_changed) semantic_change(t);
}

static void control_byte(vt *t, unsigned char b) {
    switch (b) {
    case '\b': {
        int old_x = t->cx;
        if (t->wrap_pending) t->wrap_pending = false;
        else if (t->cx > 0) t->cx--;
        if (t->cx != old_x) semantic_change(t);
        break;
    }
    case '\t': {
        int old_x = t->cx;
        int next = (t->cx / 8 + 1) * 8;
        t->cx = next < t->cols ? next : t->cols - 1;
        t->wrap_pending = false;
        if (t->cx != old_x) semantic_change(t);
        break;
    }
    case '\n':
    case 0x0b:
    case 0x0c: linefeed(t); break;
    case '\r': {
        int old_x = t->cx;
        t->cx = 0;
        t->wrap_pending = false;
        if (t->cx != old_x) semantic_change(t);
        break;
    }
    case 0x1b: t->state = S_ESC; break;
    default: break; /* NUL, BEL outside strings, SO/SI, ... */
    }
}

static void feed_byte(vt *t, unsigned char b) {
    /* UTF-8 continuation handling is orthogonal to the escape parser:
     * escape bytes never appear inside multibyte sequences. */
    switch (t->state) {
    case S_GROUND:
        if (t->utf_left > 0) {
            if ((b & 0xc0) == 0x80) {
                t->utf_cp = (t->utf_cp << 6) | (b & 0x3f);
                if (--t->utf_left == 0) {
                    uint32_t cp = t->utf_cp;
                    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
                    put_cp(t, cp);
                }
                return;
            }
            t->utf_left = 0;
            put_cp(t, 0xFFFD); /* then fall through to reparse b */
        }
        if (b < 0x20 || b == 0x7f) {
            control_byte(t, b);
        } else if (b < 0x80) {
            put_cp(t, b);
        } else if ((b & 0xe0) == 0xc0) {
            t->utf_cp = b & 0x1f;
            t->utf_left = 1;
        } else if ((b & 0xf0) == 0xe0) {
            t->utf_cp = b & 0x0f;
            t->utf_left = 2;
        } else if ((b & 0xf8) == 0xf0) {
            t->utf_cp = b & 0x07;
            t->utf_left = 3;
        } else {
            put_cp(t, 0xFFFD);
        }
        break;

    case S_ESC:
        switch (b) {
        case '[': start_csi(t); break;
        case ']': start_string(t, STR_OSC); break;
        case '_': start_string(t, STR_APC); break;
        case 'P':
        case '^':
        case 'X': start_string(t, STR_OTHER); break;
        case '(':
        case ')':
        case '*':
        case '+':
        case '#':
        case '%': t->state = S_ESC_INTER; break;
        case '7':
            t->saved_cx = t->cx;
            t->saved_cy = t->cy;
            t->saved_attrs = t->attrs;
            t->saved_fg = t->fg;
            t->saved_bg = t->bg;
            t->state = S_GROUND;
            break;
        case '8':
            move_cursor(t, t->saved_cy, t->saved_cx);
            t->attrs = t->saved_attrs;
            t->fg = t->saved_fg;
            t->bg = t->saved_bg;
            t->state = S_GROUND;
            break;
        case 'D':
            linefeed(t);
            t->state = S_GROUND;
            break;
        case 'E':
            t->cx = 0;
            linefeed(t);
            t->state = S_GROUND;
            break;
        case 'M':
            reverse_index(t);
            t->state = S_GROUND;
            break;
        case 'c':
            full_reset(t);
            t->state = S_GROUND;
            break;
        case 0x1b: break;                    /* ESC ESC: stay */
        default: t->state = S_GROUND; break; /* =, >, unknowns */
        }
        break;

    case S_ESC_INTER:
        t->state = S_GROUND; /* charset designator payload: consume one byte */
        break;

    case S_CSI:
        if (b >= '0' && b <= '9') {
            if (t->nparams == 0) t->nparams = 1;
            int *p = &t->params[t->nparams - 1];
            if (*p < 100000) *p = *p * 10 + (b - '0');
            t->has_digit = true;
        } else if (b == ';' || b == ':') {
            if (t->nparams == 0) t->nparams = 1;
            if (t->nparams < VT_MAX_PARAMS) t->params[t->nparams++] = 0;
        } else if (b == '?') {
            t->priv = true;
        } else if (b == '<' || b == '=' || b == '>') {
            t->priv = true; /* private prefixes we do not act on */
        } else if (b >= 0x20 && b <= 0x2f) {
            /* intermediate bytes (e.g. CSI ! p): keep collecting */
        } else if (b >= 0x40 && b <= 0x7e) {
            do_csi(t, (char)b);
            t->state = S_GROUND;
        } else if (b == 0x1b) {
            t->state = S_ESC; /* aborted CSI */
        } else if (b < 0x20) {
            control_byte(t, b); /* C0 inside CSI executes */
        } else {
            t->state = S_GROUND;
        }
        break;

    case S_STR:
        if (b == 0x07 && t->str_kind == STR_OSC) {
            dispatch_string(t);
            t->state = S_GROUND;
        } else if (b == 0x1b) {
            t->state = S_STR_ESC;
        } else {
            if (t->str_len < VT_STR_CAP - 1) t->str_buf[t->str_len++] = (char)b;
            else t->str_overflow = true;
        }
        break;

    case S_STR_ESC:
        if (b == '\\') {
            dispatch_string(t);
            t->state = S_GROUND;
        } else {
            /* Not ST: the string was aborted by a new escape. */
            t->str_kind = STR_NONE;
            t->str_len = 0;
            t->str_overflow = false;
            t->state = S_ESC;
            feed_byte(t, b);
        }
        break;

    default: t->state = S_GROUND; break;
    }
}

void vt_feed(vt *t, const char *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) feed_byte(t, (unsigned char)bytes[i]);
}

/* ---- lifecycle ------------------------------------------------------- */

vt *vt_new(int cols, int rows, int scrollback_max) {
    if (cols < 1 || rows < 1) return NULL;
    vt *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->cols = cols;
    t->rows = rows;
    t->main_grid = calloc((size_t)cols * rows, sizeof(vt_cell));
    t->alt_grid = calloc((size_t)cols * rows, sizeof(vt_cell));
    t->sb_max = scrollback_max > 0 ? scrollback_max : 0;
    t->sb = t->sb_max ? calloc((size_t)t->sb_max, sizeof(sb_line)) : NULL;
    if (!t->main_grid || !t->alt_grid || (t->sb_max && !t->sb)) {
        vt_free(t);
        return NULL;
    }
    t->grid = t->main_grid;
    t->bot = rows - 1;
    t->autowrap = true;
    t->cursor_visible = true;
    return t;
}

void vt_free(vt *t) {
    if (!t) return;
    if (t->sb) sb_free_all(t);
    free(t->sb);
    free(t->main_grid);
    free(t->alt_grid);
    free(t);
}

void vt_set_respond(vt *t, vt_write_cb cb, void *user) {
    t->respond = cb;
    t->respond_user = user;
}

void vt_set_graphics(vt *t, vt_write_cb cb, void *user) {
    t->gfx_cb = cb;
    t->gfx_user = user;
}

static vt_cell *resize_grid(const vt_cell *grid, int old_cols, int old_rows, int cols, int rows) {
    if ((size_t)cols > SIZE_MAX / (size_t)rows / sizeof(vt_cell)) return NULL;
    vt_cell *ng = calloc((size_t)cols * rows, sizeof(vt_cell));
    if (!ng) return NULL;
    int copy_rows = rows < old_rows ? rows : old_rows;
    int copy_cols = cols < old_cols ? cols : old_cols;
    for (int y = 0; y < copy_rows; y++)
        memcpy(&ng[(size_t)y * cols], &grid[(size_t)y * old_cols],
               (size_t)copy_cols * sizeof(vt_cell));
    return ng;
}

void vt_resize(vt *t, int cols, int rows) {
    if (cols < 1 || rows < 1 || (cols == t->cols && rows == t->rows)) return;
    vt_cell *main_grid = resize_grid(t->main_grid, t->cols, t->rows, cols, rows);
    if (!main_grid) return;
    vt_cell *alt_grid = resize_grid(t->alt_grid, t->cols, t->rows, cols, rows);
    if (!alt_grid) {
        free(main_grid);
        return;
    }
    bool was_alt = t->alt_on;
    free(t->main_grid);
    free(t->alt_grid);
    t->main_grid = main_grid;
    t->alt_grid = alt_grid;
    t->cols = cols;
    t->rows = rows;
    t->grid = was_alt ? t->alt_grid : t->main_grid;
    t->top = 0;
    t->bot = rows - 1;
    if (t->cx > cols - 1) t->cx = cols - 1;
    if (t->cy > rows - 1) t->cy = rows - 1;
    t->wrap_pending = false;
    semantic_change(t);
}

/* ---- broker snapshot ------------------------------------------------- */

static const unsigned char snapshot_magic[8] = {'T', 'N', 'Y', 'V', 'T', 'S', 'N', 'P'};

enum {
    SNAP_CURSOR_VISIBLE = 1u << 0,
    SNAP_ALT_SCREEN = 1u << 1,
    SNAP_BRACKETED = 1u << 2,
    SNAP_APP_CURSOR = 1u << 3,
    SNAP_ALL_FLAGS = SNAP_CURSOR_VISIBLE | SNAP_ALT_SCREEN | SNAP_BRACKETED | SNAP_APP_CURSOR,
};

static void put_u16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put_u32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (i * 8));
}

static void put_u64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (i * 8));
}

static uint16_t get_u16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t get_u32(const unsigned char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (i * 8);
    return v;
}

static uint64_t get_u64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

size_t vt_snapshot_size(const vt *t) {
    if (!t || t->cols < 1 || t->rows < 1) return 0;
    size_t cells = (size_t)t->cols * (size_t)t->rows;
    size_t title_len = strlen(t->title), cwd_len = strlen(t->cwd);
    if (cells > VT_SNAPSHOT_MAX_CELLS || title_len >= sizeof t->title ||
        cwd_len >= sizeof t->cwd || cells > (SIZE_MAX - VT_SNAPSHOT_HEADER - title_len - cwd_len) /
                                      VT_SNAPSHOT_CELL)
        return 0;
    return VT_SNAPSHOT_HEADER + title_len + cwd_len + cells * VT_SNAPSHOT_CELL;
}

int vt_snapshot_write(const vt *t, void *buf, size_t cap, size_t *written) {
    size_t need = vt_snapshot_size(t);
    if (written) *written = need;
    if (need == 0) {
        errno = EOVERFLOW;
        return -1;
    }
    if (!buf || cap < need) {
        errno = ENOSPC;
        return -1;
    }
    unsigned char *p = buf;
    size_t title_len = strlen(t->title), cwd_len = strlen(t->cwd);
    size_t cells = (size_t)t->cols * (size_t)t->rows;
    memcpy(p, snapshot_magic, sizeof snapshot_magic);
    put_u16(p + 8, VT_SNAPSHOT_VERSION);
    put_u16(p + 10, VT_SNAPSHOT_HEADER);
    put_u32(p + 12, (uint32_t)need);
    put_u32(p + 16, (uint32_t)t->cols);
    put_u32(p + 20, (uint32_t)t->rows);
    put_u32(p + 24, (uint32_t)t->cx);
    put_u32(p + 28, (uint32_t)t->cy);
    put_u64(p + 32, t->generation);
    uint32_t flags = (t->cursor_visible ? SNAP_CURSOR_VISIBLE : 0u) |
                     (t->alt_on ? SNAP_ALT_SCREEN : 0u) |
                     (t->bracketed ? SNAP_BRACKETED : 0u) |
                     (t->app_cursor ? SNAP_APP_CURSOR : 0u);
    put_u32(p + 40, flags);
    put_u16(p + 44, (uint16_t)title_len);
    put_u16(p + 46, (uint16_t)cwd_len);
    put_u32(p + 48, (uint32_t)cells);
    put_u16(p + 52, VT_SNAPSHOT_CELL);
    put_u16(p + 54, 0);
    p += VT_SNAPSHOT_HEADER;
    memcpy(p, t->title, title_len);
    p += title_len;
    memcpy(p, t->cwd, cwd_len);
    p += cwd_len;
    for (size_t i = 0; i < cells; i++, p += VT_SNAPSHOT_CELL) {
        const vt_cell *c = &t->grid[i];
        put_u32(p, c->cp);
        put_u32(p + 4, c->combine);
        put_u32(p + 8, c->fg);
        put_u32(p + 12, c->bg);
        put_u16(p + 16, c->attrs);
    }
    return 0;
}

static bool scalar_or_zero(uint32_t cp) {
    return cp == 0 || (cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF));
}

static bool valid_color(uint32_t c) {
    uint32_t tag = VT_COLOR_TAG(c), value = VT_COLOR_VAL(c);
    if (tag == VT_COLOR_DEFAULT) return value == 0;
    if (tag == VT_COLOR_IDX) return value <= 255;
    return tag == VT_COLOR_RGB;
}

static bool valid_metadata(const unsigned char *s, size_t n, bool path) {
    if (path && (n == 0 || s[0] != '/')) return n == 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] == 0 || (path && (s[i] < 0x20 || s[i] == 0x7f))) return false;
    return true;
}

static bool valid_grid(const vt_cell *grid, int cols, int rows) {
    const uint16_t all_attrs = VT_ATTR_BOLD | VT_ATTR_FAINT | VT_ATTR_ITALIC | VT_ATTR_UNDERLINE |
                               VT_ATTR_BLINK | VT_ATTR_REVERSE | VT_ATTR_HIDDEN | VT_ATTR_STRIKE |
                               VT_ATTR_WIDE | VT_ATTR_WIDE_CONT;
    size_t cells = (size_t)cols * (size_t)rows;
    for (size_t i = 0; i < cells; i++) {
        const vt_cell *c = &grid[i];
        if (!scalar_or_zero(c->cp) || !scalar_or_zero(c->combine) ||
            (c->combine && (!c->cp || vt_cp_width(c->combine) != 0)) || !valid_color(c->fg) ||
            !valid_color(c->bg) || (c->attrs & (uint16_t)~all_attrs) ||
            ((c->attrs & VT_ATTR_WIDE) && (c->attrs & VT_ATTR_WIDE_CONT)) ||
            (c->cp && vt_cp_width(c->cp) == 2 && !(c->attrs & VT_ATTR_WIDE)))
            return false;
        if (c->attrs & VT_ATTR_WIDE) {
            if (!c->cp || vt_cp_width(c->cp) != 2 || i % (size_t)cols == (size_t)cols - 1)
                return false;
            const vt_cell *next = &grid[i + 1];
            if (!(next->attrs & VT_ATTR_WIDE_CONT) || next->cp || next->combine ||
                next->fg != c->fg || next->bg != c->bg ||
                (next->attrs & (uint16_t)~VT_ATTR_WIDE_CONT) !=
                    (c->attrs & (uint16_t)~VT_ATTR_WIDE))
                return false;
        } else if (c->attrs & VT_ATTR_WIDE_CONT) {
            if (i % (size_t)cols == 0 || !(grid[i - 1].attrs & VT_ATTR_WIDE)) return false;
        }
    }
    return true;
}

static void free_contents(vt *t) {
    if (t->sb) sb_free_all(t);
    free(t->sb);
    free(t->main_grid);
    free(t->alt_grid);
}

int vt_snapshot_read(vt *t, const void *buf, size_t len) {
    if (!t || !buf || len < VT_SNAPSHOT_HEADER) goto invalid;
    const unsigned char *p = buf;
    if (memcmp(p, snapshot_magic, sizeof snapshot_magic) != 0 ||
        get_u16(p + 8) != VT_SNAPSHOT_VERSION || get_u16(p + 10) != VT_SNAPSHOT_HEADER ||
        get_u32(p + 12) != len || get_u16(p + 52) != VT_SNAPSHOT_CELL || get_u16(p + 54) != 0)
        goto invalid;
    uint32_t cols = get_u32(p + 16), rows = get_u32(p + 20);
    uint32_t cx = get_u32(p + 24), cy = get_u32(p + 28), flags = get_u32(p + 40);
    uint32_t cells = get_u32(p + 48);
    size_t title_len = get_u16(p + 44), cwd_len = get_u16(p + 46);
    if (cols == 0 || rows == 0 || cols > VT_SNAPSHOT_MAX_DIM || rows > VT_SNAPSHOT_MAX_DIM ||
        cx >= cols || cy >= rows || flags & ~SNAP_ALL_FLAGS || cells != cols * rows ||
        title_len >= sizeof t->title || cwd_len >= sizeof t->cwd ||
        cells > (SIZE_MAX - VT_SNAPSHOT_HEADER - title_len - cwd_len) / VT_SNAPSHOT_CELL)
        goto invalid;
    size_t expected = VT_SNAPSHOT_HEADER + title_len + cwd_len + (size_t)cells * VT_SNAPSHOT_CELL;
    if (expected != len) goto invalid;
    const unsigned char *title = p + VT_SNAPSHOT_HEADER;
    const unsigned char *cwd = title + title_len;
    if (!valid_metadata(title, title_len, false) || !valid_metadata(cwd, cwd_len, true)) goto invalid;

    vt *fresh = vt_new((int)cols, (int)rows, t->sb_max);
    if (!fresh) {
        errno = ENOMEM;
        return -1;
    }
    fresh->alt_on = (flags & SNAP_ALT_SCREEN) != 0;
    fresh->grid = fresh->alt_on ? fresh->alt_grid : fresh->main_grid;
    fresh->cursor_visible = (flags & SNAP_CURSOR_VISIBLE) != 0;
    fresh->bracketed = (flags & SNAP_BRACKETED) != 0;
    fresh->app_cursor = (flags & SNAP_APP_CURSOR) != 0;
    fresh->cx = (int)cx;
    fresh->cy = (int)cy;
    fresh->saved_cx = fresh->cx;
    fresh->saved_cy = fresh->cy;
    fresh->generation = get_u64(p + 32);
    memcpy(fresh->title, title, title_len);
    fresh->title[title_len] = '\0';
    memcpy(fresh->cwd, cwd, cwd_len);
    fresh->cwd[cwd_len] = '\0';
    p = cwd + cwd_len;
    for (size_t i = 0; i < cells; i++, p += VT_SNAPSHOT_CELL) {
        vt_cell *c = &fresh->grid[i];
        c->cp = get_u32(p);
        c->combine = get_u32(p + 4);
        c->fg = get_u32(p + 8);
        c->bg = get_u32(p + 12);
        c->attrs = get_u16(p + 16);
    }
    if (!valid_grid(fresh->grid, fresh->cols, fresh->rows)) {
        vt_free(fresh);
        goto invalid;
    }

    vt old = *t;
    vt_write_cb respond_cb = old.respond, gfx_cb = old.gfx_cb;
    void *respond_user = old.respond_user, *gfx_user = old.gfx_user;
    *t = *fresh;
    t->respond = respond_cb;
    t->respond_user = respond_user;
    t->gfx_cb = gfx_cb;
    t->gfx_user = gfx_user;
    free(fresh); /* allocations now belong to t */
    free_contents(&old);
    return 0;

invalid:
    errno = EINVAL;
    return -1;
}

/* ---- getters --------------------------------------------------------- */

int vt_cols(const vt *t) { return t->cols; }
int vt_rows(const vt *t) { return t->rows; }
int vt_cursor_x(const vt *t) { return t->cx; }
int vt_cursor_y(const vt *t) { return t->cy; }
bool vt_cursor_visible(const vt *t) { return t->cursor_visible; }
bool vt_alt_screen(const vt *t) { return t->alt_on; }
bool vt_bracketed_paste(const vt *t) { return t->bracketed; }
bool vt_app_cursor(const vt *t) { return t->app_cursor; }
const char *vt_title(const vt *t) { return t->title; }
const char *vt_cwd(const vt *t) { return t->cwd; }
uint64_t vt_generation(const vt *t) { return t->generation; }
int vt_graphics_count(const vt *t) { return t->gfx_count; }

const vt_cell *vt_line(const vt *t, int row) {
    if (row < 0 || row >= t->rows) return NULL;
    return &t->grid[(size_t)row * t->cols];
}

int vt_scrollback_len(const vt *t) { return t->sb_len; }

const vt_cell *vt_scrollback_line(const vt *t, int idx, int *cols_out) {
    if (idx < 0 || idx >= t->sb_len) return NULL;
    const sb_line *l = &t->sb[(t->sb_head + idx) % t->sb_max];
    if (cols_out) *cols_out = l->cols;
    return l->cells;
}

static size_t utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

size_t vt_line_text(const vt *t, int row, char *buf, size_t cap) {
    const vt_cell *line = vt_line(t, row);
    if (!line || cap == 0) {
        if (cap) buf[0] = '\0';
        return 0;
    }
    size_t len = 0, last_glyph_end = 0;
    for (int x = 0; x < t->cols; x++) {
        const vt_cell *c = &line[x];
        if (c->attrs & VT_ATTR_WIDE_CONT) continue;
        char tmp[8];
        size_t n = utf8_encode(c->cp ? c->cp : ' ', tmp);
        if (c->cp && c->combine) n += utf8_encode(c->combine, tmp + n);
        if (len + n + 1 > cap) break;
        memcpy(buf + len, tmp, n);
        len += n;
        if (c->cp && c->cp != ' ') last_glyph_end = len;
    }
    buf[last_glyph_end] = '\0';
    return last_glyph_end;
}
