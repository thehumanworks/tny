#include "ui/selection.h"

#include <string.h>

void tt_sel_clear(tt_selection *s) { memset(s, 0, sizeof *s); }

bool tt_sel_active(const tt_selection *s) { return s->mode != TT_SEL_NONE; }

static const vt_cell *row_cells(const vt *t, int row) { return vt_line(t, row); }

/* A cell counts as part of a word when it is not blank and not a
 * separator; everything else is its own single-cell "word". */
static bool word_char(const vt_cell *c) {
    uint32_t cp = c->cp;
    if (!cp || cp == ' ' || cp == '\t') return false;
    if (cp < 128) {
        return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
               cp == '_' || cp == '-' || cp == '.' || cp == '/' || cp == '~' || cp == '+' ||
               cp == ':' || cp == '@' || cp == '%' || cp == '=' || cp == ',' || cp == '#';
    }
    return true; /* any non-ASCII printable is word material */
}

void tt_sel_word_at(const vt *t, int col, int row, int *c0, int *c1) {
    int cols = vt_cols(t);
    if (col < 0) col = 0;
    if (col >= cols) col = cols - 1;
    const vt_cell *line = row_cells(t, row);
    if (!line || cols < 1) {
        *c0 = col;
        *c1 = col + 1;
        return;
    }
    /* Land on the lead cell of a wide glyph before scanning. */
    if (line[col].attrs & VT_ATTR_WIDE_CONT && col > 0) col--;
    if (!word_char(&line[col])) {
        *c0 = col;
        *c1 = col + ((line[col].attrs & VT_ATTR_WIDE) ? 2 : 1);
        return;
    }
    int a = col, b = col;
    while (a > 0 && word_char(&line[a - 1])) a--;
    while (b + 1 < cols && word_char(&line[b + 1])) b++;
    if (line[b].attrs & VT_ATTR_WIDE) b++;
    *c0 = a;
    *c1 = b + 1;
}

/* Widen a column range so no wide glyph is cut in half. */
static void snap_wide(const vt *t, int row, int *c0, int *c1) {
    const vt_cell *line = row_cells(t, row);
    int cols = vt_cols(t);
    if (!line) return;
    if (*c0 > 0 && *c0 < cols && (line[*c0].attrs & VT_ATTR_WIDE_CONT)) (*c0)--;
    int last = *c1 - 1;
    if (last >= 0 && last < cols && (line[last].attrs & VT_ATTR_WIDE)) (*c1)++;
}

static void unit_at(tt_selection *s, const vt *t, int col, int row, int *c0, int *c1) {
    int cols = vt_cols(t);
    switch (s->mode) {
    case TT_SEL_LINE:
        *c0 = 0;
        *c1 = cols;
        break;
    case TT_SEL_WORD: tt_sel_word_at(t, col, row, c0, c1); break;
    default:
        *c0 = col;
        *c1 = col + 1;
        snap_wide(t, row, c0, c1);
        break;
    }
}

static void clamp(const vt *t, int *col, int *row) {
    int cols = vt_cols(t), rows = vt_rows(t);
    if (*col < 0) *col = 0;
    if (*col > cols - 1) *col = cols - 1;
    if (*row < 0) *row = 0;
    if (*row > rows - 1) *row = rows - 1;
}

void tt_sel_begin(tt_selection *s, const vt *t, int col, int row, int clicks) {
    tt_sel_clear(s);
    clamp(t, &col, &row);
    int mode = ((clicks < 1 ? 1 : clicks) - 1) % 3 + 1;
    s->mode = (tt_sel_mode)mode;
    s->dragging = true;
    s->pivot_row = row;
    unit_at(s, t, col, row, &s->pivot_c0, &s->pivot_c1);
    s->start_row = s->end_row = row;
    s->start_col = s->pivot_c0;
    s->end_col = s->pivot_c1 - 1;
}

void tt_sel_extend(tt_selection *s, const vt *t, int col, int row) {
    if (s->mode == TT_SEL_NONE) return;
    clamp(t, &col, &row);
    int c0 = 0, c1 = 0;
    unit_at(s, t, col, row, &c0, &c1);
    bool forward = row > s->pivot_row || (row == s->pivot_row && c1 >= s->pivot_c1);
    if (forward) {
        s->start_row = s->pivot_row;
        s->start_col = s->pivot_c0;
        s->end_row = row;
        s->end_col = c1 - 1;
    } else {
        s->start_row = row;
        s->start_col = c0;
        s->end_row = s->pivot_row;
        s->end_col = s->pivot_c1 - 1;
    }
}

bool tt_sel_finish(tt_selection *s, const vt *t) {
    s->dragging = false;
    if (s->mode == TT_SEL_NONE) return false;
    /* A click with no drag in character mode selects nothing: it is how
     * you dismiss a selection. */
    if (s->mode == TT_SEL_CHAR && s->start_row == s->end_row && s->start_col == s->end_col &&
        tt_sel_text(s, t, NULL, 0) == 0) {
        tt_sel_clear(s);
        return false;
    }
    return true;
}

bool tt_sel_row_span(const tt_selection *s, int row, int cols, int *c0, int *c1) {
    if (s->mode == TT_SEL_NONE || row < s->start_row || row > s->end_row) return false;
    int a = row == s->start_row ? s->start_col : 0;
    int b = row == s->end_row ? s->end_col + 1 : cols;
    if (a < 0) a = 0;
    if (b > cols) b = cols;
    if (a >= b) return false;
    *c0 = a;
    *c1 = b;
    return true;
}

static size_t utf8_put(uint32_t cp, char *buf, size_t cap, size_t used) {
    char tmp[4];
    size_t n;
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xc0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3f));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xe0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        tmp[2] = (char)(0x80 | (cp & 0x3f));
        n = 3;
    } else {
        tmp[0] = (char)(0xf0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        tmp[3] = (char)(0x80 | (cp & 0x3f));
        n = 4;
    }
    if (buf && cap && used + n < cap) memcpy(buf + used, tmp, n);
    return n;
}

size_t tt_sel_text(const tt_selection *s, const vt *t, char *buf, size_t cap) {
    size_t used = 0;
    if (buf && cap) buf[0] = '\0';
    if (s->mode == TT_SEL_NONE) return 0;
    int cols = vt_cols(t);
    for (int row = s->start_row; row <= s->end_row; row++) {
        int c0 = 0, c1 = 0;
        if (!tt_sel_row_span(s, row, cols, &c0, &c1)) continue;
        const vt_cell *line = row_cells(t, row);
        if (!line) continue;
        /* Trailing blanks are padding, not content: drop them per row. */
        while (c1 > c0 && (line[c1 - 1].cp == 0 || line[c1 - 1].cp == ' ')) c1--;
        for (int col = c0; col < c1; col++) {
            if (line[col].attrs & VT_ATTR_WIDE_CONT) continue;
            uint32_t cp = line[col].cp ? line[col].cp : ' ';
            used += utf8_put(cp, buf, cap, used);
            if (line[col].combine) used += utf8_put(line[col].combine, buf, cap, used);
        }
        if (row != s->end_row) {
            if (buf && cap && used + 1 < cap) buf[used] = '\n';
            used++;
        }
    }
    if (buf && cap) buf[used < cap ? used : cap - 1] = '\0';
    return used;
}
