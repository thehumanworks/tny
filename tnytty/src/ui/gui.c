/* gui.c — `tnytty gui`: sessions in a native window (docs/adr/0005,
 * docs/adr/0006).
 *
 * One loop, as everywhere else in tnytty: poll(2) owns every pane's pty
 * master, the signal self-pipe and the HTTP fds, and each iteration also
 * drains the window's queued platform events. The poll timeout bounds how
 * long a keystroke can sit in AppKit's queue; nothing else runs a loop and
 * no thread but this one touches a VT core.
 *
 * A window holds a split tree (src/ui/layout.c). Each leaf is a pane with
 * its own session, rasterizer and selection, painting into its own
 * rectangle of the one framebuffer the window blits. */
#include "ui/gui.h"

#include "api/http.h"
#include "session/session.h"
#include "ui/layout.h"
#include "ui/render.h"
#include "ui/reply.h"
#include "ui/selection.h"
#include "ui/status.h"
#include "ui/window.h"
#include "util/config.h"
#include "util/tt.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef TNYTTY_VERSION
#define TNYTTY_VERSION "0.0.0-dev"
#endif

#define SCROLLBACK   2000
#define MAX_LOOP_FDS 64
#define PUMP_TIMEOUT 8 /* ms: AppKit's queue is not a pollable fd */
#define DEFAULT_COLS 100
#define DEFAULT_ROWS 30
#define MAX_PANES    32
/* The rule between siblings, in device pixels: a hairline, like the one
 * iTerm2 and tmux draw. It is a gap in the layout, so no pane owns it. */
#define DIVIDER_PX 1

static const char gui_help[] =
    "usage: tnytty gui [flags] [-- CMD ARGS...]\n"
    "\n"
    "Run a session in a native window (macOS; a clean error elsewhere).\n"
    "\n"
    "flags:\n"
    "  --titlebar transparent|opaque   window titlebar style (default:\n"
    "                                  transparent; config macos-titlebar)\n"
    "  --font NAME                     monospaced font (config font)\n"
    "  --font-size N                   points (config font-size)\n"
    "  --padding N                     points around the grid (config padding)\n"
    "  --cols N --rows N               initial grid (default 100x30)\n"
    "  --listen HOST:PORT              also serve the HTTP API\n"
    "  --token TOKEN                   API bearer token (env TNYTTY_TOKEN)\n"
    "  -- CMD ARGS...                  command to run (default $SHELL)\n"
    "\n"
    "keys: Cmd-D splits right, Cmd-Shift-D splits down, Cmd-W closes the\n"
    "      pane, Cmd-Opt-arrow moves focus, Cmd-[ / Cmd-] cycle panes,\n"
    "      Cmd-Q quits\n"
    "\n"
    "config: $XDG_CONFIG_HOME/tnytty/config (see tnytty/docs/config.md)\n";

static int sig_pipe[2] = {-1, -1};

static void on_signal(int signo) {
    unsigned char b = (unsigned char)signo;
    ssize_t r = write(sig_pipe[1], &b, 1);
    (void)r;
}

