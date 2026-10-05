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
const char *tui_attr(const tui *t, const char *code) { return t->attr ? code : ""; }

static void wout(const void *s, size_t n) {
    if (n) fwrite(s, 1, n, stdout);
}

/* Append at most maxw columns of s, dropping control bytes. Returns columns. */
static int push_trunc(buf_t *b, const char *s, size_t n, int maxw) {
    int w = 0;
    for (size_t i = 0; i < n && w < maxw;) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\r') {
            i++;
            continue;
        }
        if (c == '\t') {
            buf_appends(b, " ");
            w++;
            i++;
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            i++;
            continue;
        }
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
                while (j < n && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e)) j++;
                if (j < n && s[j] == 'm') buf_append(b, s + i, j - i + 1);
                i = j < n ? j + 1 : n;
            } else {
                i = j;
            }
            continue;
        }
        if (w >= maxw) {
            i++;
            continue;
        } /* keep scanning: a reset may follow */
        if (c == '\n' || c == '\r') {
            i++;
            continue;
        }
        if (c == '\t') {
            buf_appends(b, " ");
            w++;
            i++;
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            i++;
            continue;
        }
        size_t j = i + 1;
        while (j < n && ((unsigned char)s[j] & 0xC0) == 0x80) j++;
        buf_append(b, s + i, j - i);
        w++;
        i = j;
    }
    return w;
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* Emscripten's tty fakes TIOCGWINSZ; the page keeps the real size in
 * Module.tnyWinsize and raises SIGWINCH via _tny_wasm_winch on resize
 * (docs/adr/0017). */
EM_JS(int, js_tui_cols, (void), { return (Module.tnyWinsize && Module.tnyWinsize[0]) | 0; });
EM_JS(int, js_tui_rows, (void), { return (Module.tnyWinsize && Module.tnyWinsize[1]) | 0; });
#endif

