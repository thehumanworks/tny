/* keys.h — NSEvent-free key translation (docs/adr/0005).
 *
 * Platform window code classifies a key press into (tt_key, modifiers,
 * committed text) and this pure function turns it into the bytes a pty
 * expects. No platform headers, so it unit-tests on every host. */
#ifndef TNYTTY_UI_KEYS_H
#define TNYTTY_UI_KEYS_H

#include <stdbool.h>
#include <stddef.h>

enum {
    TT_MOD_SHIFT = 1 << 0,
    TT_MOD_ALT = 1 << 1, /* Option, sent as an ESC prefix (Meta) */
    TT_MOD_CTRL = 1 << 2,
    TT_MOD_SUPER = 1 << 3, /* Command; never reaches the pty */
};

typedef enum {
    TT_KEY_TEXT = 0, /* no special key: use the committed text */
    TT_KEY_ENTER,
    TT_KEY_TAB,
    TT_KEY_BACKSPACE,
    TT_KEY_ESCAPE,
    TT_KEY_UP,
    TT_KEY_DOWN,
    TT_KEY_RIGHT,
    TT_KEY_LEFT,
    TT_KEY_HOME,
    TT_KEY_END,
    TT_KEY_PAGE_UP,
    TT_KEY_PAGE_DOWN,
    TT_KEY_INSERT,
    TT_KEY_DELETE,
    TT_KEY_F1,
    TT_KEY_F2,
    TT_KEY_F3,
    TT_KEY_F4,
    TT_KEY_F5,
    TT_KEY_F6,
    TT_KEY_F7,
    TT_KEY_F8,
    TT_KEY_F9,
    TT_KEY_F10,
    TT_KEY_F11,
    TT_KEY_F12,
} tt_key;

/* Window commands bound to Command chords (docs/adr/0006,
 * docs/cli.md). Decoding is here, with the rest of the key translation,
 * so it is platform-free and unit-tested: `window_macos.c` asks whether
 * a Command press is one of ours before it consumes it, and `gui.c` asks
 * what it means. */
typedef enum {
    TT_CHORD_NONE = 0,
    TT_CHORD_COPY,        /* Cmd-C */
    TT_CHORD_PASTE,       /* Cmd-V */
    TT_CHORD_SPLIT_VERT,  /* Cmd-D: a new pane to the right */
    TT_CHORD_SPLIT_HORZ,  /* Cmd-Shift-D: a new pane below */
    TT_CHORD_CLOSE_PANE,  /* Cmd-W */
    TT_CHORD_FOCUS_LEFT,  /* Cmd-Opt-Left */
    TT_CHORD_FOCUS_RIGHT, /* Cmd-Opt-Right */
    TT_CHORD_FOCUS_UP,    /* Cmd-Opt-Up */
    TT_CHORD_FOCUS_DOWN,  /* Cmd-Opt-Down */
    TT_CHORD_FOCUS_PREV,  /* Cmd-[ : the previous pane in leaf order */
    TT_CHORD_FOCUS_NEXT,  /* Cmd-] : the next pane in leaf order */
} tt_chord;

/* text/text_len is the unmodified character of the press (macOS
 * `charactersIgnoringModifiers`), so Cmd-Shift-D arrives as "D" or "d"
 * and both decode the same; the Shift bit in mods is what distinguishes
 * the two splits. Returns TT_CHORD_NONE when the press is not a chord,
 * including every press without Command. */
tt_chord tt_key_chord(tt_key key, const char *text, size_t text_len, unsigned mods);

/* Encode one key press into out (not NUL-terminated). Returns the number
 * of bytes written, or 0 when the press produces nothing. text/text_len
 * is the UTF-8 the platform committed for the press (may be empty).
 * app_cursor selects SS3 over CSI for arrows/Home/End (DECCKM). */
size_t tt_key_encode(tt_key key, const char *text, size_t text_len, unsigned mods, bool app_cursor,
                     char *out, size_t cap);

#endif
