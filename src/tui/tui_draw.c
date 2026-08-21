/* tui_draw.c — ANSI painting. No ncurses, no terminfo: the escapes used here
 * (CUU, CUF, ED, SGR) are in every terminal that claims VT100 ancestry. */
#include "tui/tui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

const char *tui_c(const tui *t, const char *code) { return t->color ? code : ""; }

static void wout(const void *s, size_t n) {
    if (n) fwrite(s, 1, n, stdout);
}

/* Append at most maxw columns of s, dropping control bytes. Returns columns. */
static int push_trunc(buf_t *b, const char *s, size_t n, int maxw) {
    int w = 0;
    for (size_t i = 0; i < n && w < maxw;) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\r') { i++; continue; }
        if (c == '\t') { buf_appends(b, " "); w++; i++; continue; }
        if (c < 0x20 || c == 0x7f) { i++; continue; }
        size_t j = i + 1;
        while (j < n && ((unsigned char)s[j] & 0xC0) == 0x80) j++;
        buf_append(b, s + i, j - i);
        w++;
        i = j;
    }
    return w;
}

int tui_push_ansi(buf_t *b, const char *s, size_t n, int maxw) {
    int w = 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) { /* pass SGR through at zero width, drop other escapes */
            size_t j = i + 1;
            if (j < n && s[j] == '[') {
                j++;
                while (j < n && !((unsigned char)s[j] >= 0x40 &&
                                  (unsigned char)s[j] <= 0x7e)) j++;
                if (j < n && s[j] == 'm') buf_append(b, s + i, j - i + 1);
                i = j < n ? j + 1 : n;
            } else {
                i = j;
            }
            continue;
        }
        if (w >= maxw) { i++; continue; } /* keep scanning: a reset may follow */
        if (c == '\n' || c == '\r') { i++; continue; }
        if (c == '\t') { buf_appends(b, " "); w++; i++; continue; }
        if (c < 0x20 || c == 0x7f) { i++; continue; }
        size_t j = i + 1;
        while (j < n && ((unsigned char)s[j] & 0xC0) == 0x80) j++;
        buf_append(b, s + i, j - i);
        w++;
        i = j;
    }
    return w;
}

void tui_size(tui *t) {
    struct winsize ws;
    t->rows = 24;
    t->cols = 80;
    if (t->tty && ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        t->cols = ws.ws_col;
        t->rows = ws.ws_row > 0 ? ws.ws_row : 24;
    }
    if (t->cols < 20) t->cols = 20;
}

static void erase_block(tui *t) {
    if (!t->tty || t->block_rows == 0) return;
    char esc[32];
    wout("\r", 1);
    if (t->cur_row > 0) {
        int n = snprintf(esc, sizeof esc, "\x1b[%dA", t->cur_row);
        wout(esc, (size_t)n);
    }
    wout("\x1b[J", 3);
    t->block_rows = 0;
    t->cur_row = 0;
}

static void row_sep(buf_t *b, int *rows) {
    if (*rows > 0) buf_appends(b, "\n");
    (*rows)++;
}

static void status_row(tui *t, buf_t *b, int maxw) {
    buf_t s;
    buf_init(&s);
    buf_appendf(&s, "%s  %s  %s", tny_backend_name((tny_backend_id)t->ctx->backend),
                t->ctx->model ? t->ctx->model : "default",
                tny_perm_mode_name(t->ctx->perm_mode));
    if (t->session) buf_appendf(&s, "  %s", t->session->id);
    if (t->in_tok || t->out_tok)
        buf_appendf(&s, "  %lld/%lld tok", (long long)t->in_tok, (long long)t->out_tok);
    if (t->n_images) buf_appendf(&s, "  %d img", t->n_images);
    if (t->note.len) buf_appendf(&s, "  %s", t->note.data);
    else if (t->turn_active) {
        static const char *frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                       "⠴", "⠦", "⠧", "⠇", "⠏"};
        buf_appendf(&s, "  %s working… (esc cancels)", frames[t->spin % 10]);
    } else buf_appendf(&s, "  %s", t->ctx->cwd);

    buf_appends(b, tui_c(t, "\x1b[7m"));
    int w = push_trunc(b, s.data, s.len, maxw);
    for (; w < maxw; w++) buf_appends(b, " ");
    buf_appends(b, tui_c(t, "\x1b[0m"));
    buf_free(&s);
}