void tui_size(tui *t) {
    struct winsize ws;
    t->rows = 24;
    t->cols = 80;
    static const int fds[] = {1, 0, 2};
    for (size_t i = 0; t->tty && i < sizeof fds / sizeof *fds; i++) {
        if (ioctl(fds[i], TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) continue;
        t->cols = ws.ws_col;
        t->rows = ws.ws_row > 0 ? ws.ws_row : 24;
        break;
    }
#ifdef __EMSCRIPTEN__
    if (js_tui_cols() > 0) {
        t->cols = js_tui_cols();
        t->rows = js_tui_rows() > 0 ? js_tui_rows() : 24;
    }
#endif
    if (t->cols < 20) t->cols = 20;
}

/* The pty's winsize is what the kernel was told, not what the user sees:
 * a sandbox shell or web console that never forwards SIGWINCH leaves it at
 * 80x24 while the real terminal is 44 columns wide. Ask the terminal itself
 * (docs/adr/0054): park the cursor at the bottom-right corner (CUP clamps,
 * never scrolls), request a report, restore. The answer arrives on stdin as
 * ESC[rows;colsR and lands in tui_size_report via the key decoder. */
void tui_size_probe(tui *t) {
    if (!t->tty) return;
    /* octal ESC: "\x1b7" would read as one hex escape */
    static const char probe[] = "\0337\x1b[999;999H\x1b[6n\0338";
    wout(probe, sizeof probe - 1);
    fflush(stdout);
}

void tui_size_report(tui *t, int rows, int cols) {
    if (rows < 2 || cols < 2) return; /* not a corner report */
    if (cols < 20) cols = 20;
    if (rows == t->rows && cols == t->cols) return;
    t->rows = rows;
    t->cols = cols;
    t->dirty = true;
}

void tui_resize(tui *t) {
    tui_size(t);
    tui_size_probe(t);
    t->dirty = true;
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

void tui_status_row(tui *t, buf_t *b, int maxw) {
    buf_t s;
    buf_init(&s);
    if (t->session_readonly) buf_appends(&s, "read-only  ");
    buf_appendf(&s, "%s  %s  %s", tny_provider_name(t->ctx),
                t->ctx->model ? t->ctx->model : "default", tny_perm_mode_name(t->ctx->perm_mode));
    if (t->session) buf_appendf(&s, "  %s", t->session->id);
    if (t->ctx->task_name) buf_appendf(&s, "  task:%s", t->ctx->task_name);
    if (t->in_tok || t->out_tok)
        buf_appendf(&s, "  %lld/%lld tok", (long long)t->in_tok, (long long)t->out_tok);
    if (t->n_images) buf_appendf(&s, "  %d img", t->n_images);
    if (t->note.len) buf_appendf(&s, "  %s", t->note.data);
    else if (t->turn_active) {
        static const char *frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
        buf_appendf(&s, "  %s working… (esc cancels)", frames[t->spin % 10]);
    } else if (t->ctx->ssh_host) buf_appendf(&s, "  ssh %s:%s", t->ctx->ssh_host, t->ctx->ssh_cwd);
    else buf_appendf(&s, "  %s", t->ctx->cwd);

    if (t->attr) {
        /* reverse video is structural, not a color: it survives NO_COLOR */
        buf_appends(b, tui_attr(t, "\x1b[7m"));
        int w = push_trunc(b, s.data, s.len, maxw);
        for (; w < maxw; w++) buf_appends(b, " ");
        buf_appends(b, tui_attr(t, "\x1b[0m"));
    } else {
        /* no SGR at all (--color=never): without delimiters the row reads
         * as ordinary transcript text */
        buf_appends(b, "── ");
        push_trunc(b, s.data, s.len, maxw - 6);
        buf_appends(b, " ──");
    }
    buf_free(&s);
}

/* Messages waiting for the running turn to end (docs/adr/0011): one dim
 * row next to the composer, gone the moment they are sent. */
static void queue_row(tui *t, buf_t *b, int *rows, int maxw) {
    row_sep(b, rows);
    buf_t line;
    buf_init(&line);
    buf_appendf(&line, "queued (%d): %s", t->n_queue, tui_shell_visible(t->queue[0]));
    if (t->n_queue > 1) buf_appends(&line, " …");
    buf_appends(&line, " · sends when this turn ends · esc drops");
    buf_appends(b, tui_attr(t, "\x1b[2m"));
    push_trunc(b, line.data, line.len, maxw);
    buf_appends(b, tui_attr(t, "\x1b[0m"));
    buf_free(&line);
}

static void popover_rows(tui *t, buf_t *b, int *rows, int maxw, int limit) {
    int first = 0;
    if (t->sel >= limit) first = t->sel - limit + 1;
    for (int i = first; i < t->n_items && i - first < limit; i++) {
        row_sep(b, rows);
        bool on = i == t->sel;
        if (on) { /* bold is structural, cyan is the color on top */
            buf_appends(b, tui_attr(t, "\x1b[1m"));
            buf_appends(b, tui_c(t, "\x1b[36m"));
        } else {
            buf_appends(b, tui_attr(t, "\x1b[2m"));
        }
        buf_t line;
        buf_init(&line);
        buf_appendf(&line, "%s %-22s %s", on ? "›" : " ", t->items[i].label,
                    t->items[i].hint ? t->items[i].hint : "");
        push_trunc(b, line.data, line.len, maxw);
        buf_free(&line);
        buf_appends(b, tui_attr(t, "\x1b[0m"));
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

void tui_wrap_locate(const char *s, size_t n, size_t cur, int width, int *row, int *col,
                     int *total) {
    if (width < 1) width = 1;
    if (!s) s = "";
    int r = 0, c = 0, cr = 0, cc = 0;
    for (size_t i = 0; i < n;) {
        if (i == cur) {
            cr = r;
            cc = c;
        }
        if (s[i] == '\n') {
            r++;
            c = 0;
            i++;
            continue;
        }
        size_t a = cp_adv(s, n, i);
        if (c >= width) {
            r++;
            c = 0;
        }
        c++;
        i += a;
    }
    if (cur >= n) {
        cr = r;
        cc = c;
    }
    if (row) *row = cr;
    if (col) *col = cc;
    if (total) *total = r + 1;
}

size_t tui_wrap_index(const char *s, size_t n, int width, int trow, int tcol) {
    if (width < 1) width = 1;
    if (!s || trow < 0) return 0;
    int r = 0, c = 0;
    for (size_t i = 0; i < n;) {
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
        tui_wrap_locate(t->input.data, t->input.len, 0, tui_wrap_width(t), NULL, NULL, &total);
        used += total < TUI_COMP_ROWS ? total : TUI_COMP_ROWS;
    }
    if (t->pick != PICK_NONE && t->n_items > 0)
        used += t->n_items < TUI_POP_ROWS ? t->n_items : TUI_POP_ROWS;
    int budget = t->rows - used;
    return budget > 0 ? budget : 0;
}

static void overlay_rows(tui *t, buf_t *b, int *rows, int maxw, int budget) {
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
        buf_appends(b, tui_attr(t, "\x1b[0m"));
        p = nl ? nl + 1 : end;
    }
    if (show < total) {
        row_sep(b, rows);
        buf_appends(b, tui_attr(t, "\x1b[2m"));
        buf_t m;
        buf_init(&m);
        buf_appendf(&m, "  … %d more rows (enlarge the window to see them)", total - show);
        tui_push_ansi(b, m.data, m.len, maxw);
        buf_free(&m);
        buf_appends(b, tui_attr(t, "\x1b[0m"));
    }
}

static void composer_rows(tui *t, buf_t *b, int *rows, int maxw, int limit, int *cur_row,
                          int *cur_col) {
    if (t->approval) {
        row_sep(b, rows);
        *cur_row = *rows - 1;
        buf_appends(b, tui_attr(t, "\x1b[1m"));
        buf_appends(b, tui_c(t, "\x1b[33m"));
        const char *q = "approve? [y] yes  [a] yes, don't ask again  [n] no";
        *cur_col = push_trunc(b, q, strlen(q), maxw);
        buf_appends(b, tui_attr(t, "\x1b[0m"));
        return;
    }

    const char *data = t->input.len ? t->input.data : "";
    size_t len = t->input.len;
    int avail = maxw - 2;
    if (avail < 8) avail = 8;
    int caret_row = 0, caret_col = 0, total = 1;
    tui_wrap_locate(data, len, t->cur, avail, &caret_row, &caret_col, &total);

    int first = 0;
    if (caret_row >= limit) first = caret_row - limit + 1;

    for (int vr = first; vr < total && vr - first < limit; vr++) {
        row_sep(b, rows);
        size_t ls = tui_wrap_index(data, len, avail, vr, 0);
        size_t le = vr + 1 < total ? tui_wrap_index(data, len, avail, vr + 1, 0) : len;
        if (le > ls && data[le - 1] == '\n') le--;
        buf_appends(b, tui_attr(t, "\x1b[1m"));
        buf_appends(b, tui_c(t, "\x1b[32m"));
        buf_appends(b, ls == 0 ? (t->shell_mode ? "! " : "> ") : "  ");
        buf_appends(b, tui_attr(t, "\x1b[0m"));
        if (vr == caret_row) {
            *cur_row = *rows - 1;
            *cur_col = 2 + caret_col;
        }
        if (le > ls) push_trunc(b, data + ls, le - ls, avail);
    }
}

/* Retain display text only. Durable session history has its own lifecycle;
 * the renderer needs a bounded tail to rebuild after a resize or overlay. */
static void transcript_commit(tui *t) {
    if (!t->out.len) return;
    buf_append(&t->transcript, t->out.data, t->out.len);
    if (t->transcript.oom) return;
    t->transcript_version++;
    for (size_t i = 0; i < t->out.len; i++)
        if (t->out.data[i] == '\n') t->transcript_lines++;
    buf_clear(&t->out);
    size_t limit = t->scrollback_lines ? t->scrollback_lines : TNY_UI_SCROLLBACK_LINES_DEFAULT;
    size_t cut = 0;
    while (t->transcript_lines > limit) {
        const char *nl = memchr(t->transcript.data + cut, '\n', t->transcript.len - cut);
        if (!nl) break;
        cut = (size_t)(nl + 1 - t->transcript.data);
        t->transcript_lines--;
    }
    if (cut) {
        buf_consume(&t->transcript, cut);
        t->scrollback_anchor = t->scrollback_anchor > cut ? t->scrollback_anchor - cut : 0;
    }
}

/* Convert transcript text to explicit physical rows. Only SGR survives:
 * OSC, cursor movement and other controls cannot escape this viewport. */
static int transcript_cell_width(const char *s, size_t n) {
    unsigned char first = (unsigned char)s[0];
    uint32_t cp = first;
    if (n > 1) {
        cp = first & (n == 2 ? 0x1fu : n == 3 ? 0x0fu : 0x07u);
        for (size_t i = 1; i < n; i++) cp = (cp << 6) | ((unsigned char)s[i] & 0x3fu);
    }
    /* Deterministic terminal widths, independent of the process locale.
     * Combining blocks and selectors consume no cells; ambiguous characters
     * and private-use font icons stay single-width. */
    static const uint32_t zero[][2] = {
        {0x0300, 0x036f}, {0x0483, 0x0489}, {0x0591, 0x05bd}, {0x05bf, 0x05bf},   {0x05c1, 0x05c2},
        {0x05c4, 0x05c5}, {0x05c7, 0x05c7}, {0x0610, 0x061a}, {0x064b, 0x065f},   {0x0670, 0x0670},
        {0x06d6, 0x06ed}, {0x1ab0, 0x1aff}, {0x1dc0, 0x1dff}, {0x200b, 0x200f},   {0x2060, 0x2064},
        {0x20d0, 0x20ff}, {0xfe00, 0xfe0f}, {0xfe20, 0xfe2f}, {0xe0100, 0xe01ef},
    };
    static const uint32_t wide[][2] = {
        {0x1100, 0x115f},   {0x2329, 0x232a},   {0x2e80, 0x303e},   {0x3041, 0x33ff},
        {0x3400, 0x4dbf},   {0x4e00, 0x9fff},   {0xa000, 0xa4cf},   {0xa960, 0xa97f},
        {0xac00, 0xd7a3},   {0xf900, 0xfaff},   {0xfe30, 0xfe4f},   {0xff00, 0xff60},
        {0xffe0, 0xffe6},   {0x1f300, 0x1f64f}, {0x1f680, 0x1f6ff}, {0x1f900, 0x1faff},
        {0x20000, 0x2fffd}, {0x30000, 0x3fffd},
    };
    for (size_t i = 0; i < sizeof zero / sizeof *zero; i++)
        if (cp >= zero[i][0] && cp <= zero[i][1]) return 0;
    for (size_t i = 0; i < sizeof wide / sizeof *wide; i++)
        if (cp >= wide[i][0] && cp <= wide[i][1]) return 2;
    return 1;
}

typedef struct {
    size_t source, visual, sgr;
} scrollback_row;

static void transcript_wrap(buf_t *b, const char *s, size_t n, int width, bool attr,
                            buf_t *starts) {
    scrollback_row start = {0, 0, SIZE_MAX};
    size_t sgr = SIZE_MAX;
    buf_append(starts, &start, sizeof start);
    int col = 0;
    for (size_t i = 0; i < n;) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == 0x1b) {
            size_t j = i + 1;
            if (j < n && s[j] == '[') {
                j++;
                while (j < n && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e)) j++;
                if (j < n && s[j] == 'm' && attr) {
                    if ((j - i == 2) || (j - i == 3 && s[i + 2] == '0')) sgr = SIZE_MAX;
                    else if (sgr == SIZE_MAX) sgr = b->len;
                    buf_append(b, s + i, j - i + 1);
                }
                i = j < n ? j + 1 : n;
            } else if (j < n && s[j] == ']') {
                j++;
                while (j < n && s[j] != '\a' && !(s[j] == '\x1b' && j + 1 < n && s[j + 1] == '\\'))
                    j++;
                i = j < n ? j + (s[j] == '\a' ? 1u : 2u) : n;
            } else i = j < n ? j + 1 : n;
            continue;
        }
        if (ch == '\n') {
            buf_appends(b, "\n");
            col = 0;
            i++;
            start = (scrollback_row){i, b->len, sgr};
            buf_append(starts, &start, sizeof start);
            continue;
        }
        if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
            i++;
            continue;
        }
        size_t j = i + 1;
        while (j < n && ((unsigned char)s[j] & 0xc0) == 0x80) j++;
        int cells = ch == '\t' ? 1 : transcript_cell_width(s + i, j - i);
        if (cells && col + cells > width) {
            buf_appends(b, "\n");
            col = 0;
            start = (scrollback_row){i, b->len, sgr};
            buf_append(starts, &start, sizeof start);
        }
        if (ch == '\t') buf_appends(b, " ");
        else buf_append(b, s + i, j - i);
        col += cells;
        i = j;
    }
}

