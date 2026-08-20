/* tui_input.c — key decoding, the composer line editor, prompt history and
 * popover selection. Escape sequences are decoded from a byte stream, so a
 * split CSI across two read()s is re-joined instead of leaking as literals. */
#include "tui/tui.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    K_NONE = 0, K_CHAR, K_ENTER, K_NEWLINE, K_BS, K_DEL, K_LEFT, K_RIGHT,
    K_UP, K_DOWN, K_HOME, K_END, K_WLEFT, K_WRIGHT, K_WBS, K_KILL_EOL,
    K_KILL_BOL, K_ESC, K_TAB, K_CTRLC, K_CTRLD, K_CTRLL, K_CTRLO, K_CTRLX
};

/* ---- composer primitives ---- */

static void ins(tui *t, const char *s, size_t n) {
    if (t->input.len + n > 1u << 20) return; /* paste guard */
    buf_reserve(&t->input, n);
    memmove(t->input.data + t->cur + n, t->input.data + t->cur,
            t->input.len - t->cur);
    memcpy(t->input.data + t->cur, s, n);
    t->input.len += n;
    t->input.data[t->input.len] = 0;
    t->cur += n;
}

static void del_range(tui *t, size_t from, size_t to) {
    if (from >= to || to > t->input.len) return;
    memmove(t->input.data + from, t->input.data + to, t->input.len - to);
    t->input.len -= to - from;
    t->input.data[t->input.len] = 0;
    if (t->cur > to) t->cur -= to - from;
    else if (t->cur > from) t->cur = from;
}

static size_t prev_ch(const tui *t, size_t i) {
    if (i == 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)t->input.data[i] & 0xC0) == 0x80) i--;
    return i;
}

static size_t next_ch(const tui *t, size_t i) {
    if (i >= t->input.len) return t->input.len;
    i++;
    while (i < t->input.len && ((unsigned char)t->input.data[i] & 0xC0) == 0x80) i++;
    return i;
}

static bool is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/';
}

static size_t word_left(const tui *t, size_t i) {
    while (i > 0 && !is_word(t->input.data[i - 1])) i--;
    while (i > 0 && is_word(t->input.data[i - 1])) i--;
    return i;
}

static size_t word_right(const tui *t, size_t i) {
    while (i < t->input.len && !is_word(t->input.data[i])) i++;
    while (i < t->input.len && is_word(t->input.data[i])) i++;
    return i;
}

static size_t line_start(const tui *t, size_t i) {
    while (i > 0 && t->input.data[i - 1] != '\n') i--;
    return i;
}

static size_t line_end(const tui *t, size_t i) {
    while (i < t->input.len && t->input.data[i] != '\n') i++;
    return i;
}

static void set_input(tui *t, const char *s) {
    buf_clear(&t->input);
    if (s) buf_appends(&t->input, s);
    t->cur = t->input.len;
}

/* ---- popovers ---- */

void tui_pick_close(tui *t) {
    tui_items_clear(t);
    t->pick = PICK_NONE;
    t->sel = 0;
}

void tui_pick_refresh(tui *t) {
    if (t->approval) { tui_pick_close(t); return; }
    const char *d = t->input.len ? t->input.data : "";
    size_t cur = t->cur;

    /* slash palette: only while the whole draft is one unbroken /word */
    if (cur > 0 && d[0] == '/') {
        bool clean = true;
        for (size_t i = 1; i < cur; i++)
            if (d[i] == ' ' || d[i] == '\n') { clean = false; break; }
        if (clean && cur == t->input.len) {
            char *f = xstrndup(d + 1, cur - 1);
            pick_kind was = t->pick;
            tui_items_clear(t);
            tui_pick_build_cmd(t, f);
            free(f);
            t->pick = t->n_items ? PICK_CMD : PICK_NONE;
            t->pick_at = 0;
            if (was != t->pick || t->sel >= t->n_items) t->sel = 0;
            t->dirty = true;
            return;
        }
    }

    /* @ file / $ skill: trigger must start a token */
    size_t at = cur;
    char trig = 0;
    while (at > 0) {
        char c = d[at - 1];
        if (c == ' ' || c == '\n' || c == '\t') break;
        if (c == '@' || c == '$') {
            if (at - 1 == 0 || d[at - 2] == ' ' || d[at - 2] == '\n' || d[at - 2] == '\t') {
                trig = c;
                at--;
            }
            break;
        }
        at--;
    }
    if (trig) {
        char *f = xstrndup(d + at + 1, cur - at - 1);
        pick_kind want = trig == '@' ? PICK_FILE : PICK_SKILL;
        pick_kind was = t->pick;
        tui_items_clear(t);
        if (want == PICK_FILE) tui_pick_build_file(t, f);
        else tui_pick_build_skill(t, f);
        free(f);
        t->pick = t->n_items ? want : PICK_NONE;
        t->pick_at = at;
        if (was != t->pick || t->sel >= t->n_items) t->sel = 0;
        t->dirty = true;
        return;
    }
    if (t->pick != PICK_NONE) { tui_pick_close(t); t->dirty = true; }
}

