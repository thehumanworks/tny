/* vt.h — tnytty's headless terminal state machine (docs/adr/0001).
 *
 * Pure library: vt_feed() consumes bytes the child program wrote, the
 * getters read the resulting screen. No I/O — answers to terminal
 * queries (DSR/DA) and kitty graphics passthrough leave through
 * caller-provided callbacks. */
#ifndef TNYTTY_VT_H
#define TNYTTY_VT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    VT_ATTR_BOLD = 1 << 0,
    VT_ATTR_FAINT = 1 << 1,
    VT_ATTR_ITALIC = 1 << 2,
    VT_ATTR_UNDERLINE = 1 << 3,
    VT_ATTR_BLINK = 1 << 4,
    VT_ATTR_REVERSE = 1 << 5,
    VT_ATTR_HIDDEN = 1 << 6,
    VT_ATTR_STRIKE = 1 << 7,
    VT_ATTR_WIDE = 1 << 8,      /* lead cell of a double-width glyph */
    VT_ATTR_WIDE_CONT = 1 << 9, /* continuation cell of a double-width glyph */
};

/* Tagged colors: the high byte says which namespace the low bytes are in. */
#define VT_COLOR_DEFAULT 0x00000000u
#define VT_COLOR_IDX     0x01000000u /* | palette index 0..255 */
#define VT_COLOR_RGB     0x02000000u /* | rrggbb */
#define VT_COLOR_TAG(c)  ((c) & 0xff000000u)
#define VT_COLOR_VAL(c)  ((c) & 0x00ffffffu)

typedef struct {
    uint32_t cp;      /* base codepoint; 0 = blank */
    uint32_t combine; /* one trailing combining mark, 0 = none */
    uint32_t fg, bg;
    uint16_t attrs;
} vt_cell;

typedef struct vt vt;

typedef void (*vt_write_cb)(void *user, const char *bytes, size_t len);

vt *vt_new(int cols, int rows, int scrollback_max);
void vt_free(vt *t);

/* Answers the emulated program asked for (cursor position report, device
 * attributes); the adapter writes them to the pty. */
void vt_set_respond(vt *t, vt_write_cb cb, void *user);
/* Complete kitty graphics APC sequences, re-framed ESC_ ... ESC\, for live
 * passthrough to an attached renderer (docs/adr/0003). */
void vt_set_graphics(vt *t, vt_write_cb cb, void *user);

/* Feed child output. Incremental: any split of the same bytes yields the
 * same state (enforced by tests/test_vt.c). */
void vt_feed(vt *t, const char *bytes, size_t len);
void vt_resize(vt *t, int cols, int rows);

int vt_cols(const vt *t);
int vt_rows(const vt *t);
int vt_cursor_x(const vt *t);
int vt_cursor_y(const vt *t);
bool vt_cursor_visible(const vt *t);
bool vt_alt_screen(const vt *t);
bool vt_bracketed_paste(const vt *t);
/* DECCKM: arrow/Home/End keys should send SS3 rather than CSI. */
bool vt_app_cursor(const vt *t);
const char *vt_title(const vt *t);
int vt_graphics_count(const vt *t);

const vt_cell *vt_line(const vt *t, int row);
/* UTF-8 text of one row, trailing blanks trimmed. Returns bytes written
 * (excluding the NUL); buf should hold at least cols * 8 + 1. */
size_t vt_line_text(const vt *t, int row, char *buf, size_t cap);

int vt_scrollback_len(const vt *t);
/* idx 0 = oldest retained line. *cols_out gets the line's width when the
 * line was pushed (resizes do not rewrite history). */
const vt_cell *vt_scrollback_line(const vt *t, int idx, int *cols_out);

/* Display width of a codepoint: 0, 1, or 2 (docs/adr/0004: PUA is 1,
 * never locale wcwidth). */
int vt_cp_width(uint32_t cp);

#endif