/* Reflow only when display text or terminal width changes. Composer edits,
 * spinner ticks and scrolling reuse the physical-row index. */
static bool scrollback_reflow(tui *t, int width) {
    if (t->scrollback_width != width || t->scrollback_attr != t->attr ||
        t->scrollback_version != t->transcript_version ||
        t->scrollback_partial.len != t->partial.len ||
        (t->partial.len &&
         memcmp(t->scrollback_partial.data, t->partial.data, t->partial.len) != 0)) {
        buf_t source;
        buf_init(&source);
        buf_append(&source, t->transcript.data, t->transcript.len);
        buf_append(&source, t->partial.data, t->partial.len);
        if (source.oom) {
            buf_free(&source);
            return false;
        }
        buf_clear(&t->scrollback_visual);
        buf_clear(&t->scrollback_starts);
        transcript_wrap(&t->scrollback_visual, source.data, source.len, width, t->attr,
                        &t->scrollback_starts);
        buf_free(&source);
        buf_clear(&t->scrollback_partial);
        buf_append(&t->scrollback_partial, t->partial.data, t->partial.len);
        if (t->scrollback_starts.oom || t->scrollback_visual.oom || t->scrollback_partial.oom)
            return false;
        t->scrollback_version = t->transcript_version;
        t->scrollback_width = width;
        t->scrollback_attr = t->attr;
    }
    if (t->scrollback_starts.oom || t->scrollback_visual.oom ||
        t->scrollback_starts.len < sizeof(scrollback_row))
        return false;
    size_t total = t->scrollback_starts.len / sizeof(scrollback_row);
    if (!t->scrollback_visual.len ||
        t->scrollback_visual.data[t->scrollback_visual.len - 1] == '\n')
        total--;
    t->scrollback_total = total;
    return true;
}

