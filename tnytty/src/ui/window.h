/* window.h — the native-window seam (docs/adr/0005, docs/platforms.md).
 *
 * Same rule as the pty seam: one implementation per platform behind this
 * header (window_macos.c; window_stub.c everywhere else, which returns a
 * clean "not supported" error). Nothing above this header knows AppKit
 * exists, and no platform #ifdef appears in render.c, keys.c, or gui.c.
 *
 * The window never runs a loop of its own: tt_window_pump() drains the
 * platform's queued events and returns, so the caller keeps the single
 * poll(2) loop that also owns the pty and the HTTP fds. */
#ifndef TNYTTY_UI_WINDOW_H
#define TNYTTY_UI_WINDOW_H

#include "ui/keys.h"
#include "ui/render.h"
#include "util/config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct tt_window tt_window;

typedef enum {
    TT_WIN_EV_NONE = 0,
    TT_WIN_EV_KEY,        /* key/mods/text */
    TT_WIN_EV_RESIZE,     /* the backing surface changed size */
    TT_WIN_EV_FOCUS,      /* focused */
    TT_WIN_EV_CLOSE,      /* the user closed the window or asked to quit */
    TT_WIN_EV_MOUSE_DOWN, /* px_x/px_y/clicks */
    TT_WIN_EV_MOUSE_DRAG,
    TT_WIN_EV_MOUSE_UP,
    TT_WIN_EV_CHORD, /* a Command chord the window did not handle itself */
} tt_win_ev_type;

typedef struct {
    tt_win_ev_type type;
    tt_key key;
    tt_chord chord; /* TT_WIN_EV_CHORD only */
    unsigned mods;  /* TT_MOD_* */
    char text[32];  /* UTF-8 committed by the press */
    size_t text_len;
    bool focused;
    /* Device pixels, top-left origin, always inside the surface: a press
     * AppKit reports outside the window (it uses a sentinel location for
     * synthesized activation clicks) is dropped, never clamped. */
    int px_x, px_y;
    int clicks; /* 1 = single, 2 = double, 3 = triple */
} tt_win_ev;

/* Open a window sized to cols x rows cells using cfg's font, padding and
 * titlebar style. Returns NULL with err filled (clean message, no
 * newline) when the platform has no window support. */
tt_window *tt_window_open(const tt_config *cfg, int cols, int rows, const char *title, char *err,
                          size_t errcap);
void tt_window_close(tt_window *w);

/* Rasterizer settings matching this window's font metrics and backing
 * scale: cell size in device pixels, padding (titlebar inset folded into
 * pad_top when the titlebar is transparent), colors, and the glyph
 * cache's lookup. */
void tt_window_render_config(tt_window *w, tt_render_config *out);
/* Current backing-store size in device pixels. */
void tt_window_surface(tt_window *w, int *px_w, int *px_h);

/* Drain queued platform events into ev, one per call. Returns true while
 * events remain. Never blocks. */
bool tt_window_pump(tt_window *w, tt_win_ev *ev);
/* Blit the framebuffer. y0..y1 is the dirty device-pixel band (y1
 * exclusive); an empty band presents nothing. */
void tt_window_present(tt_window *w, const uint32_t *px, int px_w, int px_h, int y0, int y1);
/* Shown in the titlebar when it is opaque; ignored otherwise. */
void tt_window_set_title(tt_window *w, const char *title);

/* System pasteboard, plain UTF-8 text. tt_window_clipboard returns a
 * string owned by the window, valid until the next call, or NULL when
 * the pasteboard holds no text. */
void tt_window_set_clipboard(tt_window *w, const char *utf8, size_t len);
const char *tt_window_clipboard(tt_window *w);

#endif