static void install_signals(void) {
    if (pipe(sig_pipe) != 0) return;
    fcntl(sig_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(sig_pipe[1], F_SETFL, O_NONBLOCK);
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

static void warn_line(void *user, const char *msg) {
    (void)user;
    fprintf(stderr, "tnytty: %s\n", msg);
}

typedef struct {
    tt_config cfg;
    const char *listen, *token;
    int cols, rows;
    char *const *cmd;
} gui_opts;

static int parse_flags(int argc, char **argv, gui_opts *o) {
    char err[256];
    err[0] = '\0';
    if (tt_config_load(&o->cfg, err, sizeof err, warn_line, NULL) != 0) {
        fprintf(stderr, "tnytty: %s\n", err);
        return -1;
    }
    o->listen = NULL;
    o->token = getenv("TNYTTY_TOKEN");
    o->cols = 0;
    o->rows = 0;
    o->cmd = NULL;

    static const struct {
        const char *flag, *key;
    } cfg_flags[] = {{"--titlebar", "macos-titlebar"},
                     {"--font", "font"},
                     {"--font-size", "font-size"},
                     {"--padding", "padding"}};

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            o->cmd = i + 1 < argc ? &argv[i + 1] : NULL;
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fputs(gui_help, stdout);
            return 1;
        }
        bool matched = false;
        for (size_t f = 0; f < sizeof cfg_flags / sizeof *cfg_flags; f++) {
            if (strcmp(argv[i], cfg_flags[f].flag) != 0) continue;
            if (i + 1 >= argc) {
                fprintf(stderr, "tnytty: %s needs a value\n", argv[i]);
                return -1;
            }
            /* A bad value on the command line is an error, not a warning:
             * the user typed it just now. */
            if (tt_config_set(&o->cfg, cfg_flags[f].key, argv[++i], err, sizeof err) != 0) {
                fprintf(stderr, "tnytty: %s\n", err);
                return -1;
            }
            matched = true;
            break;
        }
        if (matched) continue;
        if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) o->listen = argv[++i];
        else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) o->token = argv[++i];
        else if (strcmp(argv[i], "--cols") == 0 && i + 1 < argc) o->cols = atoi(argv[++i]);
        else if (strcmp(argv[i], "--rows") == 0 && i + 1 < argc) o->rows = atoi(argv[++i]);
        else {
            fprintf(stderr, "tnytty: unknown flag %s (see `tnytty gui --help`)\n", argv[i]);
            return -1;
        }
    }
    return 0;
}

/* ---- panes ------------------------------------------------------------ */

/* One leaf of the split tree: a session, the rasterizer that paints it
 * into this pane's rectangle, and the selection made inside it. */
typedef struct {
    tt_session *s;
    tt_render *r;
    tt_reply reply;
    tt_selection sel;
    char *sel_snapshot;
    tt_node *node;
    tt_render_config cfg; /* this pane's metrics: pad_top varies (below) */
} pane;

/* Everything the event handlers need, so they stay small functions
 * rather than a 400-line switch. */
typedef struct {
    tt_window *win;
    tt_registry *reg;
    tt_layout layout;
    tt_fb fb;
    tt_render_config base; /* window metrics; status_h is the window's bar */
    int titlebar_inset;    /* device pixels folded into pad_top by the window */
    uint32_t divider;
    char *const *cmd;
    tt_status status;
    char status_shown[TT_STATUS_MAX];
    bool relayout; /* the tree or the surface changed: repaint everything */
    bool copy_on_select;
} gui_ctx;

static pane *node_pane(const tt_node *n) { return n ? (pane *)n->user : NULL; }

/* The window folds the transparent titlebar's height into pad_top
 * (window.h). Only panes that touch the top of the grid area sit under
 * the traffic lights, so every other pane gets the plain padding back. */
static tt_render_config pane_config(const gui_ctx *g, tt_rect rect) {
    tt_render_config c = g->base;
    c.status_h = 0; /* the bar belongs to the window, not to a pane */
    if (rect.y > g->layout.area.y) c.pad_top -= g->titlebar_inset;
    return c;
}

static void pane_free(void *user, void *ctx) {
    gui_ctx *g = ctx;
    pane *p = user;
    if (!p) return;
    if (p->s) tt_session_destroy(g->reg, p->s);
    tt_render_free(p->r);
    free(p->sel_snapshot);
    free(p);
}

/* The VT core answers DSR/DA through a callback; in the window there is
 * no outer terminal to forward to, so the answer goes into the pty of
 * the pane that asked. */
static int reply_to_pty(void *user, const char *bytes, size_t len) {
    return tt_session_write((tt_session *)user, bytes, len);
}

/* A pane with a session but no geometry yet; tt_gui_relayout gives it
 * one. Returns NULL (having cleaned up) when anything fails. */
static pane *pane_new(gui_ctx *g, int cols, int rows) {
    pane *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->s = tt_session_create(g->reg, g->cmd, cols, rows);
    if (!p->s) {
        free(p);
        return NULL;
    }
    tt_reply_attach(p->s->term, &p->reply, reply_to_pty, p->s);
    tt_sel_clear(&p->sel);
    return p;
}