static void popover_rows(tui *t, buf_t *b, int *rows, int maxw) {
    int first = 0;
    if (t->sel >= TUI_POP_ROWS) first = t->sel - TUI_POP_ROWS + 1;
    for (int i = first; i < t->n_items && i - first < TUI_POP_ROWS; i++) {
        row_sep(b, rows);
        bool on = i == t->sel;
        buf_appends(b, tui_c(t, on ? "\x1b[1;36m" : "\x1b[2m"));
        buf_t line;
        buf_init(&line);
        buf_appendf(&line, "%s %-22s %s", on ? "›" : " ", t->items[i].label,
                    t->items[i].hint ? t->items[i].hint : "");
        push_trunc(b, line.data, line.len, maxw);
        buf_free(&line);
        buf_appends(b, tui_c(t, "\x1b[0m"));
    }
}

/* The whole block must fit the screen or erase_block's cursor-up arithmetic
 * breaks, so the overlay only gets the rows everything else leaves over. */
int tui_wrap_width(const tui *t) {
    int avail = t->cols - 1 - 2;
    return avail < 8 ? 8 : avail;
}

static size_t cp_adv(const char *s, size_t n, size_t i) {
    if (i >= n) return 0;
    unsigned char c = (unsigned char)s[i];
    size_t a = 1;
    if (c >= 0xF0) a = 4;
    else if (c >= 0xE0) a = 3;
    else if (c >= 0xC0) a = 2;
    if (i + a > n) a = n - i;
    return a ? a : 1;
}

void tui_wrap_locate(const char *s, size_t n, size_t cur, int width,
                     int *row, int *col, int *total) {
    if (width < 1) width = 1;
    if (!s) s = "";
    int r = 0, c = 0, cr = 0, cc = 0;
    for (size_t i = 0; i < n; ) {
        if (i == cur) { cr = r; cc = c; }
        if (s[i] == '\n') {
            r++;
            c = 0;
            i++;
            continue;
        }
        size_t a = cp_adv(s, n, i);
        if (c >= width) { r++; c = 0; }
        c++;
        i += a;
    }
    if (cur >= n) { cr = r; cc = c; }
    if (row) *row = cr;
    if (col) *col = cc;
    if (total) *total = r + 1;
}

size_t tui_wrap_index(const char *s, size_t n, int width, int trow, int tcol) {
    if (width < 1) width = 1;
    if (!s || trow < 0) return 0;
    int r = 0, c = 0;
    for (size_t i = 0; i < n; ) {
        if (r == trow && c >= tcol) return i;
        if (s[i] == '\n') {
            if (r == trow) return i;
            r++;
            c = 0;
            i++;
            if (r > trow) return i;
            continue;
        }
        if (c >= width) {
            r++;
            c = 0;
            if (r > trow) return i;
            if (r == trow && tcol <= 0) return i;
        }
        i += cp_adv(s, n, i);
        c++;
    }
    return n;
}

int tui_overlay_budget(const tui *t) {
    int used = 1 /* status */ + (t->partial.len ? 1 : 0) + 1 /* slack */;
    if (t->approval) {
        used += 1;
    } else {
        int total = 1;
        tui_wrap_locate(t->input.data, t->input.len, 0, tui_wrap_width(t),
                        NULL, NULL, &total);
        used += total < TUI_COMP_ROWS ? total : TUI_COMP_ROWS;
    }
    if (t->pick != PICK_NONE && t->n_items > 0)
        used += t->n_items < TUI_POP_ROWS ? t->n_items : TUI_POP_ROWS;
    int budget = t->rows - used;
    return budget > 0 ? budget : 0;
}

