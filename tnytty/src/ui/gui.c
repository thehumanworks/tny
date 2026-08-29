/* gui.c — `tnytty gui`: a session in a native window (docs/adr/0005).
 *
 * One loop, as everywhere else in tnytty: poll(2) owns the pty master,
 * the signal self-pipe and the HTTP fds, and each iteration also drains
 * the window's queued platform events. The poll timeout bounds how long
 * a keystroke can sit in AppKit's queue; nothing else runs a loop and no
 * thread but this one touches the VT core. */
#include "ui/gui.h"

#include "api/http.h"
#include "session/session.h"
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

static void copy_selection(tt_window *win, const tt_selection *sel, const vt *t,
                           tt_status *status) {
    char *text = sel_dup(sel, t);
    if (text && *text) {
        tt_window_set_clipboard(win, text, strlen(text));
        tt_status_copied(status, text, now_sec());
    }
    free(text);
}

/* The VT core answers DSR/DA through a callback; in the window there is
 * no outer terminal to forward to, so the answer goes into the pty. */
static int reply_to_pty(void *user, const char *bytes, size_t len) {
    return tt_session_write((tt_session *)user, bytes, len);
}

/* Paste as the child asked for it: bracketed when it enabled mode 2004,
 * raw bytes otherwise. Newlines stay newlines; a paste is not a keypress. */
static void paste_clipboard(tt_window *win, tt_session *s) {
    const char *text = tt_window_clipboard(win);
    if (!text || !*text) return;
    bool bracketed = vt_bracketed_paste(s->term);
    if (bracketed) tt_session_write(s, "\x1b[200~", 6);
    tt_session_write(s, text, strlen(text));
    if (bracketed) tt_session_write(s, "\x1b[201~", 6);
}

