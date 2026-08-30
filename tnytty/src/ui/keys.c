#include "ui/keys.h"

#include <stdio.h>
#include <string.h>

/* xterm's modifier parameter: 1 + shift(1) + alt(2) + ctrl(4). */
static int mod_param(unsigned mods) {
    int m = 1;
    if (mods & TT_MOD_SHIFT) m += 1;
    if (mods & TT_MOD_ALT) m += 2;
    if (mods & TT_MOD_CTRL) m += 4;
    return m;
}

static size_t put(char *out, size_t cap, const char *s, size_t n) {
    if (n > cap) return 0;
    memcpy(out, s, n);
    return n;
}

/* CSI/SS3 sequence with a final letter: unmodified uses ss3 (when the
 * app-cursor flag asks) or a bare CSI; modified always uses CSI 1;m. */
static size_t seq_letter(char final, unsigned mods, bool ss3, char *out, size_t cap) {
    char buf[16];
    int n;
    int m = mod_param(mods);
    if (m != 1) n = snprintf(buf, sizeof buf, "\x1b[1;%d%c", m, final);
    else if (ss3) n = snprintf(buf, sizeof buf, "\x1bO%c", final);
    else n = snprintf(buf, sizeof buf, "\x1b[%c", final);
    return n > 0 ? put(out, cap, buf, (size_t)n) : 0;
}

/* CSI n ~ style ("tilde") keys. */
static size_t seq_tilde(int num, unsigned mods, char *out, size_t cap) {
    char buf[16];
    int n;
    int m = mod_param(mods);
    if (m != 1) n = snprintf(buf, sizeof buf, "\x1b[%d;%d~", num, m);
    else n = snprintf(buf, sizeof buf, "\x1b[%d~", num);
    return n > 0 ? put(out, cap, buf, (size_t)n) : 0;
}

/* Control bytes for the ASCII range: Ctrl-A..Z, Ctrl-@ / space, and the
 * Ctrl-[ \ ] ^ _ group. Returns 0 when the pair has no control byte. */
static char ctrl_byte(unsigned char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 1);
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 1);
    if (c == '[') return 0x1b;
    if (c == '\\') return 0x1c;
    if (c == ']') return 0x1d;
    if (c == '^') return 0x1e;
    if (c == '_' || c == '?') return 0x1f;
    if (c == '/') return 0x1f;
    return 0;
}

tt_chord tt_key_chord(tt_key key, const char *text, size_t text_len, unsigned mods) {
    if (!(mods & TT_MOD_SUPER)) return TT_CHORD_NONE;

    /* Cmd-Opt-arrow walks the split tree; the arrows carry no text. */
    if (mods == (TT_MOD_SUPER | TT_MOD_ALT)) {
        switch (key) {
        case TT_KEY_LEFT: return TT_CHORD_FOCUS_LEFT;
        case TT_KEY_RIGHT: return TT_CHORD_FOCUS_RIGHT;
        case TT_KEY_UP: return TT_CHORD_FOCUS_UP;
        case TT_KEY_DOWN: return TT_CHORD_FOCUS_DOWN;
        default: break;
        }
    }
    if (key != TT_KEY_TEXT || !text || text_len != 1) return TT_CHORD_NONE;
    char c = text[0];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (mods == TT_MOD_SUPER) {
        switch (c) {
        case 'c': return TT_CHORD_COPY;
        case 'v': return TT_CHORD_PASTE;
        case 'd': return TT_CHORD_SPLIT_VERT;
        case 'w': return TT_CHORD_CLOSE_PANE;
        case 'q': return TT_CHORD_QUIT;
        case 't': return TT_CHORD_NEW_TAB;
        /* Cycling has no geometry to fail on, so unlike the arrows it always
         * lands somewhere (docs/adr/0006). */
        case '[': return TT_CHORD_FOCUS_PREV;
        case ']': return TT_CHORD_FOCUS_NEXT;
        default:
            if (c >= '1' && c <= '9') return (tt_chord)(TT_CHORD_TAB_1 + c - '1');
            return TT_CHORD_NONE;
        }
    }
    if (mods == (TT_MOD_SUPER | TT_MOD_SHIFT)) {
        switch (c) {
        case 'd': return TT_CHORD_SPLIT_HORZ;
        case 'w': return TT_CHORD_CLOSE_TAB;
        case '[':
        case '{': return TT_CHORD_TAB_PREV;
        case ']':
        case '}': return TT_CHORD_TAB_NEXT;
        default: return TT_CHORD_NONE;
        }
    }
    return TT_CHORD_NONE;
}