/* Give every leaf its rectangle, its metrics and its grid, and resize
 * the session behind it. Called after every split, close and window
 * resize -- the one place pixels become rows and columns. */
static void relayout(gui_ctx *g) {
    int px_w = 0, px_h = 0;
    tt_window_surface(g->win, &px_w, &px_h);
    if (px_w != g->fb.w || px_h != g->fb.h)
        if (tt_fb_alloc(&g->fb, px_w, px_h) != 0) return;
    tt_rect area = {0, 0, g->fb.w, g->fb.h - g->base.status_h};
    if (area.h < 1) area.h = 1;
    tt_layout_apply(&g->layout, area);

    tt_node *leaves[MAX_PANES];
    int n = tt_layout_leaves(&g->layout, leaves, MAX_PANES);
    if (n > MAX_PANES) n = MAX_PANES;
    for (int i = 0; i < n; i++) {
        pane *p = node_pane(leaves[i]);
        if (!p) continue;
        p->cfg = pane_config(g, leaves[i]->rect);
        if (!p->r) p->r = tt_render_new_in(&p->cfg, &g->fb, leaves[i]->rect);
        else if (tt_render_set_area(p->r, leaves[i]->rect) == 0) tt_render_configure(p->r, &p->cfg);
        if (!p->r) continue;
        int cols = tt_render_cols(p->r), rows = tt_render_rows(p->r);
        if (cols != vt_cols(p->s->term) || rows != vt_rows(p->s->term))
            tt_session_resize(p->s, cols, rows);
        /* A selection is anchored to cells that just moved. */
        tt_sel_clear(&p->sel);
        tt_render_set_selection(p->r, &p->sel);
        free(p->sel_snapshot);
        p->sel_snapshot = NULL;
    }
    g->relayout = true;
}

/* ---- selection and clipboard ------------------------------------------ */

/* The selected text, freshly allocated, or NULL when nothing is
 * selected. Callers own the result. */
static char *sel_dup(const tt_selection *sel, const vt *t) {
    if (!tt_sel_active(sel)) return NULL;
    size_t n = tt_sel_text(sel, t, NULL, 0);
    char *buf = malloc(n + 1);
    if (!buf) return NULL;
    tt_sel_text(sel, t, buf, n + 1);
    return buf;
}

/* Monotonic seconds for the status-bar timer: the one event loop is the
 * only clock, no timer thread (docs/adr/0005). */
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void copy_selection(gui_ctx *g, pane *p) {
    char *text = sel_dup(&p->sel, p->s->term);
    if (text && *text) {
        tt_window_set_clipboard(g->win, text, strlen(text));
        tt_status_copied(&g->status, text, now_sec());
    }
    free(text);
}

/* Paste as the child asked for it: bracketed when it enabled mode 2004,
 * raw bytes otherwise. Newlines stay newlines; a paste is not a keypress. */
static void paste_clipboard(gui_ctx *g, pane *p) {
    const char *text = tt_window_clipboard(g->win);
    if (!text || !*text) return;
    bool bracketed = vt_bracketed_paste(p->s->term);
    if (bracketed) tt_session_write(p->s, "\x1b[200~", 6);
    tt_session_write(p->s, text, strlen(text));
    if (bracketed) tt_session_write(p->s, "\x1b[201~", 6);
}

/* Only one pane may hold a selection: starting one drops the others, so
 * Cmd-C and copy-on-select never have to guess which is meant. */
static void clear_other_selections(gui_ctx *g, const pane *keep) {
    tt_node *leaves[MAX_PANES];
    int n = tt_layout_leaves(&g->layout, leaves, MAX_PANES);
    if (n > MAX_PANES) n = MAX_PANES;
    for (int i = 0; i < n; i++) {
        pane *p = node_pane(leaves[i]);
        if (!p || p == keep || !tt_sel_active(&p->sel)) continue;
        tt_sel_clear(&p->sel);
        tt_render_set_selection(p->r, &p->sel);
        free(p->sel_snapshot);
        p->sel_snapshot = NULL;
    }
}