/* Grow the session to whatever grid the window's pixels now hold. */
static void sync_size(tt_window *win, tt_render *rend, tt_session *s) {
    int px_w = 0, px_h = 0;
    tt_window_surface(win, &px_w, &px_h);
    if (tt_render_resize(rend, px_w, px_h) != 0) return;
    int cols = tt_render_cols(rend), rows = tt_render_rows(rend);
    if (cols != vt_cols(s->term) || rows != vt_rows(s->term)) tt_session_resize(s, cols, rows);
}

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

    tt_render_config rcfg;
    tt_window_render_config(win, &rcfg);
    int px_w = 0, px_h = 0;
    tt_window_surface(win, &px_w, &px_h);
    tt_render *rend = tt_render_new(&rcfg, px_w, px_h);
    if (!rend) {
        fprintf(stderr, "tnytty: gui: out of memory\n");
        tt_window_close(win);
        return 1;
    }
    cols = tt_render_cols(rend);
    rows = tt_render_rows(rend);

    tt_registry reg;
    tt_registry_init(&reg, SCROLLBACK);
    tt_session *s = tt_session_create(&reg, o.cmd, cols, rows);
    if (!s) {
        fprintf(stderr, "tnytty: spawn failed: %s\n", strerror(errno));
        tt_render_free(rend);
        tt_window_close(win);
        return 1;
    }

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

    tt_reply reply;
    tt_reply_attach(s->term, &reply, reply_to_pty, s);
    tt_status status;
    tt_status_clear(&status);
    tt_render_set_status(rend, "");
    tt_selection sel;
    tt_sel_clear(&sel);
    char *sel_snapshot = NULL;
    bool focused = true;
    bool quit = false;
    int exit_code = 0;
    char scratch[16384];
    char last_title[128] = {0};
    struct pollfd fds[8 + MAX_LOOP_FDS];

    while (!quit) {
        tt_win_ev ev;
        while (tt_window_pump(win, &ev)) {
            switch (ev.type) {
            case TT_WIN_EV_KEY: {
                char out[64];
                size_t n = tt_key_encode(ev.key, ev.text, ev.text_len, ev.mods,
                                         vt_app_cursor(s->term), out, sizeof out);
                if (n) tt_session_write(s, out, n);
                break;
            }
            case TT_WIN_EV_MOUSE_DOWN:
            case TT_WIN_EV_MOUSE_DRAG:
            case TT_WIN_EV_MOUSE_UP: {
                int col = 0, row = 0;
                tt_render_cell_at(&rcfg, ev.px_x, ev.px_y, tt_render_cols(rend),
                                  tt_render_rows(rend), &col, &row);
                if (ev.type == TT_WIN_EV_MOUSE_DOWN)
                    tt_sel_begin(&sel, s->term, col, row, ev.clicks);
                else if (ev.type == TT_WIN_EV_MOUSE_DRAG) tt_sel_extend(&sel, s->term, col, row);
                else if (tt_sel_finish(&sel, s->term) && o.cfg.copy_on_select)
                    copy_selection(win, &sel, s->term, &status);
                if (ev.type == TT_WIN_EV_MOUSE_UP) {
                    free(sel_snapshot);
                    sel_snapshot = sel_dup(&sel, s->term);
                }
                tt_render_set_selection(rend, &sel);
                break;
            }
            case TT_WIN_EV_COPY: copy_selection(win, &sel, s->term, &status); break;
            case TT_WIN_EV_PASTE: paste_clipboard(win, s); break;
            case TT_WIN_EV_RESIZE:
                tt_sel_clear(&sel);
                tt_render_set_selection(rend, &sel);
                sync_size(win, rend, s);
                break;
            case TT_WIN_EV_FOCUS: focused = ev.focused; break;
            case TT_WIN_EV_CLOSE: quit = true; break;
            case TT_WIN_EV_NONE: break;
            }
        }
        if (quit) break;

        int n = 0;
        int sig_i = -1, pty_i = -1, http_i = -1;
        if (sig_pipe[0] >= 0) {
            sig_i = n;
            fds[n].fd = sig_pipe[0];
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }
        if (s->alive) {
            pty_i = n;
            fds[n].fd = s->pty.master;
            fds[n].events = POLLIN | (tt_session_pending(s) ? POLLOUT : 0);
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
        if (pty_i >= 0 && (fds[pty_i].revents & POLLOUT)) tt_session_flush(s);
        if (pty_i >= 0 && (fds[pty_i].revents & (POLLIN | POLLHUP))) {
            while (tt_session_pump(s, scratch, sizeof scratch, NULL, NULL) > 0) {}
            if (!s->alive) {
                exit_code = s->exit_code;
                quit = true;
            }
        }
        if (http && http_i >= 0) tt_http_handle(http, fds + http_i, n - http_i);

        const char *title = vt_title(s->term);
        if (title && *title && strcmp(title, last_title) != 0) {
            snprintf(last_title, sizeof last_title, "%s", title);
            tt_window_set_title(win, title);
        }
        /* A selection belongs to the text it was made from: when that
         * text scrolls or is overwritten, drop the highlight rather than
         * leave it pointing at something the user never selected. */
        if (tt_sel_active(&sel) && !sel.dragging && sel_snapshot) {
            char *now = sel_dup(&sel, s->term);
            if (!now || strcmp(now, sel_snapshot) != 0) {
                tt_sel_clear(&sel);
                tt_render_set_selection(rend, &sel);
                free(sel_snapshot);
                sel_snapshot = NULL;
            }
            free(now);
        }

        /* The status message is transient; the renderer repaints the bar
         * only when the text actually changes, so setting it every turn
         * costs a strcmp. */
        tt_status_tick(&status, now_sec());
        tt_render_set_status(rend, tt_status_text(&status));

        int y0 = 0, y1 = 0;
        if (tt_render_frame(rend, s->term, focused, &y0, &y1) > 0)
            tt_window_present(win, tt_render_pixels(rend), tt_render_width(rend),
                              tt_render_height(rend), y0, y1);
    }
    free(sel_snapshot);

    tt_http_free(http);
    tt_render_free(rend);
    tt_window_close(win);
    tt_registry_free(&reg);
    return exit_code;
}