/* Accept the highlighted item. run=true also submits (Enter on the palette). */
static void pick_accept(tui *t, bool run) {
    if (t->pick == PICK_NONE || t->sel >= t->n_items) return;
    const char *label = t->items[t->sel].label;
    pick_kind kind = t->pick;
    size_t at = t->pick_at;
    buf_t nb;
    buf_init(&nb);
    buf_append(&nb, t->input.data, at);
    if (kind == PICK_CMD) buf_appends(&nb, "/");
    buf_appends(&nb, label);
    if (kind != PICK_CMD) buf_appends(&nb, " ");
    buf_append(&nb, t->input.data + t->cur, t->input.len - t->cur);
    size_t newcur = at + strlen(label) + 1;
    tui_pick_close(t);
    buf_free(&t->input);
    buf_init(&t->input);
    buf_append(&t->input, nb.data, nb.len);
    buf_free(&nb);
    t->cur = newcur < t->input.len ? newcur : t->input.len;
    t->dirty = true;
    if (run && kind == PICK_CMD) {
        char *line = xstrdup(t->input.data);
        set_input(t, NULL);
        tui_submit(t, line);
        free(line);
    }
}

/* ---- key handling ---- */

static void submit_current(tui *t) {
    /* backslash continuation: "\" at the very end opens a new line */
    if (t->input.len && t->input.data[t->input.len - 1] == '\\') {
        t->input.data[t->input.len - 1] = '\n';
        t->cur = t->input.len;
        t->dirty = true;
        return;
    }
    char *line = t->input.len ? xstrdup(t->input.data) : xstrdup("");
    set_input(t, NULL);
    tui_pick_close(t);
    t->hist_pos = t->n_hist;
    t->dirty = true;
    tui_submit(t, line);
    free(line);
}

static void hist_move(tui *t, int dir) {
    if (dir < 0) {
        if (t->hist_pos <= 0) return;
        if (t->hist_pos == t->n_hist) {
            free(t->hist_draft);
            t->hist_draft = xstrdup(t->input.len ? t->input.data : "");
        }
        t->hist_pos--;
        set_input(t, t->hist[t->hist_pos]);
    } else {
        if (t->hist_pos >= t->n_hist) return;
        t->hist_pos++;
        set_input(t, t->hist_pos == t->n_hist ? (t->hist_draft ? t->hist_draft : "")
                                              : t->hist[t->hist_pos]);
    }
    t->dirty = true;
}

static void caret_line_move(tui *t, int dir) {
    size_t ls = line_start(t, t->cur);
    size_t col = t->cur - ls;
    if (dir < 0) {
        size_t pls = ls ? line_start(t, ls - 1) : 0;
        size_t ple = ls ? ls - 1 : 0;
        t->cur = pls + col < ple ? pls + col : ple;
    } else {
        size_t le = line_end(t, t->cur);
        size_t nls = le + 1;
        size_t nle = line_end(t, nls);
        t->cur = nls + col < nle ? nls + col : nle;
    }
    t->dirty = true;
}