static void overlay_rows(tui *t, buf_t *b, int *rows, int maxw) {
    int budget = tui_overlay_budget(t);
    if (budget <= 0) return;

    int total = 0;
    for (size_t i = 0; i < t->overlay.len; i++)
        if (t->overlay.data[i] == '\n') total++;
    int show = total <= budget ? total : budget - 1; /* keep a row for the "…" */
    if (show < 0) show = 0;

    const char *p = t->overlay.data;
    const char *end = p + t->overlay.len;
    for (int i = 0; i < show && p < end; i++) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        row_sep(b, rows);
        tui_push_ansi(b, p, ll, maxw);
        buf_appends(b, tui_c(t, "\x1b[0m"));
        p = nl ? nl + 1 : end;
    }
    if (show < total) {
        row_sep(b, rows);
        buf_appends(b, tui_c(t, "\x1b[2m"));
        buf_t m;
        buf_init(&m);
        buf_appendf(&m, "  … %d more rows (enlarge the window to see them)",
                    total - show);
        tui_push_ansi(b, m.data, m.len, maxw);
        buf_free(&m);
        buf_appends(b, tui_c(t, "\x1b[0m"));
    }
}

static void composer_rows(tui *t, buf_t *b, int *rows, int maxw, int *cur_row, int *cur_col) {
    if (t->approval) {
        row_sep(b, rows);
        *cur_row = *rows - 1;
        buf_appends(b, tui_c(t, "\x1b[1;33m"));
        const char *q = "approve? [y] yes  [a] yes, don't ask again  [n] no";
        *cur_col = push_trunc(b, q, strlen(q), maxw);
        buf_appends(b, tui_c(t, "\x1b[0m"));
        return;
    }

    const char *data = t->input.len ? t->input.data : "";
    size_t len = t->input.len;
    int avail = maxw - 2;
    if (avail < 8) avail = 8;
    int caret_row = 0, caret_col = 0, total = 1;
    tui_wrap_locate(data, len, t->cur, avail, &caret_row, &caret_col, &total);

    int first = 0;
    if (caret_row >= TUI_COMP_ROWS) first = caret_row - TUI_COMP_ROWS + 1;

    for (int vr = first; vr < total && vr - first < TUI_COMP_ROWS; vr++) {
        row_sep(b, rows);
        size_t ls = tui_wrap_index(data, len, avail, vr, 0);
        size_t le = vr + 1 < total ? tui_wrap_index(data, len, avail, vr + 1, 0) : len;
        if (le > ls && data[le - 1] == '\n') le--;
        buf_appends(b, tui_c(t, "\x1b[1;32m"));
        buf_appends(b, ls == 0 ? "> " : "  ");
        buf_appends(b, tui_c(t, "\x1b[0m"));
        if (vr == caret_row) {
            *cur_row = *rows - 1;
            *cur_col = 2 + caret_col;
        }
        if (le > ls) push_trunc(b, data + ls, le - ls, avail);
    }
}

void tui_render_force(tui *t) {
    t->dirty = true;
    tui_render(t);
}