int tt_chord_tab_index(tt_chord chord) {
    return chord >= TT_CHORD_TAB_1 && chord <= TT_CHORD_TAB_9 ? chord - TT_CHORD_TAB_1 : -1;
}

size_t tt_key_encode(tt_key key, const char *text, size_t text_len, unsigned mods, bool app_cursor,
                     char *out, size_t cap) {
    /* Command belongs to the window (Cmd-Q, Cmd-W); it never goes to the
     * child, and it suppresses whatever text the press committed. */
    if (mods & TT_MOD_SUPER) return 0;

    size_t len = 0;
    bool meta = (mods & TT_MOD_ALT) != 0;

    switch (key) {
    case TT_KEY_ENTER: return put(out, cap, meta ? "\x1b\r" : "\r", meta ? 2 : 1);
    case TT_KEY_TAB:
        if (mods & TT_MOD_SHIFT) return put(out, cap, "\x1b[Z", 3);
        return put(out, cap, meta ? "\x1b\t" : "\t", meta ? 2 : 1);
    case TT_KEY_BACKSPACE: return put(out, cap, meta ? "\x1b\x7f" : "\x7f", meta ? 2 : 1);
    case TT_KEY_ESCAPE: return put(out, cap, meta ? "\x1b\x1b" : "\x1b", meta ? 2 : 1);
    case TT_KEY_UP: return seq_letter('A', mods, app_cursor, out, cap);
    case TT_KEY_DOWN: return seq_letter('B', mods, app_cursor, out, cap);
    case TT_KEY_RIGHT: return seq_letter('C', mods, app_cursor, out, cap);
    case TT_KEY_LEFT: return seq_letter('D', mods, app_cursor, out, cap);
    case TT_KEY_HOME: return seq_letter('H', mods, app_cursor, out, cap);
    case TT_KEY_END: return seq_letter('F', mods, app_cursor, out, cap);
    case TT_KEY_INSERT: return seq_tilde(2, mods, out, cap);
    case TT_KEY_DELETE: return seq_tilde(3, mods, out, cap);
    case TT_KEY_PAGE_UP: return seq_tilde(5, mods, out, cap);
    case TT_KEY_PAGE_DOWN: return seq_tilde(6, mods, out, cap);
    case TT_KEY_F1: return seq_letter('P', mods, true, out, cap);
    case TT_KEY_F2: return seq_letter('Q', mods, true, out, cap);
    case TT_KEY_F3: return seq_letter('R', mods, true, out, cap);
    case TT_KEY_F4: return seq_letter('S', mods, true, out, cap);
    case TT_KEY_F5: return seq_tilde(15, mods, out, cap);
    case TT_KEY_F6: return seq_tilde(17, mods, out, cap);
    case TT_KEY_F7: return seq_tilde(18, mods, out, cap);
    case TT_KEY_F8: return seq_tilde(19, mods, out, cap);
    case TT_KEY_F9: return seq_tilde(20, mods, out, cap);
    case TT_KEY_F10: return seq_tilde(21, mods, out, cap);
    case TT_KEY_F11: return seq_tilde(23, mods, out, cap);
    case TT_KEY_F12: return seq_tilde(24, mods, out, cap);
    case TT_KEY_TEXT: break;
    }

    if (!text || !text_len) return 0;

    if (mods & TT_MOD_CTRL) {
        /* Ctrl applies to the unmodified ASCII character only; anything
         * else falls through as plain text. */
        unsigned char c = (unsigned char)text[0];
        if (text_len == 1) {
            char b;
            if (c == ' ' || c == '@') b = 0x00;
            else {
                b = ctrl_byte(c);
                if (!b) goto plain;
            }
            if (meta) {
                if (cap < 2) return 0;
                out[0] = 0x1b;
                out[1] = b;
                return 2;
            }
            return put(out, cap, &b, 1);
        }
    }

plain:
    if (meta) {
        if (cap < 1) return 0;
        out[0] = 0x1b;
        len = 1;
    }
    if (text_len > cap - len) return len;
    memcpy(out + len, text, text_len);
    return len + text_len;
}