static void do_key(tui *t, int k, const char *ch, size_t chlen) {
    bool popover = t->pick != PICK_NONE && t->n_items > 0;

    if (t->approval) {
        /* answered by the nested approval reader; ignore here */
        return;
    }

    switch (k) {
    case K_CHAR:
        ins(t, ch, chlen);
        t->dirty = true;
        tui_pick_refresh(t);
        break;
    case K_ENTER:
        if (popover) { pick_accept(t, true); break; }
        submit_current(t);
        break;
    case K_TAB:
        if (popover) pick_accept(t, false);
        break;
    case K_NEWLINE:
        ins(t, "\n", 1);
        t->dirty = true;
        break;
    case K_BS:
        if (t->cur > 0) {
            del_range(t, prev_ch(t, t->cur), t->cur);
            t->dirty = true;
            tui_pick_refresh(t);
        }
        break;
    case K_DEL:
        del_range(t, t->cur, next_ch(t, t->cur));
        t->dirty = true;
        tui_pick_refresh(t);
        break;
    case K_WBS:
        del_range(t, word_left(t, t->cur), t->cur);
        t->dirty = true;
        tui_pick_refresh(t);
        break;
    case K_LEFT:  t->cur = prev_ch(t, t->cur); t->dirty = true; tui_pick_refresh(t); break;
    case K_RIGHT: t->cur = next_ch(t, t->cur); t->dirty = true; tui_pick_refresh(t); break;
    case K_WLEFT: t->cur = word_left(t, t->cur); t->dirty = true; break;
    case K_WRIGHT: t->cur = word_right(t, t->cur); t->dirty = true; break;
    case K_HOME:  t->cur = line_start(t, t->cur); t->dirty = true; break;
    case K_END:   t->cur = line_end(t, t->cur); t->dirty = true; break;
    case K_KILL_EOL: del_range(t, t->cur, line_end(t, t->cur)); t->dirty = true; break;
    case K_KILL_BOL: del_range(t, line_start(t, t->cur), t->cur); t->dirty = true; break;
    case K_UP:
        if (popover) { if (t->sel > 0) t->sel--; t->dirty = true; break; }
        if (line_start(t, t->cur) > 0) caret_line_move(t, -1);
        else hist_move(t, -1);
        break;
    case K_DOWN:
        if (popover) { if (t->sel + 1 < t->n_items) t->sel++; t->dirty = true; break; }
        if (line_end(t, t->cur) < t->input.len) caret_line_move(t, 1);
        else hist_move(t, 1);
        break;
    case K_ESC:
        if (popover) { tui_pick_close(t); t->dirty = true; break; }
        if (t->overlay.len) { tui_overlay_clear(t); break; }
        if (t->turn_active) { tui_cancel_turn(t); break; }
        if (t->input.len) { set_input(t, NULL); t->dirty = true; }
        break;
    case K_CTRLC:
        if (popover) { tui_pick_close(t); t->dirty = true; break; }
        if (t->overlay.len) { tui_overlay_clear(t); break; }
        if (t->turn_active) { tui_cancel_turn(t); break; }
        if (t->input.len) { set_input(t, NULL); t->dirty = true; break; }
        if (t->last_ctrlc_ms && now_ms() - t->last_ctrlc_ms < 3000) {
            t->quit = true;
            t->exit_code = 130;
        } else {
            t->last_ctrlc_ms = now_ms();
            tui_note(t, "press ctrl-c again to exit");
        }
        break;
    case K_CTRLD:
        if (!t->input.len) t->quit = true;
        else { del_range(t, t->cur, next_ch(t, t->cur)); t->dirty = true; }
        break;
    case K_CTRLL:
        tui_raw_begin(t);
        fputs("\x1b[H\x1b[2J\x1b[3J", stdout);
        tui_raw_end(t);
        break;
    case K_CTRLO:
        tui_command(t, "/transcript");
        break;
    case K_CTRLX:
        tui_sys(t, "subagent manager: not available on this backend");
        break;
    default:
        break;
    }
}

