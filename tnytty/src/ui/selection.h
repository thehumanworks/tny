/* selection.h — mouse selection over the VT grid (docs/adr/0005).
 *
 * Platform-free: the window turns a click into a cell, this decides what
 * is selected and what text that means. Wide glyphs select whole (a
 * continuation cell pulls in its lead), lines are trimmed of trailing
 * blanks, and rows join with '\n'. */
#ifndef TNYTTY_UI_SELECTION_H
#define TNYTTY_UI_SELECTION_H

#include "vt/vt.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TT_SEL_NONE = 0,
    TT_SEL_CHAR, /* one click: character-wise */
    TT_SEL_WORD, /* double click: whole words */
    TT_SEL_LINE, /* triple click: whole rows */
} tt_sel_mode;

typedef struct {
    tt_sel_mode mode;
    bool dragging;
    /* The unit the drag started on (a cell, a word, or a row); extending
     * always covers this whole unit. */
    int pivot_row, pivot_c0, pivot_c1;
    /* Normalized inclusive range, start <= end in reading order. */
    int start_row, start_col;
    int end_row, end_col;
} tt_selection;

void tt_sel_clear(tt_selection *s);
bool tt_sel_active(const tt_selection *s);

/* clicks: 1 = character, 2 = word, 3 = line (higher wraps back to 1). */
void tt_sel_begin(tt_selection *s, const vt *t, int col, int row, int clicks);
void tt_sel_extend(tt_selection *s, const vt *t, int col, int row);
/* Ends the drag. Returns false when nothing ended up selected (a plain
 * click), in which case the selection is cleared. */
bool tt_sel_finish(tt_selection *s, const vt *t);

/* Columns [*c0, *c1) of row that are selected; false when none are. */
bool tt_sel_row_span(const tt_selection *s, int row, int cols, int *c0, int *c1);

/* Word under (col, row), as the half-open column range [*c0, *c1). */
void tt_sel_word_at(const vt *t, int col, int row, int *c0, int *c1);

/* UTF-8 text of the selection, NUL-terminated. Returns the length that
 * would be written excluding the NUL (so a caller can size a buffer by
 * passing cap 0); writes at most cap - 1 bytes plus the NUL. */
size_t tt_sel_text(const tt_selection *s, const vt *t, char *buf, size_t cap);

#endif
