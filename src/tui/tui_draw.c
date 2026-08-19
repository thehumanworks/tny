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

/* Display columns of a UTF-8 run, counting one column per code point.
 * Wide (CJK/emoji) glyphs are undercounted; the cols-1 clamp absorbs it. */
static int dw(const char *s, size_t n) {
    int w = 0;
    for (size_t i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) w++;
    return w;
}

/* Advance past `cols` display columns. */
static const char *skip_cols(const char *s, const char *end, int cols) {
    while (s < end && cols > 0) {
        s++;
        while (s < end && ((unsigned char)*s & 0xC0) == 0x80) s++;
        cols--;
    }
    return s;
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
    else if (t->turn_active) buf_appends(&s, "  working… (esc cancels)");
    else buf_appendf(&s, "  %s", t->ctx->cwd);

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

    /* logical lines */
    const char *data = t->input.len ? t->input.data : "";
    size_t len = t->input.len;
    size_t starts[258];
    int nl = 0;
    starts[nl++] = 0;
    for (size_t i = 0; i < len && nl < 256; i++)
        if (data[i] == '\n') starts[nl++] = i + 1;
    starts[nl] = len + 1; /* sentinel: line i spans [starts[i], starts[i+1]-1) */

    int caret_line = 0;
    for (int i = 0; i < nl; i++)
        if (t->cur >= starts[i]) caret_line = i;

    int first = 0;
    if (caret_line >= TUI_COMP_ROWS) first = caret_line - TUI_COMP_ROWS + 1;

    int avail = maxw - 2;
    if (avail < 8) avail = 8;
    for (int i = first; i < nl && i - first < TUI_COMP_ROWS; i++) {
        row_sep(b, rows);
        size_t ls = starts[i], le = starts[i + 1] - 1;
        if (le > len) le = len;
        const char *lp = data + ls;
        size_t ll = le - ls;
        buf_appends(b, tui_c(t, "\x1b[1;32m"));
        buf_appends(b, i == 0 ? "> " : "  ");
        buf_appends(b, tui_c(t, "\x1b[0m"));
        int off = 0;
        if (i == caret_line) {
            int col = dw(lp, t->cur - ls);
            if (col > avail - 1) off = col - (avail - 1);
            *cur_row = *rows - 1;
            *cur_col = 2 + col - off;
        }
        const char *from = skip_cols(lp, lp + ll, off);
        push_trunc(b, from, (size_t)(lp + ll - from), avail);
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
        push_trunc(&b, t->partial.data, t->partial.len, maxw);
    }
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
    buf_append(&t->partial, s, n);
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