static size_t scrollback_first(tui *t, size_t maximum) {
    if (!t->scrollback_offset) return maximum;
    const scrollback_row *starts = (const scrollback_row *)t->scrollback_starts.data;
    size_t low = 0, high = maximum;
    while (low < high) {
        size_t middle = low + (high - low + 1) / 2;
        if (starts[middle].source <= t->scrollback_anchor) low = middle;
        else high = middle - 1;
    }
    return low;
}

static void fullscreen_transcript(tui *t, buf_t *frame, int height, int width) {
    if (height <= 0) return;
    if (!scrollback_reflow(t, width)) return;
    size_t total = t->scrollback_total;
    buf_t visual = t->scrollback_visual;
    size_t maximum = total > (size_t)height ? total - (size_t)height : 0;
    size_t skip = scrollback_first(t, maximum);
    t->scrollback_offset = maximum - skip;
    t->scrollback_anchor = ((scrollback_row *)t->scrollback_starts.data)[skip].source;
    t->scrollback_height = height;
    size_t at = ((scrollback_row *)t->scrollback_starts.data)[skip].visual;
    /* A wrapped visible row can inherit an attribute from discarded rows.
     * Replay only those zero-width SGR sequences, never their text. */
    if (at && t->attr) {
        size_t sgr = ((scrollback_row *)t->scrollback_starts.data)[skip].sgr;
        if (sgr != SIZE_MAX) tui_push_ansi(frame, visual.data + sgr, at - sgr, 0);
    }
    for (int row = 1; row <= height && at < visual.len; row++) {
        const char *nl = memchr(visual.data + at, '\n', visual.len - at);
        size_t end = nl ? (size_t)(nl - visual.data) : visual.len;
        buf_appendf(frame, "\x1b[%d;1H", row);
        buf_append(frame, visual.data + at, end - at);
        at = nl ? end + 1 : end;
    }
    buf_appends(frame, tui_attr(t, "\x1b[0m"));
}