/* ---- splitting and closing -------------------------------------------- */

static void split_focus(gui_ctx *g, tt_split_dir dir) {
    tt_node *from = g->layout.focus;
    if (!from || tt_layout_count(&g->layout) >= MAX_PANES) return;
    /* The new session starts at the old pane's size and is resized by
     * the relayout below; spawning first keeps the tree unchanged when
     * the fork fails. */
    pane *p = pane_new(g, vt_cols(node_pane(from)->s->term), vt_rows(node_pane(from)->s->term));
    if (!p) {
        tt_status_set(&g->status, "split failed: cannot start a shell", now_sec());
        return;
    }
    tt_node *leaf = tt_layout_split(&g->layout, from, dir, p);
    if (!leaf) {
        pane_free(p, g);
        return;
    }
    /* `from` is now the internal node; the pane that was there moved to
     * its first child, and has to re-learn which leaf it lives on. */
    p->node = leaf;
    node_pane(leaf->parent->a)->node = leaf->parent->a;
    relayout(g);
}

/* Returns false when the pane closed was the last one: the window goes
 * with it. */
static bool close_pane(gui_ctx *g, pane *p, int *exit_code) {
    if (!p || !p->node) return true;
    if (tt_layout_count(&g->layout) <= 1) {
        if (exit_code && p->s) *exit_code = p->s->exit_code;
        return false;
    }
    tt_node *node = p->node;
    if (!tt_layout_close(&g->layout, node)) return false;
    pane_free(p, g);
    /* Splicing moved payloads between nodes; every leaf re-learns its
     * own node so a later close addresses the right one. */
    tt_node *leaves[MAX_PANES];
    int n = tt_layout_leaves(&g->layout, leaves, MAX_PANES);
    if (n > MAX_PANES) n = MAX_PANES;
    for (int i = 0; i < n; i++)
        if (node_pane(leaves[i])) node_pane(leaves[i])->node = leaves[i];
    relayout(g);
    return true;
}

/* ---- events ------------------------------------------------------------ */

static void on_mouse(gui_ctx *g, const tt_win_ev *ev) {
    pane *p = NULL;
    if (ev->type == TT_WIN_EV_MOUSE_DOWN) {
        tt_node *hit = tt_layout_at(&g->layout, ev->px_x, ev->px_y);
        if (!hit) return; /* a divider, the status bar, or nothing */
        g->layout.focus = hit;
        p = node_pane(hit);
    } else {
        p = node_pane(g->layout.focus); /* a drag stays in the pane it began in */
    }
    if (!p || !p->r) return;

    tt_rect area = tt_render_area(p->r);
    int col = 0, row = 0;
    int lx = ev->px_x - area.x, ly = ev->px_y - area.y;
    bool on_grid =
        tt_render_cell_hit(&p->cfg, lx, ly, tt_render_cols(p->r), tt_render_rows(p->r), &col, &row);
    if (ev->type == TT_WIN_EV_MOUSE_DOWN) {
        /* A press in the padding, under the traffic lights or past the
         * last row is not a cell: starting a selection there would clamp
         * it onto the corner cell and leave a block that looks like a
         * second caret (docs/adr/0006). */
        if (!on_grid) return;
        clear_other_selections(g, p);
        tt_sel_begin(&p->sel, p->s->term, col, row, ev->clicks);
    } else if (ev->type == TT_WIN_EV_MOUSE_DRAG) {
        tt_sel_extend(&p->sel, p->s->term, col, row);
    } else {
        if (tt_sel_finish(&p->sel, p->s->term) && g->copy_on_select) copy_selection(g, p);
        free(p->sel_snapshot);
        p->sel_snapshot = sel_dup(&p->sel, p->s->term);
    }
    tt_render_set_selection(p->r, &p->sel);
}

