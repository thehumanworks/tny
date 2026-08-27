/* tui_input.c — key decoding, the composer line editor, prompt history and
 * popover selection. Escape sequences are decoded from a byte stream, so a
 * split CSI across two read()s is re-joined instead of leaking as literals. */
#include "tui/tui.h"
#include "util/tny_poll.h"
#include "core/image.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- composer primitives ---- */

static void ins(tui *t, const char *s, size_t n) {
    if (t->input.len + n > 1u << 20) return; /* paste guard */
    buf_reserve(&t->input, n);
    memmove(t->input.data + t->cur + n, t->input.data + t->cur, t->input.len - t->cur);
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
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || c == '.' || c == '/';
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
    if (t->approval) {
        tui_pick_close(t);
        return;
    }
    const char *d = t->input.len ? t->input.data : "";
    size_t cur = t->cur;

    /* slash palette: only while the whole draft is one unbroken /word */
    if (cur > 0 && d[0] == '/') {
        bool clean = true;
        for (size_t i = 1; i < cur; i++)
            if (d[i] == ' ' || d[i] == '\n') {
                clean = false;
                break;
            }
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
    if (t->pick != PICK_NONE) {
        tui_pick_close(t);
        t->dirty = true;
    }
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

static void caret_visual_move(tui *t, int dir) {
    int width = tui_wrap_width(t);
    int row = 0, col = 0, total = 1;
    tui_wrap_locate(t->input.data, t->input.len, t->cur, width, &row, &col, &total);
    if (dir < 0) {
        if (row <= 0) {
            hist_move(t, -1);
            return;
        }
        t->cur = tui_wrap_index(t->input.data, t->input.len, width, row - 1, col);
    } else {
        if (row + 1 >= total) {
            hist_move(t, 1);
            return;
        }
        t->cur = tui_wrap_index(t->input.data, t->input.len, width, row + 1, col);
    }
    t->dirty = true;
}

/* ---- clipboard (spawned only on Ctrl-V; never at startup) ---- */

static int spawn_to_fd(char *const argv[], int outfd) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (outfd >= 0) dup2(outfd, STDOUT_FILENO);
        int nulfd = open("/dev/null", O_RDWR);
        if (nulfd >= 0) {
            dup2(nulfd, STDERR_FILENO);
            if (nulfd > 2) close(nulfd);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    for (int i = 0; i < 40; i++) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
        tny_poll(NULL, 0, 50);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

#ifndef __APPLE__
static int spawn_to_file(char *const argv[], const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int rc = spawn_to_fd(argv, fd);
    close(fd);
    return rc;
}
#endif

static int clipboard_image(char *path, size_t pathlen) {
    snprintf(path, pathlen, "/tmp/tny-paste-%d-%d.png", (int)getpid(), (int)now_ms());
#ifdef __APPLE__
    char *pngpaste[] = {(char *)"pngpaste", path, NULL};
    if (spawn_to_fd(pngpaste, -1) == 0 && file_exists(path)) return 0;
    char script[1536]; /* boilerplate + any caller-sized path, no truncation */
    snprintf(script, sizeof script,
             "try\nset p to POSIX file \"%s\"\n"
             "set d to the clipboard as «class PNGf»\n"
             "set f to open for access p with write permission\n"
             "write d to f\nclose access f\non error\nreturn \"\"\nend try",
             path);
    char *osa[] = {(char *)"osascript", (char *)"-e", script, NULL};
    if (spawn_to_fd(osa, -1) == 0 && file_exists(path)) return 0;
#else
    char *wl[] = {(char *)"wl-paste", (char *)"-t", (char *)"image/png", (char *)"-o", NULL};
    if (spawn_to_file(wl, path) == 0 && file_exists(path)) return 0;
    char *xc[] = {(char *)"xclip",
                  (char *)"-selection",
                  (char *)"clipboard",
                  (char *)"-t",
                  (char *)"image/png",
                  (char *)"-o",
                  NULL};
    if (spawn_to_file(xc, path) == 0 && file_exists(path)) return 0;
#endif
    unlink(path);
    return -1;
}

static int drain_pipe(int rfd, buf_t *out) {
    char tmp[4096];
    ssize_t n;
    while ((n = read(rfd, tmp, sizeof tmp)) > 0) buf_append(out, tmp, (size_t)n);
    return out->len ? 0 : -1;
}

static int clipboard_text(buf_t *out) {
    int pfd[2];
    if (pipe(pfd) != 0) return -1;
    int rc = -1;
#ifdef __APPLE__
    char *pb[] = {(char *)"pbpaste", NULL};
    rc = spawn_to_fd(pb, pfd[1]);
#else
    char *wl[] = {(char *)"wl-paste", (char *)"-n", (char *)"-t", (char *)"text", NULL};
    rc = spawn_to_fd(wl, pfd[1]);
    if (rc != 0) {
        char *xc[] = {(char *)"xclip", (char *)"-selection", (char *)"clipboard", (char *)"-o",
                      NULL};
        rc = spawn_to_fd(xc, pfd[1]);
    }
#endif
    close(pfd[1]);
    if (rc == 0) rc = drain_pipe(pfd[0], out);
    close(pfd[0]);
    return rc;
}

static void do_paste(tui *t) {
    char path[PATH_MAX];
    if (clipboard_image(path, sizeof path) == 0) {
        uint8_t *data = image_load(path, NULL, NULL, NULL, 0);
        if (data) {
            free(data);
            /* Absolute temp paths begin with '/', which the composer treats
             * as slash commands at column zero. Inline-code quoting keeps the
             * path provider-neutral prompt text wherever it is inserted. */
            ins(t, "`", 1);
            ins(t, path, strlen(path));
            ins(t, "`", 1);
            tui_note(t, "pasted image path");
            t->dirty = true;
            tui_pick_refresh(t);
            return;
        }
        unlink(path);
    }
    buf_t text;
    buf_init(&text);
    if (clipboard_text(&text) == 0) {
        ins(t, text.data, text.len);
        t->dirty = true;
        tui_pick_refresh(t);
    } else {
        tui_note(t, "clipboard has no image");
    }
    buf_free(&text);
}

static void do_key(tui *t, int k, const char *ch, size_t chlen) {
    bool popover = t->pick != PICK_NONE && t->n_items > 0;

    if (t->approval) {
        /* answered by the nested approval reader; ignore here */
        return;
    }

    switch (k) {
    case TUI_K_CHAR:
        ins(t, ch, chlen);
        t->dirty = true;
        tui_pick_refresh(t);
        break;
    case TUI_K_ENTER:
        if (popover) {
            pick_accept(t, true);
            break;
        }
        submit_current(t);
        break;
    case TUI_K_TAB:
        if (popover) pick_accept(t, false);
        break;
    case TUI_K_NEWLINE:
        ins(t, "\n", 1);
        t->dirty = true;
        break;
    case TUI_K_PASTE: do_paste(t); break;
    case TUI_K_PASTE_BEGIN: t->in_paste = true; break;
    case TUI_K_BS:
        if (t->cur > 0) {
            del_range(t, prev_ch(t, t->cur), t->cur);
            t->dirty = true;
            tui_pick_refresh(t);
        }
        break;
    case TUI_K_DEL:
        del_range(t, t->cur, next_ch(t, t->cur));
        t->dirty = true;
        tui_pick_refresh(t);
        break;
    case TUI_K_WBS:
        del_range(t, word_left(t, t->cur), t->cur);
        t->dirty = true;
        tui_pick_refresh(t);
        break;
    case TUI_K_LEFT:
        t->cur = prev_ch(t, t->cur);
        t->dirty = true;
        tui_pick_refresh(t);
        break;
    case TUI_K_RIGHT:
        t->cur = next_ch(t, t->cur);
        t->dirty = true;
        tui_pick_refresh(t);
        break;
    case TUI_K_WLEFT:
        t->cur = word_left(t, t->cur);
        t->dirty = true;
        break;
    case TUI_K_WRIGHT:
        t->cur = word_right(t, t->cur);
        t->dirty = true;
        break;
    case TUI_K_HOME:
        t->cur = line_start(t, t->cur);
        t->dirty = true;
        break;
    case TUI_K_END:
        t->cur = line_end(t, t->cur);
        t->dirty = true;
        break;
    case TUI_K_KILL_EOL:
        del_range(t, t->cur, line_end(t, t->cur));
        t->dirty = true;
        break;
    case TUI_K_KILL_BOL:
        del_range(t, line_start(t, t->cur), t->cur);
        t->dirty = true;
        break;
    case TUI_K_UP:
        if (popover) {
            if (t->sel > 0) t->sel--;
            t->dirty = true;
            break;
        }
        caret_visual_move(t, -1);
        break;
    case TUI_K_DOWN:
        if (popover) {
            if (t->sel + 1 < t->n_items) t->sel++;
            t->dirty = true;
            break;
        }
        caret_visual_move(t, 1);
        break;
    case TUI_K_ESC:
        if (popover) {
            tui_pick_close(t);
            t->dirty = true;
            break;
        }
        if (t->overlay.len) {
            tui_overlay_clear(t);
            break;
        }
        if (t->turn_active) {
            tui_cancel_turn(t);
            break;
        }
        if (t->input.len) {
            set_input(t, NULL);
            t->dirty = true;
        }
        break;
    case TUI_K_CTRLC:
        if (popover) {
            tui_pick_close(t);
            t->dirty = true;
            break;
        }
        if (t->overlay.len) {
            tui_overlay_clear(t);
            break;
        }
        if (t->turn_active) {
            tui_cancel_turn(t);
            break;
        }
        if (t->input.len) {
            set_input(t, NULL);
            t->dirty = true;
            break;
        }
        if (t->last_ctrlc_ms && now_ms() - t->last_ctrlc_ms < 3000) {
            t->quit = true;
            t->exit_code = 130;
        } else {
            t->last_ctrlc_ms = now_ms();
            tui_note(t, "press ctrl-c again to exit");
        }
        break;
    case TUI_K_CTRLD:
        if (!t->input.len) t->quit = true;
        else {
            del_range(t, t->cur, next_ch(t, t->cur));
            t->dirty = true;
        }
        break;
    case TUI_K_CTRLL:
        tui_raw_begin(t);
        fputs("\x1b[H\x1b[2J\x1b[3J", stdout);
        tui_raw_end(t);
        break;
    case TUI_K_CTRLO: tui_command(t, "/transcript"); break;
    case TUI_K_CTRLX: tui_sys(t, "subagent manager: not available on this backend"); break;
    default: break;
    }
}

static void csi_params(const char *p, size_t i, int *a, int *b, int *c) {
    *a = 0;
    *b = 0;
    *c = 0;
    int *slots[3] = {a, b, c};
    int n = 0, v = 0;
    bool any = false;
    for (size_t j = 2; j < i && n < 3; j++) {
        if (p[j] >= '0' && p[j] <= '9') {
            v = v * 10 + (p[j] - '0');
            any = true;
        } else if (p[j] == ';') {
            *slots[n++] = any ? v : 0;
            v = 0;
            any = false;
        }
    }
    if (n < 3 && any) *slots[n] = v;
}

static void set_key(tui_decoded *out, tui_key k) {
    if (out) {
        out->key = k;
        out->ch = NULL;
        out->chlen = 0;
    }
}

/* Decode one key from p[0..n). Returns bytes consumed, 0 if it needs more. */
size_t tui_decode_one(const char *p, size_t n, bool final, tui_decoded *out) {
    if (out) {
        out->key = TUI_K_NONE;
        out->ch = NULL;
        out->chlen = 0;
    }
    if (!p || n == 0) return 0;
    unsigned char c = (unsigned char)p[0];

    if (c != 0x1b) {
        switch (c) {
        case '\r': set_key(out, TUI_K_ENTER); return 1;
        case '\n': set_key(out, TUI_K_NEWLINE); return 1; /* Ctrl-J */
        case 0x7f:
        case 0x08: set_key(out, TUI_K_BS); return 1;
        case 0x01: set_key(out, TUI_K_HOME); return 1;
        case 0x02: set_key(out, TUI_K_LEFT); return 1;
        case 0x03: set_key(out, TUI_K_CTRLC); return 1;
        case 0x04: set_key(out, TUI_K_CTRLD); return 1;
        case 0x05: set_key(out, TUI_K_END); return 1;
        case 0x06: set_key(out, TUI_K_RIGHT); return 1;
        case 0x09: set_key(out, TUI_K_TAB); return 1;
        case 0x0b: set_key(out, TUI_K_KILL_EOL); return 1;
        case 0x0c: set_key(out, TUI_K_CTRLL); return 1;
        case 0x0f: set_key(out, TUI_K_CTRLO); return 1;
        case 0x15: set_key(out, TUI_K_KILL_BOL); return 1;
        case 0x16: set_key(out, TUI_K_PASTE); return 1; /* Ctrl-V */
        case 0x17: set_key(out, TUI_K_WBS); return 1;
        case 0x18: set_key(out, TUI_K_CTRLX); return 1;
        default: break;
        }
        if (c < 0x20) return 1; /* other control bytes: ignore */
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (n < len) {
            if (!final) return 0;
            len = n;
        }
        if (out) {
            out->key = TUI_K_CHAR;
            out->ch = p;
            out->chlen = len;
        }
        return len;
    }

    if (n == 1) {
        if (!final) return 0;
        set_key(out, TUI_K_ESC);
        return 1;
    }
    if (p[1] == '[') {
        size_t i = 2;
        while (i < n && !((unsigned char)p[i] >= 0x40 && (unsigned char)p[i] <= 0x7e)) i++;
        if (i >= n) return final ? n : 0;
        char fin = p[i];
        int a = 0, b = 0, csi_c = 0;
        csi_params(p, i, &a, &b, &csi_c);
        bool ctrl = (b == 5 || b == 3);
        size_t used = i + 1;
        switch (fin) {
        case 'A': set_key(out, TUI_K_UP); break;
        case 'B': set_key(out, TUI_K_DOWN); break;
        case 'C': set_key(out, ctrl ? TUI_K_WRIGHT : TUI_K_RIGHT); break;
        case 'D': set_key(out, ctrl ? TUI_K_WLEFT : TUI_K_LEFT); break;
        case 'H': set_key(out, TUI_K_HOME); break;
        case 'F': set_key(out, TUI_K_END); break;
        case 'Z': set_key(out, TUI_K_TAB); break;
        case 'u': /* kitty / CSI-u */
            if (a == 13) set_key(out, b <= 1 ? TUI_K_ENTER : TUI_K_NEWLINE);
            else if (a == 10 || a == 106) set_key(out, TUI_K_NEWLINE);
            else if (a == 118 && b >= 5) set_key(out, TUI_K_PASTE);
            break;
        case '~':
            if (a == 27) { /* modifyOtherKeys: ESC [ 27 ; mod ; key ~ */
                if (csi_c == 13) set_key(out, b <= 1 ? TUI_K_ENTER : TUI_K_NEWLINE);
                else if (csi_c == 10 || csi_c == 106) set_key(out, TUI_K_NEWLINE);
                else if (csi_c == 118 && b >= 5) set_key(out, TUI_K_PASTE);
            } else if (a == 3) set_key(out, TUI_K_DEL);
            else if (a == 1 || a == 7) set_key(out, TUI_K_HOME);
            else if (a == 4 || a == 8) set_key(out, TUI_K_END);
            else if (a == 200) set_key(out, TUI_K_PASTE_BEGIN);
            /* a == 201: stray paste end with no begin — consume, no key */
            break;
        default: break;
        }
        return used;
    }
    if (p[1] == 'O') {
        if (n < 3) return final ? n : 0;
        switch (p[2]) {
        case 'A': set_key(out, TUI_K_UP); break;
        case 'B': set_key(out, TUI_K_DOWN); break;
        case 'C': set_key(out, TUI_K_RIGHT); break;
        case 'D': set_key(out, TUI_K_LEFT); break;
        case 'H': set_key(out, TUI_K_HOME); break;
        case 'F': set_key(out, TUI_K_END); break;
        default: break;
        }
        return 3;
    }
    switch (p[1]) {
    case '\r':
    case '\n': set_key(out, TUI_K_NEWLINE); break; /* Alt-Enter */
    case 'j':
    case 'J': set_key(out, TUI_K_NEWLINE); break; /* Option-J */
    case 'b': set_key(out, TUI_K_WLEFT); break;
    case 'f': set_key(out, TUI_K_WRIGHT); break;
    case 0x7f: set_key(out, TUI_K_WBS); break;
    default: break;
    }
    return 2;
}

size_t tui_paste_scan(const char *p, size_t n, buf_t *out, bool *done) {
    *done = false;
    size_t i = 0;
    while (i < n) {
        char c = p[i];
        if (c == 0x1b) {
            static const char end[] = "\x1b[201~";
            size_t have = n - i < 6 ? n - i : 6;
            if (memcmp(p + i, end, have) == 0) {
                if (have == 6) {
                    *done = true;
                    return i + 6;
                }
                return i; /* terminator may be split: wait for more bytes */
            }
            buf_append(out, &c, 1); /* stray ESC inside the paste: literal */
            i++;
        } else if (c == '\r') {
            if (i + 1 == n) return i; /* \r\n may straddle the chunk */
            buf_append(out, "\n", 1);
            i += p[i + 1] == '\n' ? 2 : 1;
        } else {
            buf_append(out, &c, 1);
            i++;
        }
    }
    return i;
}

static size_t decode_one(tui *t, const char *p, size_t n, bool final) {
    tui_decoded d;
    size_t used = tui_decode_one(p, n, final, &d);
    if (!used) return 0;
    if (d.key != TUI_K_NONE) do_key(t, (int)d.key, d.ch, d.chlen);
    return used;
}

static char g_kb[4096];
static size_t g_kn;

static bool decode_all(tui *t, bool final) {
    bool progress = false;
    while (g_kn) {
        size_t used;
        if (t->in_paste) {
            buf_t txt;
            buf_init(&txt);
            bool done = false;
            used = tui_paste_scan(g_kb, g_kn, &txt, &done);
            if (txt.len && !t->approval) {
                ins(t, txt.data, txt.len);
                t->dirty = true;
            }
            buf_free(&txt);
            if (done) {
                t->in_paste = false;
                if (!t->approval) tui_pick_refresh(t);
            }
        } else {
            used = decode_one(t, g_kb, g_kn, final);
        }
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

    if (!t->tty) {
        dumb_feed(t, tmp, (size_t)n);
        return (int)n;
    }

    if (g_kn + (size_t)n > sizeof g_kb) g_kn = 0; /* runaway: drop stale bytes */
    memcpy(g_kb + g_kn, tmp, (size_t)n);
    g_kn += (size_t)n;

    for (int tries = 0; g_kn && tries < 3; tries++) {
        decode_all(t, false);
        if (!g_kn) break;
        struct pollfd pf = {0, POLLIN, 0};
        if (tny_poll(&pf, 1, 25) <= 0) break;
        ssize_t m = read(0, tmp, sizeof tmp);
        if (m <= 0) break;
        if (g_kn + (size_t)m > sizeof g_kb) g_kn = 0;
        memcpy(g_kb + g_kn, tmp, (size_t)m);
        g_kn += (size_t)m;
    }
    if (g_kn) decode_all(t, true);
    return (int)n;
}