int tui_scrollback_page_rows(const tui *t) {
    return t->scrollback_height > 1 ? t->scrollback_height - 1 : 1;
}

void tui_scrollback_scroll(tui *t, int lines) {
    if (!t->fullscreen || !lines) return;
    transcript_commit(t);
    if (!scrollback_reflow(t, t->cols > 3 ? t->cols - 1 : 3)) return;
    size_t total = t->scrollback_total;
    size_t height = t->scrollback_height > 0 ? (size_t)t->scrollback_height : 1;
    size_t maximum = total > height ? total - height : 0;
    scrollback_row *indices = (scrollback_row *)t->scrollback_starts.data;
    size_t first = scrollback_first(t, maximum);
    if (lines > 0) first = (size_t)lines > first ? 0 : first - (size_t)lines;
    else {
        size_t down = (size_t)(-(int64_t)lines);
        first = down > maximum - first ? maximum : first + down;
    }
    t->scrollback_offset = maximum - first;
    t->scrollback_anchor = indices[first].source;
    t->dirty = true;
}

void tui_scrollback_home(tui *t) { tui_scrollback_scroll(t, INT32_MAX); }
void tui_scrollback_end(tui *t) { tui_scrollback_scroll(t, -INT32_MAX); }

void tui_fullscreen_frame(tui *t, buf_t *frame) {
    transcript_commit(t);
    int height = t->rows > 0 ? t->rows : 1;
    int width = t->cols > 3 ? t->cols - 1 : 3;
    /* Reserve transcript space whenever the terminal can fit all three
     * regions. Composer and status take priority over menus on short ttys. */
    int bottom_budget = height >= 3 ? height - 1 : height;
    int status = height >= 2 ? 1 : 0;
    int comp = 1;
    if (!t->approval)
        tui_wrap_locate(t->input.data, t->input.len, t->cur, tui_wrap_width(t), NULL, NULL, &comp);
    if (comp > TUI_COMP_ROWS) comp = TUI_COMP_ROWS;
    if (comp > bottom_budget - status) comp = bottom_budget - status;
    int left = bottom_budget - status - comp;
    int queued = t->n_queue && left > 0 ? 1 : 0;
    left -= queued;
    int pick = t->pick != PICK_NONE ? t->n_items : 0;
    if (pick > TUI_POP_ROWS) pick = TUI_POP_ROWS;
    if (pick > left) pick = left;
    left -= pick;

    buf_t bottom;
    buf_init(&bottom);
    int rows = 0, caret_row = 0, caret_col = 0;
    if (t->overlay.len) overlay_rows(t, &bottom, &rows, width, left);
    if (pick > 0) popover_rows(t, &bottom, &rows, width, pick);
    if (queued) queue_row(t, &bottom, &rows, width);
    if (status) {
        row_sep(&bottom, &rows);
        tui_status_row(t, &bottom, width);
    }
    composer_rows(t, &bottom, &rows, width, comp, &caret_row, &caret_col);

    buf_appends(frame, "\x1b[?25l\x1b[?7l\x1b[H\x1b[2J");
    fullscreen_transcript(t, frame, height - rows, width);
    size_t at = 0;
    int top = height - rows + 1;
    for (int row = 0; row < rows; row++) {
        const char *nl = memchr(bottom.data + at, '\n', bottom.len - at);
        size_t end = nl ? (size_t)(nl - bottom.data) : bottom.len;
        buf_appendf(frame, "\x1b[%d;1H", top + row);
        tui_push_ansi(frame, bottom.data + at, end - at, width);
        at = nl ? end + 1 : end;
    }
    if (caret_col >= t->cols) caret_col = t->cols > 0 ? t->cols - 1 : 0;
    buf_appendf(frame, "\x1b[%d;%dH\x1b[?7h\x1b[?25h", top + caret_row, caret_col + 1);
    t->block_rows = rows;
    t->cur_row = caret_row;
    buf_free(&bottom);
}