static void on_chord(gui_ctx *g, tt_chord chord, bool *quit, int *exit_code) {
    pane *p = node_pane(g->layout.focus);
    if (!p) return;
    switch (chord) {
    case TT_CHORD_COPY: copy_selection(g, p); break;
    case TT_CHORD_PASTE: paste_clipboard(g, p); break;
    case TT_CHORD_SPLIT_VERT: split_focus(g, TT_SPLIT_VERT); break;
    case TT_CHORD_SPLIT_HORZ: split_focus(g, TT_SPLIT_HORZ); break;
    case TT_CHORD_CLOSE_PANE:
        if (!close_pane(g, p, exit_code)) *quit = true;
        break;
    case TT_CHORD_FOCUS_LEFT:
    case TT_CHORD_FOCUS_RIGHT:
    case TT_CHORD_FOCUS_UP:
    case TT_CHORD_FOCUS_DOWN: {
        tt_move_dir dir = chord == TT_CHORD_FOCUS_LEFT    ? TT_MOVE_LEFT
                          : chord == TT_CHORD_FOCUS_RIGHT ? TT_MOVE_RIGHT
                          : chord == TT_CHORD_FOCUS_UP    ? TT_MOVE_UP
                                                          : TT_MOVE_DOWN;
        tt_node *to = tt_layout_neighbour(&g->layout, g->layout.focus, dir);
        if (to) g->layout.focus = to;
        break;
    }
    case TT_CHORD_FOCUS_PREV:
    case TT_CHORD_FOCUS_NEXT: {
        tt_node *to =
            tt_layout_cycle(&g->layout, g->layout.focus, chord == TT_CHORD_FOCUS_NEXT ? 1 : -1);
        if (to) g->layout.focus = to;
        break;
    }
    case TT_CHORD_NONE: break;
    }
}

/* ---- main -------------------------------------------------------------- */

