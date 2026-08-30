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

#include "broker/client.h"
#include "session/session.h"
#include "ui/layout.h"
#include "ui/render.h"
#include "ui/selection.h"
#include "ui/status.h"
#include "ui/tabs.h"
#include "ui/window.h"
#include "ui/workspace.h"
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
    for (int i = 0; i < argc && strcmp(argv[i], "--") != 0; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fputs(gui_help, stdout);
            return 1;
        }
    }
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
    char session_id[TT_SESSION_ID_LEN + 1];
    vt *term; /* frontend mirror; the broker is authoritative */
    tt_render *r;
    tt_selection sel;
    char *sel_snapshot;
    tt_node *node;
    tt_render_config cfg; /* this pane's metrics: pad_top varies (below) */
    uint64_t generation;
} pane;

typedef struct {
    tt_layout layout;
    bool activity;
} gui_tab;

/* Everything the event handlers need, so they stay small functions
 * rather than a 400-line switch. */
typedef struct {
    tt_window *win;
    tt_broker_client broker;
    tt_tabs tabs;
    tt_fb fb;
    tt_render_config base; /* window metrics; status_h is the window's bar */
    int titlebar_inset;    /* device pixels folded into pad_top by the window */
    uint32_t divider;
    char *const *cmd;
    tt_status status;
    char status_shown[TT_STATUS_MAX];
    bool relayout; /* the tree or the surface changed: repaint everything */
    bool copy_on_select;
    int tab_h;
    bool tabs_dirty;
    pane *syncing;
    gui_tab *syncing_tab;
    int sync_cursor;
    char workspace_path[1024];
} gui_ctx;

static pane *node_pane(const tt_node *n) { return n ? (pane *)n->user : NULL; }
static gui_tab *active_tab(const gui_ctx *g) { return (gui_tab *)tt_tabs_active(&g->tabs); }
static tt_layout *active_layout(gui_ctx *g) {
    gui_tab *tab = active_tab(g);
    return tab ? &tab->layout : NULL;
}
static void save_workspace(gui_ctx *g);

/* The window folds the transparent titlebar's height into pad_top
 * (window.h). Only panes that touch the top of the grid area sit under
 * the traffic lights, so every other pane gets the plain padding back. */
static tt_render_config pane_config(const gui_ctx *g, tt_rect rect) {
    tt_render_config c = g->base;
    c.status_h = 0; /* the bar belongs to the window, not to a pane */
    (void)rect;
    /* The tab strip already reserves the transparent titlebar inset. */
    c.pad_top = c.pad_bottom;
    return c;
}

static void pane_free(void *user, void *ctx) {
    (void)ctx;
    pane *p = user;
    if (!p) return;
    vt_free(p->term);
    tt_render_free(p->r);
    free(p->sel_snapshot);
    free(p);
}

static void pane_detach(gui_ctx *g, pane *p) {
    if (g->syncing == p) {
        g->syncing = NULL;
        g->syncing_tab = NULL;
    }
    if (p && p->session_id[0]) (void)tt_broker_client_detach(&g->broker, p->session_id);
}

static void pane_kill(gui_ctx *g, pane *p) {
    if (g->syncing == p) {
        g->syncing = NULL;
        g->syncing_tab = NULL;
    }
    if (p && p->session_id[0]) (void)tt_broker_client_kill(&g->broker, p->session_id);
}

/* A pane with a session but no geometry yet; tt_gui_relayout gives it
 * one. Returns NULL (having cleaned up) when anything fails. */
static pane *pane_attach(gui_ctx *g, const char *session_id, int cols, int rows) {
    pane *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->term = vt_new(cols, rows, SCROLLBACK);
    if (!p->term) {
        free(p);
        return NULL;
    }
    snprintf(p->session_id, sizeof p->session_id, "%s", session_id);
    if (tt_broker_client_attach(&g->broker, p->session_id) != 0) {
        pane_free(p, NULL);
        return NULL;
    }
    tt_buf snapshot;
    if (tt_broker_client_snapshot(&g->broker, p->session_id, 0, &snapshot) != 0 ||
        vt_snapshot_read(p->term, snapshot.data, snapshot.len) != 0) {
        tt_buf_free(&snapshot);
        pane_detach(g, p);
        pane_free(p, NULL);
        return NULL;
    }
    tt_buf_free(&snapshot);
    p->generation = vt_generation(p->term);
    tt_sel_clear(&p->sel);
    return p;
}