void tui_clear_screen(tui *t) {
    if (!t->tty) return;
    /* Discard only pending display text, never the saved session. Queue the
     * clear with the next paint so stale output cannot precede the new view.
     * The old block's cursor coordinates are no longer valid after home. */
    buf_clear(&t->out);
    buf_clear(&t->partial);
    buf_clear(&t->transcript);
    t->transcript_version++;
    t->transcript_lines = t->scrollback_offset = t->scrollback_anchor = t->scrollback_total = 0;
    if (!t->fullscreen) buf_appends(&t->out, "\x1b[H\x1b[2J\x1b[3J");
    t->block_rows = t->cur_row = 0;
    t->dirty = true;
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

    if (t->fullscreen) {
        buf_t frame;
        buf_init(&frame);
        tui_fullscreen_frame(t, &frame);
        wout(frame.data, frame.len);
        buf_free(&frame);
        t->dirty = false;
        fflush(stdout);
        return;
    }

    erase_block(t);
    if (t->out.len) {
        wout(t->out.data, t->out.len);
        buf_clear(&t->out);
    }

    int maxw = t->cols - 1;
    buf_t b;
    buf_init(&b);
    /* DECAWM off while the block is on screen: if t->cols is still wider
     * than the real terminal (probe answer pending, or a terminal that
     * never answers), an over-long row is clipped at the margin instead of
     * soft-wrapping onto a second physical row that erase_block does not
     * know about — which is how every repaint used to leave a stale copy of
     * the status row behind (docs/adr/0054). The transcript above keeps
     * wrapping: it is written before this and after the restore. */
    buf_appends(&b, "\x1b[?7l");
    int rows = 0, cur_row = 0, cur_col = 0;

    if (t->partial.len) {
        row_sep(&b, &rows);
        /* the streaming line carries SGR (thinking is dimmed): push_trunc
         * would strip the ESC and print the "[2m"/"[0m" remainder literally */
        tui_push_ansi(&b, t->partial.data, t->partial.len, maxw);
        buf_appends(&b, tui_attr(t, "\x1b[0m"));
    }
    if (t->overlay.len) overlay_rows(t, &b, &rows, maxw, tui_overlay_budget(t));
    if (t->pick != PICK_NONE && t->n_items > 0) popover_rows(t, &b, &rows, maxw, TUI_POP_ROWS);
    if (t->n_queue) queue_row(t, &b, &rows, maxw);
    row_sep(&b, &rows);
    tui_status_row(t, &b, maxw);
    composer_rows(t, &b, &rows, maxw, TUI_COMP_ROWS, &cur_row, &cur_col);

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
    wout("\x1b[?7h", 5);
    t->block_rows = rows;
    t->cur_row = cur_row;
    t->dirty = false;
    fflush(stdout);
}