int tt_gui_main(int argc, char **argv) {
    gui_opts o;
    int rc = parse_flags(argc, argv, &o);
    if (rc != 0) return rc < 0 ? 2 : 0;

    char err[256];
    err[0] = '\0';
    int cols = o.cols > 0 ? o.cols : DEFAULT_COLS;
    int rows = o.rows > 0 ? o.rows : DEFAULT_ROWS;

    tt_window *win = tt_window_open(&o.cfg, cols, rows, "tnytty", err, sizeof err);
    if (!win) {
        fprintf(stderr, "tnytty: %s\n", err);
        return 1;
    }

    tt_registry reg;
    tt_registry_init(&reg, SCROLLBACK);

    gui_ctx g;
    memset(&g, 0, sizeof g);
    g.win = win;
    g.reg = &reg;
    g.cmd = o.cmd;
    g.divider = o.cfg.divider;
    g.copy_on_select = o.cfg.copy_on_select;
    tt_window_render_config(win, &g.base);
    /* window.h folds the titlebar height into pad_top and leaves the
     * other three at the configured padding, so the inset is the
     * difference; panes below a horizontal split give it back. */
    g.titlebar_inset = g.base.pad_top - g.base.pad_bottom;
    if (g.titlebar_inset < 0) g.titlebar_inset = 0;
    tt_status_clear(&g.status);

    int px_w = 0, px_h = 0;
    tt_window_surface(win, &px_w, &px_h);
    if (tt_fb_alloc(&g.fb, px_w, px_h) != 0) {
        fprintf(stderr, "tnytty: gui: out of memory\n");
        tt_registry_free(&reg);
        tt_window_close(win);
        return 1;
    }
    /* One pane's worth of grid, to size the first session before the
     * layout exists; relayout() gives it the exact numbers. */
    tt_render_grid_for(&g.base, px_w, px_h, &cols, &rows);

    if (tt_layout_init(&g.layout, DIVIDER_PX, NULL) != 0) {
        fprintf(stderr, "tnytty: gui: out of memory\n");
        tt_fb_free(&g.fb);
        tt_registry_free(&reg);
        tt_window_close(win);
        return 1;
    }
    pane *root_pane = pane_new(&g, cols, rows);
    if (!root_pane) {
        fprintf(stderr, "tnytty: spawn failed: %s\n", strerror(errno));
        tt_layout_free(&g.layout, NULL, NULL);
        tt_fb_free(&g.fb);
        tt_registry_free(&reg);
        tt_window_close(win);
        return 1;
    }
    g.layout.root->user = root_pane;
    root_pane->node = g.layout.root;
    relayout(&g);

    tt_api api = {&reg, o.token, TNYTTY_VERSION};
    tt_http *http = NULL;
    if (o.listen) {
        char host[128];
        int port = 0;
        const char *colon = strrchr(o.listen, ':');
        size_t hl = colon ? (size_t)(colon - o.listen) : 0;
        if (colon && hl > 0 && hl < sizeof host) {
            memcpy(host, o.listen, hl);
            host[hl] = '\0';
            port = atoi(colon + 1);
        }
        if (port < 1 || port > 65535) {
            fprintf(stderr, "tnytty: bad --listen %s (want HOST:PORT)\n", o.listen);
        } else {
            http = tt_http_listen(&api, host, port, err, sizeof err);
            if (!http) fprintf(stderr, "tnytty: %s\n", err);
            else fprintf(stderr, "tnytty: HTTP API on http://%s:%d\n", host, port);
        }
    }
    install_signals();

    bool focused = true;
    bool quit = false;
    int exit_code = 0;
    char scratch[16384];
    char last_title[128] = {0};
    struct pollfd fds[8 + MAX_LOOP_FDS + MAX_PANES];
    int pty_i[MAX_PANES];
    tt_node *leaves[MAX_PANES];

    while (!quit) {
        tt_win_ev ev;
        while (tt_window_pump(win, &ev)) {
            switch (ev.type) {
            case TT_WIN_EV_KEY: {
                pane *p = node_pane(g.layout.focus);
                char out[64];
                if (!p) break;
                size_t n = tt_key_encode(ev.key, ev.text, ev.text_len, ev.mods,
                                         vt_app_cursor(p->s->term), out, sizeof out);
                if (n) tt_session_write(p->s, out, n);
                break;
            }
            case TT_WIN_EV_MOUSE_DOWN:
            case TT_WIN_EV_MOUSE_DRAG:
            case TT_WIN_EV_MOUSE_UP: on_mouse(&g, &ev); break;
            case TT_WIN_EV_CHORD: on_chord(&g, ev.chord, &quit, &exit_code); break;
            case TT_WIN_EV_RESIZE: relayout(&g); break;
            case TT_WIN_EV_FOCUS:
                focused = ev.focused;
                /* A drag whose mouse-up went to another app must not stay
                 * live: it would keep painting a highlight forever. */
                if (!focused) {
                    pane *p = node_pane(g.layout.focus);
                    if (p && p->sel.dragging) {
                        tt_sel_finish(&p->sel, p->s->term);
                        tt_render_set_selection(p->r, &p->sel);
                    }
                }
                break;
            case TT_WIN_EV_CLOSE: quit = true; break;
            case TT_WIN_EV_NONE: break;
            }
            if (quit) break;
        }
        if (quit) break;

        int npanes = tt_layout_leaves(&g.layout, leaves, MAX_PANES);
        if (npanes > MAX_PANES) npanes = MAX_PANES;

        int n = 0;
        int sig_i = -1, http_i = -1;
        if (sig_pipe[0] >= 0) {
            sig_i = n;
            fds[n].fd = sig_pipe[0];
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }
        for (int i = 0; i < npanes; i++) {
            pane *p = node_pane(leaves[i]);
            pty_i[i] = -1;
            if (!p || !p->s->alive) continue;
            pty_i[i] = n;
            fds[n].fd = p->s->pty.master;
            fds[n].events = POLLIN | (tt_session_pending(p->s) ? POLLOUT : 0);
            fds[n].revents = 0;
            n++;
        }
        if (http) {
            http_i = n;
            n += tt_http_fill(http, fds + n, MAX_LOOP_FDS);
        }
        if (poll(fds, (nfds_t)n, PUMP_TIMEOUT) < 0 && errno != EINTR) break;

        if (sig_i >= 0 && (fds[sig_i].revents & POLLIN)) {
            unsigned char sig;
            while (read(sig_pipe[0], &sig, 1) == 1)
                if (sig == SIGINT || sig == SIGTERM) quit = true;
        }
        for (int i = 0; i < npanes && !quit; i++) {
            pane *p = node_pane(leaves[i]);
            if (!p || pty_i[i] < 0) continue;
            if (fds[pty_i[i]].revents & POLLOUT) tt_session_flush(p->s);
            if (!(fds[pty_i[i]].revents & (POLLIN | POLLHUP))) continue;
            while (tt_session_pump(p->s, scratch, sizeof scratch, NULL, NULL) > 0) {}
            if (p->s->alive) continue;
            /* A pane whose child exited leaves the tree; its sibling
             * takes the space. The last one takes the window with it. */
            int code = p->s->exit_code;
            if (!close_pane(&g, p, &code)) {
                exit_code = code;
                quit = true;
            }
            break; /* the leaf array is stale now; rebuild it next turn */
        }
        if (quit) break;
        if (http && http_i >= 0) tt_http_handle(http, fds + http_i, n - http_i);

        pane *fp = node_pane(g.layout.focus);
        const char *title = fp ? vt_title(fp->s->term) : NULL;
        if (title && *title && strcmp(title, last_title) != 0) {
            snprintf(last_title, sizeof last_title, "%s", title);
            tt_window_set_title(win, title);
        }

        tt_status_tick(&g.status, now_sec());

        /* One frame: the divider ground and the status bar are the
         * window's, every cell is a pane's. */
        int y0 = g.fb.h, y1 = 0, painted = 0;
        if (g.relayout) {
            tt_fb_fill(&g.fb, g.layout.area.x, g.layout.area.y, g.layout.area.w, g.layout.area.h,
                       g.divider);
            g.status_shown[0] = '\1'; /* force the bar to repaint too */
            y0 = 0;
            y1 = g.fb.h;
            painted++;
        }
        npanes = tt_layout_leaves(&g.layout, leaves, MAX_PANES);
        if (npanes > MAX_PANES) npanes = MAX_PANES;
        for (int i = 0; i < npanes; i++) {
            pane *p = node_pane(leaves[i]);
            if (!p || !p->r) continue;
            /* A selection belongs to the text it was made from: when that
             * text scrolls or is overwritten, drop the highlight rather
             * than leave it pointing at something never selected. */
            if (tt_sel_active(&p->sel) && !p->sel.dragging && p->sel_snapshot) {
                char *now = sel_dup(&p->sel, p->s->term);
                if (!now || strcmp(now, p->sel_snapshot) != 0) {
                    tt_sel_clear(&p->sel);
                    tt_render_set_selection(p->r, &p->sel);
                    free(p->sel_snapshot);
                    p->sel_snapshot = NULL;
                }
                free(now);
            }
            int a = 0, b = 0;
            int lines =
                tt_render_frame(p->r, p->s->term, focused && leaves[i] == g.layout.focus, &a, &b);
            if (lines <= 0) continue;
            painted += lines;
            if (a < y0) y0 = a;
            if (b > y1) y1 = b;
        }
        const char *msg = tt_status_text(&g.status);
        if (g.base.status_h > 0 && strcmp(msg, g.status_shown) != 0) {
            tt_render_status_bar(&g.fb, &g.base, msg);
            snprintf(g.status_shown, sizeof g.status_shown, "%s", msg);
            painted++;
            if (g.fb.h - g.base.status_h < y0) y0 = g.fb.h - g.base.status_h;
            if (g.fb.h > y1) y1 = g.fb.h;
        }
        g.relayout = false;
        if (painted > 0 && y1 > y0) tt_window_present(win, g.fb.px, g.fb.w, g.fb.h, y0, y1);
    }

    tt_layout_free(&g.layout, pane_free, &g);
    tt_fb_free(&g.fb);
    tt_http_free(http);
    tt_window_close(win);
    tt_registry_free(&reg);
    return exit_code;
}