static pane *pane_new(gui_ctx *g, int cols, int rows, const char *cwd) {
    char id[TT_SESSION_ID_LEN + 1];
    if (tt_broker_client_create(&g->broker, g->cmd, cwd, cols, rows, id) != 0) return NULL;
    pane *p = pane_attach(g, id, cols, rows);
    if (!p) (void)tt_broker_client_kill(&g->broker, id);
    return p;
}

/* Give every leaf its rectangle, its metrics and its grid, and resize
 * the session behind it. Called after every split, close and window
 * resize -- the one place pixels become rows and columns. */
static void relayout(gui_ctx *g) {
    tt_layout *layout = active_layout(g);
    if (!layout) return;
    int px_w = 0, px_h = 0;
    tt_window_surface(g->win, &px_w, &px_h);
    if (px_w != g->fb.w || px_h != g->fb.h)
        if (tt_fb_alloc(&g->fb, px_w, px_h) != 0) return;
    tt_rect area = {0, g->tab_h, g->fb.w, g->fb.h - g->tab_h - g->base.status_h};
    if (area.h < 1) area.h = 1;
    tt_layout_apply(layout, area);

    tt_node *leaves[MAX_PANES];
    int n = tt_layout_leaves(layout, leaves, MAX_PANES);
    if (n > MAX_PANES) n = MAX_PANES;
    for (int i = 0; i < n; i++) {
        pane *p = node_pane(leaves[i]);
        if (!p) continue;
        p->cfg = pane_config(g, leaves[i]->rect);
        if (!p->r) p->r = tt_render_new_in(&p->cfg, &g->fb, leaves[i]->rect);
        else if (tt_render_set_area(p->r, leaves[i]->rect) == 0) tt_render_configure(p->r, &p->cfg);
        if (!p->r) continue;
        int cols = tt_render_cols(p->r), rows = tt_render_rows(p->r);
        if (cols != vt_cols(p->term) || rows != vt_rows(p->term))
            (void)tt_broker_client_resize(&g->broker, p->session_id, cols, rows);
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
    char *text = sel_dup(&p->sel, p->term);
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
    bool bracketed = vt_bracketed_paste(p->term);
    size_t len = strlen(text);
    char *framed = NULL;
    const char *bytes = text;
    if (bracketed) {
        if (len > TT_INPUT_QUEUE_MAX - 12 || !(framed = malloc(len + 12))) {
            tt_status_set(&g->status, "paste failed: input is too large", now_sec());
            return;
        }
        memcpy(framed, "\x1b[200~", 6);
        memcpy(framed + 6, text, len);
        memcpy(framed + 6 + len, "\x1b[201~", 6);
        bytes = framed;
        len += 12;
    }
    if (tt_broker_client_input(&g->broker, p->session_id, bytes, len) < 0)
        tt_status_set(&g->status,
                      errno == ENOBUFS ? "paste failed: input queue is full"
                                       : "paste failed: session is not writable",
                      now_sec());
    free(framed);
}

/* Only one pane may hold a selection: starting one drops the others, so
 * Cmd-C and copy-on-select never have to guess which is meant. */
static void clear_other_selections(gui_ctx *g, const pane *keep) {
    tt_layout *layout = active_layout(g);
    if (!layout) return;
    tt_node *leaves[MAX_PANES];
    int n = tt_layout_leaves(layout, leaves, MAX_PANES);
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

static void finish_selection(gui_ctx *g, pane *p) {
    if (!p || !p->sel.dragging) return;
    if (tt_sel_finish(&p->sel, p->term) && g->copy_on_select) copy_selection(g, p);
    free(p->sel_snapshot);
    p->sel_snapshot = sel_dup(&p->sel, p->term);
    tt_render_set_selection(p->r, &p->sel);
}

/* ---- splitting and closing -------------------------------------------- */

static void split_focus(gui_ctx *g, tt_split_dir dir) {
    tt_layout *layout = active_layout(g);
    if (!layout) return;
    tt_node *from = layout->focus;
    if (!from || tt_layout_count(layout) >= MAX_PANES) return;
    /* The new session starts at the old pane's size and is resized by
     * the relayout below; spawning first keeps the tree unchanged when
     * the fork fails. */
    pane *source = node_pane(from);
    const char *cwd = vt_cwd(source->term);
    pane *p = pane_new(g, vt_cols(source->term), vt_rows(source->term), cwd && *cwd ? cwd : NULL);
    if (!p) {
        tt_status_set(&g->status, "split failed: cannot start a shell", now_sec());
        return;
    }
    tt_node *leaf = tt_layout_split(layout, from, dir, p);
    if (!leaf) {
        pane_kill(g, p);
        pane_free(p, NULL);
        return;
    }
    /* `from` is now the internal node; the pane that was there moved to
     * its first child, and has to re-learn which leaf it lives on. */
    p->node = leaf;
    node_pane(leaf->parent->a)->node = leaf->parent->a;
    g->tabs_dirty = true;
    relayout(g);
    save_workspace(g);
}

/* Returns false when the pane closed was the last one: the window goes
 * with it. */
static bool close_pane(gui_ctx *g, pane *p, int *exit_code) {
    (void)exit_code;
    tt_layout *layout = active_layout(g);
    if (!layout) return false;
    if (!p || !p->node) return true;
    if (tt_layout_count(layout) <= 1) return false;
    tt_node *node = p->node;
    if (!tt_layout_close(layout, node)) return false;
    pane_kill(g, p);
    pane_free(p, NULL);
    /* Splicing moved payloads between nodes; every leaf re-learns its
     * own node so a later close addresses the right one. */
    tt_node *leaves[MAX_PANES];
    int n = tt_layout_leaves(layout, leaves, MAX_PANES);
    if (n > MAX_PANES) n = MAX_PANES;
    for (int i = 0; i < n; i++)
        if (node_pane(leaves[i])) node_pane(leaves[i])->node = leaves[i];
    g->tabs_dirty = true;
    relayout(g);
    save_workspace(g);
    return true;
}

static gui_tab *tab_new(gui_ctx *g, pane *first) {
    gui_tab *tab = calloc(1, sizeof *tab);
    if (!tab) return NULL;
    if (tt_layout_init(&tab->layout, DIVIDER_PX, first) != 0) {
        free(tab);
        return NULL;
    }
    first->node = tab->layout.root;
    if (tt_tabs_add(&g->tabs, tab) < 0) {
        tt_layout_free(&tab->layout, NULL, NULL);
        free(tab);
        return NULL;
    }
    g->tabs_dirty = true;
    return tab;
}

static void tab_release(gui_ctx *g, gui_tab *tab, bool kill) {
    if (!tab) return;
    tt_node *leaves[MAX_PANES];
    int n = tt_layout_leaves(&tab->layout, leaves, MAX_PANES);
    if (n > MAX_PANES) n = MAX_PANES;
    for (int i = 0; i < n; i++) {
        pane *p = node_pane(leaves[i]);
        if (kill) pane_kill(g, p);
        else pane_detach(g, p);
    }
    tt_layout_free(&tab->layout, pane_free, NULL);
    free(tab);
}

static bool close_active_tab(gui_ctx *g) {
    int at = tt_tabs_active_index(&g->tabs);
    gui_tab *tab = active_tab(g);
    if (!tab) return false;
    (void)tt_tabs_remove(&g->tabs, at);
    tab_release(g, tab, true);
    g->tabs_dirty = true;
    if (tt_tabs_count(&g->tabs) == 0) {
        save_workspace(g);
        return false;
    }
    active_tab(g)->activity = false;
    relayout(g);
    save_workspace(g);
    return true;
}

static void select_tab(gui_ctx *g, int index) {
    if (!tt_tabs_select(&g->tabs, index)) return;
    gui_tab *tab = active_tab(g);
    tab->activity = false;
    g->tabs_dirty = true;
    relayout(g);
    save_workspace(g);
}

static void new_tab(gui_ctx *g) {
    if (tt_tabs_count(&g->tabs) >= TT_TABS_MAX) return;
    gui_tab *current = active_tab(g);
    pane *focused = current ? node_pane(current->layout.focus) : NULL;
    const char *cwd = focused ? vt_cwd(focused->term) : NULL;
    int cols = focused ? vt_cols(focused->term) : DEFAULT_COLS;
    int rows = focused ? vt_rows(focused->term) : DEFAULT_ROWS;
    pane *p = pane_new(g, cols, rows, cwd && *cwd ? cwd : NULL);
    if (!p) {
        tt_status_set(&g->status, "new tab failed: cannot start a shell", now_sec());
        return;
    }
    if (!tab_new(g, p)) {
        pane_kill(g, p);
        pane_free(p, NULL);
        return;
    }
    relayout(g);
    save_workspace(g);
}

static int capture_node(const tt_node *node, const tt_node *focus, tt_workspace_tab *out) {
    if (!node || out->node_count >= TT_WORKSPACE_MAX_NODES) return -1;
    int at = out->node_count++;
    tt_workspace_node *dst = &out->nodes[at];
    memset(dst, 0, sizeof *dst);
    dst->dir = node->dir;
    dst->ratio = node->ratio;
    dst->a = dst->b = -1;
    if (node == focus) out->focus = at;
    if (tt_layout_is_leaf(node)) {
        pane *p = node_pane(node);
        if (!p) return -1;
        snprintf(dst->session_id, sizeof dst->session_id, "%s", p->session_id);
        return at;
    }
    int a = capture_node(node->a, focus, out);
    int b = capture_node(node->b, focus, out);
    if (a < 0 || b < 0) return -1;
    dst = &out->nodes[at]; /* recursion may not move the fixed array, but be explicit */
    dst->a = a;
    dst->b = b;
    return at;
}

static int capture_workspace(const gui_ctx *g, tt_workspace *ws) {
    tt_workspace_init(ws);
    ws->tab_count = tt_tabs_count(&g->tabs);
    ws->active = tt_tabs_active_index(&g->tabs);
    for (int i = 0; i < ws->tab_count; i++) {
        const gui_tab *tab = tt_tabs_at(&g->tabs, i);
        tt_workspace_tab *dst = &ws->tabs[i];
        dst->focus = -1;
        int root = capture_node(tab->layout.root, tab->layout.focus, dst);
        if (root < 0 || dst->focus < 0) return -1;
        dst->root = root;
    }
    return 0;
}

static void save_workspace(gui_ctx *g) {
    if (!g->workspace_path[0]) return;
    tt_workspace ws;
    char err[160];
    if (capture_workspace(g, &ws) != 0 ||
        tt_workspace_save(g->workspace_path, &ws, err, sizeof err) != 0)
        tt_status_set(&g->status, "workspace could not be saved", now_sec());
}

static void free_restored_node(tt_node *n) {
    if (!n) return;
    free_restored_node(n->a);
    free_restored_node(n->b);
    if (tt_layout_is_leaf(n)) pane_free(n->user, NULL);
    free(n);
}

static tt_node *restore_node(gui_ctx *g, const tt_workspace_tab *src, int at, tt_node *parent,
                             tt_node **focus) {
    if (at < 0 || at >= src->node_count) return NULL;
    const tt_workspace_node *saved = &src->nodes[at];
    tt_node *node = calloc(1, sizeof *node);
    if (!node) return NULL;
    node->dir = saved->dir;
    node->ratio = saved->ratio;
    node->parent = parent;
    if (saved->dir == TT_SPLIT_LEAF) {
        pane *p = pane_attach(g, saved->session_id, DEFAULT_COLS, DEFAULT_ROWS);
        if (!p) {
            free(node);
            return NULL;
        }
        node->user = p;
        p->node = node;
    } else {
        node->a = restore_node(g, src, saved->a, node, focus);
        node->b = restore_node(g, src, saved->b, node, focus);
        if (!node->a || !node->b) {
            free_restored_node(node);
            return NULL;
        }
    }
    if (at == src->focus) *focus = node;
    return node;
}

static int restore_workspace(gui_ctx *g, const tt_workspace *ws) {
    for (int i = 0; i < ws->tab_count; i++) {
        gui_tab *tab = calloc(1, sizeof *tab);
        if (!tab) return -1;
        tab->layout.divider = DIVIDER_PX;
        tab->layout.focus = NULL;
        tab->layout.root = restore_node(g, &ws->tabs[i], ws->tabs[i].root, NULL,
                                        &tab->layout.focus);
        if (!tab->layout.root || !tab->layout.focus || tt_tabs_add(&g->tabs, tab) < 0) {
            free_restored_node(tab->layout.root);
            free(tab);
            return -1;
        }
    }
    if (ws->tab_count > 0) tt_tabs_select(&g->tabs, ws->active);
    g->tabs_dirty = true;
    return ws->tab_count;
}

/* ---- events ------------------------------------------------------------ */

static void on_mouse(gui_ctx *g, const tt_win_ev *ev) {
    tt_layout *layout = active_layout(g);
    if (!layout) return;
    pane *p = NULL;
    if (ev->type == TT_WIN_EV_MOUSE_DOWN) {
        if (ev->px_y >= g->titlebar_inset && ev->px_y < g->tab_h) {
            int at = tt_render_tab_at(g->fb.w, tt_tabs_count(&g->tabs), ev->px_x);
            if (at >= 0) select_tab(g, at);
            return;
        }
        tt_node *hit = tt_layout_at(layout, ev->px_x, ev->px_y);
        if (!hit) return; /* a divider, the status bar, or nothing */
        layout->focus = hit;
        p = node_pane(hit);
    } else {
        p = node_pane(layout->focus); /* a drag stays in the pane it began in */
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
        tt_sel_begin(&p->sel, p->term, col, row, ev->clicks);
    } else if (ev->type == TT_WIN_EV_MOUSE_DRAG) {
        tt_sel_extend(&p->sel, p->term, col, row);
    } else {
        finish_selection(g, p);
    }
    tt_render_set_selection(p->r, &p->sel);
}

static void on_chord(gui_ctx *g, tt_chord chord, bool *quit, int *exit_code) {
    tt_layout *layout = active_layout(g);
    if (!layout) return;
    pane *p = node_pane(layout->focus);
    if (!p) return;
    switch (chord) {
    case TT_CHORD_COPY: copy_selection(g, p); break;
    case TT_CHORD_PASTE: paste_clipboard(g, p); break;
    case TT_CHORD_SPLIT_VERT: split_focus(g, TT_SPLIT_VERT); break;
    case TT_CHORD_SPLIT_HORZ: split_focus(g, TT_SPLIT_HORZ); break;
    case TT_CHORD_CLOSE_PANE:
        if (!close_pane(g, p, exit_code) && !close_active_tab(g)) *quit = true;
        break;
    case TT_CHORD_FOCUS_LEFT:
    case TT_CHORD_FOCUS_RIGHT:
    case TT_CHORD_FOCUS_UP:
    case TT_CHORD_FOCUS_DOWN: {
        tt_move_dir dir = chord == TT_CHORD_FOCUS_LEFT    ? TT_MOVE_LEFT
                          : chord == TT_CHORD_FOCUS_RIGHT ? TT_MOVE_RIGHT
                          : chord == TT_CHORD_FOCUS_UP    ? TT_MOVE_UP
                                                          : TT_MOVE_DOWN;
        tt_node *to = tt_layout_neighbour(layout, layout->focus, dir);
        if (to) layout->focus = to;
        break;
    }
    case TT_CHORD_FOCUS_PREV:
    case TT_CHORD_FOCUS_NEXT: {
        tt_node *to =
            tt_layout_cycle(layout, layout->focus, chord == TT_CHORD_FOCUS_NEXT ? 1 : -1);
        if (to) layout->focus = to;
        break;
    }
    case TT_CHORD_QUIT: *quit = true; break;
    case TT_CHORD_NEW_TAB: new_tab(g); break;
    case TT_CHORD_CLOSE_TAB:
        if (!close_active_tab(g)) *quit = true;
        break;
    case TT_CHORD_TAB_PREV:
        tt_tabs_cycle(&g->tabs, -1);
        select_tab(g, tt_tabs_active_index(&g->tabs));
        break;
    case TT_CHORD_TAB_NEXT:
        tt_tabs_cycle(&g->tabs, 1);
        select_tab(g, tt_tabs_active_index(&g->tabs));
        break;
    case TT_CHORD_TAB_1:
    case TT_CHORD_TAB_2:
    case TT_CHORD_TAB_3:
    case TT_CHORD_TAB_4:
    case TT_CHORD_TAB_5:
    case TT_CHORD_TAB_6:
    case TT_CHORD_TAB_7:
    case TT_CHORD_TAB_8:
    case TT_CHORD_TAB_9: select_tab(g, tt_chord_tab_index(chord)); break;
    case TT_CHORD_NONE: break;
    }
}

static pane *sync_candidate(gui_ctx *g, gui_tab **owner) {
    int total = 0;
    for (int ti = 0; ti < tt_tabs_count(&g->tabs); ti++) {
        gui_tab *tab = tt_tabs_at(&g->tabs, ti);
        total += tt_layout_count(&tab->layout);
    }
    if (total < 1) return NULL;
    int wanted = g->sync_cursor++ % total;
    for (int ti = 0; ti < tt_tabs_count(&g->tabs); ti++) {
        gui_tab *tab = tt_tabs_at(&g->tabs, ti);
        tt_node *leaves[MAX_PANES];
        int n = tt_layout_leaves(&tab->layout, leaves, MAX_PANES);
        if (n > MAX_PANES) n = MAX_PANES;
        if (wanted < n) {
            if (owner) *owner = tab;
            return node_pane(leaves[wanted]);
        }
        wanted -= n;
    }
    return NULL;
}

static void begin_snapshot(gui_ctx *g) {
    struct pollfd unused;
    if (tt_broker_client_pollfd(&g->broker, &unused)) return;
    gui_tab *tab = NULL;
    pane *p = sync_candidate(g, &tab);
    if (!p) return;
    if (tt_broker_client_snapshot_begin(&g->broker, p->session_id, 0) == 0) {
        g->syncing = p;
        g->syncing_tab = tab;
    }
}

static void finish_snapshot(gui_ctx *g, short revents) {
    const void *body = NULL;
    size_t len = 0;
    int rc = tt_broker_client_pump(&g->broker, revents, &body, &len);
    if (rc == 0) return;
    pane *p = g->syncing;
    gui_tab *tab = g->syncing_tab;
    g->syncing = NULL;
    g->syncing_tab = NULL;
    if (rc < 0 || !p || !tab) return;
    char old_title[256];
    snprintf(old_title, sizeof old_title, "%s", vt_title(p->term));
    uint64_t old_generation = p->generation;
    if (vt_snapshot_read(p->term, body, len) != 0) return;
    p->generation = vt_generation(p->term);
    if (p->generation != old_generation && tab != active_tab(g)) tab->activity = true;
    if (p->generation != old_generation || strcmp(old_title, vt_title(p->term)) != 0)
        g->tabs_dirty = true;
}

static const char *pane_label(const pane *p) {
    const char *title = vt_title(p->term);
    if (title && *title) return title;
    const char *cwd = vt_cwd(p->term);
    if (cwd && *cwd) {
        const char *slash = strrchr(cwd, '/');
        if (slash && slash[1]) return slash + 1;
        return cwd;
    }
    return "shell";
}

static void paint_tabs(gui_ctx *g) {
    tt_render_tab view[TT_TABS_MAX];
    int count = tt_tabs_count(&g->tabs);
    for (int i = 0; i < count; i++) {
        gui_tab *tab = tt_tabs_at(&g->tabs, i);
        pane *p = node_pane(tab->layout.focus);
        view[i].title = p ? pane_label(p) : "shell";
        view[i].active = i == tt_tabs_active_index(&g->tabs);
        view[i].activity = tab->activity;
    }
    tt_render_tab_bar(&g->fb, &g->base, g->titlebar_inset,
                      g->tab_h - g->titlebar_inset, view, count);
}

static void release_all_tabs(gui_ctx *g, bool kill) {
    while (tt_tabs_count(&g->tabs) > 0) {
        gui_tab *tab = tt_tabs_remove(&g->tabs, tt_tabs_count(&g->tabs) - 1);
        tab_release(g, tab, kill);
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

    gui_ctx g;
    memset(&g, 0, sizeof g);
    tt_tabs_init(&g.tabs);
    g.cmd = o.cmd;
    g.divider = o.cfg.divider;
    g.copy_on_select = o.cfg.copy_on_select;
    if (tt_workspace_path(g.workspace_path, sizeof g.workspace_path) != 0)
        g.workspace_path[0] = '\0';
    /* The broker must fork before AppKit/CoreText initialize. */
    if (tt_broker_client_open(&g.broker, NULL, err, sizeof err) != 0) {
        fprintf(stderr, "tnytty: gui: %s\n", err[0] ? err : strerror(errno));
        return 1;
    }
    if (o.listen) {
        fprintf(stderr,
                "tnytty: gui --listen is unavailable with durable sessions; "
                "the same API is available on the private broker socket\n");
        tt_broker_client_close(&g.broker);
        return 2;
    }

    tt_window *win = tt_window_open(&o.cfg, cols, rows, "tnytty", err, sizeof err);
    if (!win) {
        fprintf(stderr, "tnytty: %s\n", err);
        tt_broker_client_close(&g.broker);
        return 1;
    }
    g.win = win;
    tt_window_render_config(win, &g.base);
    /* window.h folds the titlebar height into pad_top and leaves the
     * other three at the configured padding, so the inset is the
     * difference; panes below a horizontal split give it back. */
    g.titlebar_inset = g.base.pad_top - g.base.pad_bottom;
    if (g.titlebar_inset < 0) g.titlebar_inset = 0;
    g.tab_h = g.titlebar_inset + g.base.cell_h;
    tt_status_clear(&g.status);

    int px_w = 0, px_h = 0;
    tt_window_surface(win, &px_w, &px_h);
    if (tt_fb_alloc(&g.fb, px_w, px_h) != 0) {
        fprintf(stderr, "tnytty: gui: out of memory\n");
        tt_window_close(win);
        tt_broker_client_close(&g.broker);
        return 1;
    }
    /* One pane's worth of grid, to size the first session before the
     * layout exists; relayout() gives it the exact numbers. */
    tt_render_grid_for(&g.base, px_w, px_h, &cols, &rows);

    bool restored = false;
    if (!o.cmd && g.workspace_path[0]) {
        tt_workspace saved;
        int loaded = tt_workspace_load(g.workspace_path, &saved, err, sizeof err);
        if (loaded == 0 && saved.tab_count > 0) restored = restore_workspace(&g, &saved) > 0;
        if (loaded < 0) fprintf(stderr, "tnytty: %s; starting a fresh workspace\n", err);
    }
    if (!restored) {
        release_all_tabs(&g, false);
        pane *root = pane_new(&g, cols, rows, NULL);
        if (!root || !tab_new(&g, root)) {
            if (root) {
                pane_kill(&g, root);
                pane_free(root, NULL);
            }
            fprintf(stderr, "tnytty: spawn failed: %s\n", strerror(errno));
            tt_fb_free(&g.fb);
            tt_window_close(win);
            tt_broker_client_close(&g.broker);
            return 1;
        }
        save_workspace(&g);
    }
    relayout(&g);
    install_signals();

    bool focused = true;
    bool quit = false;
    int exit_code = 0;
    char last_title[128] = {0};
    struct pollfd fds[2];
    tt_node *leaves[MAX_PANES];

    while (!quit) {
        tt_win_ev ev;
        while (tt_window_pump(win, &ev)) {
            switch (ev.type) {
            case TT_WIN_EV_KEY: {
                tt_layout *layout = active_layout(&g);
                pane *p = layout ? node_pane(layout->focus) : NULL;
                char out[64];
                if (!p) break;
                size_t n = tt_key_encode(ev.key, ev.text, ev.text_len, ev.mods,
                                         vt_app_cursor(p->term), out, sizeof out);
                if (n) (void)tt_broker_client_input(&g.broker, p->session_id, out, n);
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
                    tt_layout *layout = active_layout(&g);
                    pane *p = layout ? node_pane(layout->focus) : NULL;
                    finish_selection(&g, p);
                }
                break;
            case TT_WIN_EV_CLOSE: quit = true; break;
            case TT_WIN_EV_NONE: break;
            }
            if (quit) break;
        }
        if (quit) break;

        begin_snapshot(&g);
        int n = 0;
        int sig_i = -1, broker_i = -1;
        if (sig_pipe[0] >= 0) {
            sig_i = n;
            fds[n].fd = sig_pipe[0];
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }
        if (tt_broker_client_pollfd(&g.broker, &fds[n])) {
            broker_i = n;
            n++;
        }
        if (poll(fds, (nfds_t)n, PUMP_TIMEOUT) < 0 && errno != EINTR) break;

        if (sig_i >= 0 && (fds[sig_i].revents & POLLIN)) {
            unsigned char sig;
            while (read(sig_pipe[0], &sig, 1) == 1)
                if (sig == SIGINT || sig == SIGTERM) quit = true;
        }
        if (broker_i >= 0) finish_snapshot(&g, fds[broker_i].revents);
        if (quit) break;

        tt_layout *layout = active_layout(&g);
        pane *fp = layout ? node_pane(layout->focus) : NULL;
        const char *title = fp ? vt_title(fp->term) : NULL;
        if (title && *title && strcmp(title, last_title) != 0) {
            snprintf(last_title, sizeof last_title, "%s", title);
            tt_window_set_title(win, title);
        }

        tt_status_tick(&g.status, now_sec());

        /* One frame: the divider ground and the status bar are the
         * window's, every cell is a pane's. */
        int y0 = g.fb.h, y1 = 0, painted = 0;
        if (g.relayout) {
            layout = active_layout(&g);
            if (layout)
                tt_fb_fill(&g.fb, layout->area.x, layout->area.y, layout->area.w, layout->area.h,
                           g.divider);
            g.status_shown[0] = '\1'; /* force the bar to repaint too */
            g.tabs_dirty = true;
            y0 = 0;
            y1 = g.fb.h;
            painted++;
        }
        int npanes = layout ? tt_layout_leaves(layout, leaves, MAX_PANES) : 0;
        if (npanes > MAX_PANES) npanes = MAX_PANES;
        for (int i = 0; i < npanes; i++) {
            pane *p = node_pane(leaves[i]);
            if (!p || !p->r) continue;
            /* A selection belongs to the text it was made from: when that
             * text scrolls or is overwritten, drop the highlight rather
             * than leave it pointing at something never selected. */
            if (tt_sel_active(&p->sel) && !p->sel.dragging && p->sel_snapshot) {
                char *now = sel_dup(&p->sel, p->term);
                if (!now || strcmp(now, p->sel_snapshot) != 0) {
                    tt_sel_clear(&p->sel);
                    tt_render_set_selection(p->r, &p->sel);
                    free(p->sel_snapshot);
                    p->sel_snapshot = NULL;
                }
                free(now);
            }
            int a = 0, b = 0;
            int lines = tt_render_frame(p->r, p->term, focused && layout && leaves[i] == layout->focus,
                                        &a, &b);
            if (lines <= 0) continue;
            painted += lines;
            if (a < y0) y0 = a;
            if (b > y1) y1 = b;
        }
        if (g.tabs_dirty) {
            paint_tabs(&g);
            painted++;
            if (g.titlebar_inset < y0) y0 = g.titlebar_inset;
            if (g.tab_h > y1) y1 = g.tab_h;
            g.tabs_dirty = false;
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

    save_workspace(&g);
    release_all_tabs(&g, false);
    tt_fb_free(&g.fb);
    tt_window_close(win);
    tt_broker_client_close(&g.broker);
    return exit_code;
}