void tui_raw_begin(tui *t) {
    tui_terminal_mouse(false);
    if (t->tty && t->fullscreen) {
        tui_bol(t);
        tui_render_force(t);
        /* Release the bottom composer and permit ordinary command output
         * to scroll. Interactive commands (SSH, login) keep live stdout. */
        char esc[64];
        int row = t->rows - t->block_rows + 1;
        if (row < 1) row = 1;
        int n = snprintf(esc, sizeof esc, "\x1b[?7h\x1b[r\x1b[%d;1H\x1b[J", row);
        wout(esc, (size_t)n);
        t->block_rows = t->cur_row = 0;
        fflush(stdout);
        return;
    }
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
    tui_terminal_mouse(true);
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
        if (t->partial.data[i - 1] == '\n') {
            cut = i;
            break;
        }
    if (cut) {
        buf_append(&t->out, t->partial.data, cut);
        buf_consume(&t->partial, cut);
    }
    t->dirty = true;
}

void tui_bol(tui *t) {
    if (t->partial.len) tui_write(t, "\n", 1);
}

/* Dim streaming text (reasoning traces). tui_write flushes the transcript at
 * newline granularity, and tui_render repaints t->partial from scratch every
 * frame — after emitting a reset. SGR state therefore never survives a '\n':
 * each physical line must open and close its own dim attribute, or the tail
 * of a delta that crossed a newline repaints in the default color
 * (docs/adr/0012). */
void tui_write_dim(tui *t, const char *s, size_t n) {
    if (!t->attr) { /* dim is an attribute: it survives NO_COLOR */
        tui_write(t, s, n);
        return;
    }
    size_t at = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && s[i] != '\n') continue;
        if (i > at) {
            tui_write(t, "\x1b[2m", 4);
            tui_write(t, s + at, i - at);
            tui_write(t, "\x1b[0m", 4);
        }
        if (i < n) tui_write(t, "\n", 1);
        at = i + 1;
    }
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
    tui_linef(t, "%s%s%s", tui_attr(t, "\x1b[2m"), s, tui_attr(t, "\x1b[0m"));
}

void tui_sysf(tui *t, const char *fmt, ...) {
    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    line[sizeof line - 1] = 0;
    tui_sys(t, line);
}

void tui_err(tui *t, const char *s) {
    tui_linef(t, "%stny: %s%s", tui_c(t, "\x1b[31m"), s, tui_attr(t, "\x1b[0m"));
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
    t->settings_open = false;
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