void tui_render(tui *t) {
    if (!t->tty) {
        if (t->out.len) {
            wout(t->out.data, t->out.len);
            buf_clear(&t->out);
            fflush(stdout);
        }
        t->dirty = false;
        return;
    }
    if (!t->dirty) return;

    erase_block(t);
    if (t->out.len) {
        wout(t->out.data, t->out.len);
        buf_clear(&t->out);
    }

    int maxw = t->cols - 1;
    buf_t b;
    buf_init(&b);
    int rows = 0, cur_row = 0, cur_col = 0;

    if (t->partial.len) {
        row_sep(&b, &rows);
        /* the streaming line carries SGR (thinking is dimmed): push_trunc
         * would strip the ESC and print the "[2m"/"[0m" remainder literally */
        tui_push_ansi(&b, t->partial.data, t->partial.len, maxw);
        buf_appends(&b, tui_c(t, "\x1b[0m"));
    }
    if (t->overlay.len) overlay_rows(t, &b, &rows, maxw);
    if (t->pick != PICK_NONE && t->n_items > 0) popover_rows(t, &b, &rows, maxw);
    row_sep(&b, &rows);
    status_row(t, &b, maxw);
    composer_rows(t, &b, &rows, maxw, &cur_row, &cur_col);

    wout(b.data, b.len);
    buf_free(&b);

    /* park the caret: we are at the end of the last row */
    char esc[48];
    wout("\r", 1);
    int up = rows - 1 - cur_row;
    if (up > 0) {
        int n = snprintf(esc, sizeof esc, "\x1b[%dA", up);
        wout(esc, (size_t)n);
    }
    if (cur_col > 0) {
        int n = snprintf(esc, sizeof esc, "\x1b[%dC", cur_col);
        wout(esc, (size_t)n);
    }
    t->block_rows = rows;
    t->cur_row = cur_row;
    t->dirty = false;
    fflush(stdout);
}

void tui_raw_begin(tui *t) {
    erase_block(t);
    if (t->out.len) {
        wout(t->out.data, t->out.len);
        buf_clear(&t->out);
    }
    if (t->partial.len) {
        wout(t->partial.data, t->partial.len);
        wout("\n", 1);
        buf_clear(&t->partial);
    }
    fflush(stdout);
}

void tui_raw_end(tui *t) {
    fflush(stdout);
    t->dirty = true;
}

void tui_write(tui *t, const char *s, size_t n) {
    /* backend text can carry embedded NULs (JSON u+0000): never emit them */
    size_t at = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\0') continue;
        buf_append(&t->partial, s + at, i - at);
        at = i + 1;
    }
    buf_append(&t->partial, s + at, n - at);
    size_t cut = 0;
    for (size_t i = t->partial.len; i > 0; i--)
        if (t->partial.data[i - 1] == '\n') { cut = i; break; }
    if (cut) {
        buf_append(&t->out, t->partial.data, cut);
        buf_consume(&t->partial, cut);
    }
    t->dirty = true;
}

void tui_bol(tui *t) {
    if (t->partial.len) tui_write(t, "\n", 1);
}

void tui_linef(tui *t, const char *fmt, ...) {
    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    tui_bol(t);
    tui_write(t, line, n < (int)sizeof line ? (size_t)n : sizeof line - 1);
    tui_write(t, "\n", 1);
}

void tui_sys(tui *t, const char *s) {
    tui_linef(t, "%s%s%s", tui_c(t, "\x1b[2m"), s, tui_c(t, "\x1b[0m"));
}

void tui_err(tui *t, const char *s) {
    tui_linef(t, "%stny: %s%s", tui_c(t, "\x1b[31m"), s, tui_c(t, "\x1b[0m"));
}

/* Menu-style output: transient on a terminal (cleared once the interaction
 * ends), plain transcript lines when stdout is not a tty. */
void tui_overlay_linef(tui *t, const char *fmt, ...) {
    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (!t->tty) {
        tui_bol(t);
        tui_write(t, line, n < (int)sizeof line ? (size_t)n : sizeof line - 1);
        tui_write(t, "\n", 1);
        return;
    }
    buf_append(&t->overlay, line, n < (int)sizeof line ? (size_t)n : sizeof line - 1);
    buf_appends(&t->overlay, "\n");
    t->dirty = true;
}

void tui_overlay_clear(tui *t) {
    if (!t->overlay.len) return;
    buf_clear(&t->overlay);
    t->dirty = true;
}

void tui_note(tui *t, const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    buf_clear(&t->note);
    buf_appends(&t->note, line);
    t->dirty = true;
}