/* Decode one key from p[0..n). Returns bytes consumed, 0 if it needs more. */
static size_t decode_one(tui *t, const char *p, size_t n, bool final) {
    unsigned char c = (unsigned char)p[0];

    if (c != 0x1b) {
        switch (c) {
        case '\r': case '\n': do_key(t, K_ENTER, NULL, 0); return 1;
        case 0x7f: case 0x08: do_key(t, K_BS, NULL, 0); return 1;
        case 0x01: do_key(t, K_HOME, NULL, 0); return 1;
        case 0x02: do_key(t, K_LEFT, NULL, 0); return 1;
        case 0x03: do_key(t, K_CTRLC, NULL, 0); return 1;
        case 0x04: do_key(t, K_CTRLD, NULL, 0); return 1;
        case 0x05: do_key(t, K_END, NULL, 0); return 1;
        case 0x06: do_key(t, K_RIGHT, NULL, 0); return 1;
        case 0x09: do_key(t, K_TAB, NULL, 0); return 1;
        case 0x0b: do_key(t, K_KILL_EOL, NULL, 0); return 1;
        case 0x0c: do_key(t, K_CTRLL, NULL, 0); return 1;
        case 0x0f: do_key(t, K_CTRLO, NULL, 0); return 1;
        case 0x15: do_key(t, K_KILL_BOL, NULL, 0); return 1;
        case 0x17: do_key(t, K_WBS, NULL, 0); return 1;
        case 0x18: do_key(t, K_CTRLX, NULL, 0); return 1;
        default: break;
        }
        if (c < 0x20) return 1; /* other control bytes: ignore */
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (n < len) { if (!final) return 0; len = n; }
        do_key(t, K_CHAR, p, len);
        return len;
    }

    if (n == 1) {
        if (!final) return 0;
        do_key(t, K_ESC, NULL, 0);
        return 1;
    }
    if (p[1] == '[') {
        size_t i = 2;
        while (i < n && !((unsigned char)p[i] >= 0x40 && (unsigned char)p[i] <= 0x7e)) i++;
        if (i >= n) return final ? n : 0;
        char fin = p[i];
        bool ctrl = false;
        for (size_t j = 2; j < i; j++)
            if (p[j] == ';' && j + 1 < i && (p[j + 1] == '5' || p[j + 1] == '3')) ctrl = true;
        size_t used = i + 1;
        switch (fin) {
        case 'A': do_key(t, K_UP, NULL, 0); break;
        case 'B': do_key(t, K_DOWN, NULL, 0); break;
        case 'C': do_key(t, ctrl ? K_WRIGHT : K_RIGHT, NULL, 0); break;
        case 'D': do_key(t, ctrl ? K_WLEFT : K_LEFT, NULL, 0); break;
        case 'H': do_key(t, K_HOME, NULL, 0); break;
        case 'F': do_key(t, K_END, NULL, 0); break;
        case 'Z': do_key(t, K_TAB, NULL, 0); break;
        case '~':
            if (i > 2 && p[2] == '3') do_key(t, K_DEL, NULL, 0);
            else if (i > 2 && (p[2] == '1' || p[2] == '7')) do_key(t, K_HOME, NULL, 0);
            else if (i > 2 && (p[2] == '4' || p[2] == '8')) do_key(t, K_END, NULL, 0);
            break;
        default: break;
        }
        return used;
    }
    if (p[1] == 'O') {
        if (n < 3) return final ? n : 0;
        switch (p[2]) {
        case 'A': do_key(t, K_UP, NULL, 0); break;
        case 'B': do_key(t, K_DOWN, NULL, 0); break;
        case 'C': do_key(t, K_RIGHT, NULL, 0); break;
        case 'D': do_key(t, K_LEFT, NULL, 0); break;
        case 'H': do_key(t, K_HOME, NULL, 0); break;
        case 'F': do_key(t, K_END, NULL, 0); break;
        default: break;
        }
        return 3;
    }
    switch (p[1]) {
    case '\r': case '\n': do_key(t, K_NEWLINE, NULL, 0); break;
    case 'b': do_key(t, K_WLEFT, NULL, 0); break;
    case 'f': do_key(t, K_WRIGHT, NULL, 0); break;
    case 0x7f: do_key(t, K_WBS, NULL, 0); break;
    default: break;
    }
    return 2;
}

static char g_kb[4096];
static size_t g_kn;

static bool decode_all(tui *t, bool final) {
    bool progress = false;
    while (g_kn) {
        size_t used = decode_one(t, g_kb, g_kn, final);
        if (!used) break;
        memmove(g_kb, g_kb + used, g_kn - used);
        g_kn -= used;
        progress = true;
    }
    return progress;
}

/* Dumb (non-tty) mode: no escapes, no editing, one prompt per line. */
static void dumb_feed(tui *t, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') {
            char *line = t->input.len ? xstrdup(t->input.data) : xstrdup("");
            set_input(t, NULL);
            tui_submit(t, line);
            free(line);
        } else if ((unsigned char)p[i] >= 0x20) {
            ins(t, p + i, 1);
        }
    }
}

int tui_read_input(tui *t) {
    char tmp[2048];
    ssize_t n = read(0, tmp, sizeof tmp);
    if (n == 0) return -1;
    if (n < 0) return (errno == EINTR || errno == EAGAIN) ? 0 : -1;

    if (!t->tty) { dumb_feed(t, tmp, (size_t)n); return (int)n; }

    if (g_kn + (size_t)n > sizeof g_kb) g_kn = 0; /* runaway: drop stale bytes */
    memcpy(g_kb + g_kn, tmp, (size_t)n);
    g_kn += (size_t)n;

    for (int tries = 0; g_kn && tries < 3; tries++) {
        decode_all(t, false);
        if (!g_kn) break;
        struct pollfd pf = {0, POLLIN, 0};
        if (poll(&pf, 1, 25) <= 0) break;
        ssize_t m = read(0, tmp, sizeof tmp);
        if (m <= 0) break;
        if (g_kn + (size_t)m > sizeof g_kb) g_kn = 0;
        memcpy(g_kb + g_kn, tmp, (size_t)m);
        g_kn += (size_t)m;
    }
    if (g_kn) decode_all(t, true);
    return (int)n;
}
