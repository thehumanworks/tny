/* tnytty — the tiny terminal. CLI adapter over the session registry:
 * `run` (raw passthrough), `serve` (headless HTTP API), `icat`. */
#include "api/http.h"
#include "icat/icat.h"
#include "session/session.h"
#include "ui/gui.h"
#include "util/tt.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#ifndef TNYTTY_VERSION
#define TNYTTY_VERSION "0.0.0-dev"
#endif

#define SCROLLBACK   2000
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 7681
#define MAX_LOOP_FDS 128

static const char root_help[] =
    "tnytty — the tiny terminal\n"
    "\n"
    "usage: tnytty [run] [flags] [-- CMD ARGS...]   passthrough terminal (default)\n"
    "       tnytty gui [flags] [-- CMD ARGS...]     native window (macOS)\n"
    "       tnytty serve [flags]                    headless HTTP API server\n"
    "       tnytty icat FILE|-                      inline image (kitty graphics)\n"
    "       tnytty --help | --version\n"
    "\n"
    "flags (run/serve):\n"
    "  --listen HOST:PORT   serve the HTTP API (default off; serve: 127.0.0.1:7681)\n"
    "  --token TOKEN        API bearer token (env TNYTTY_TOKEN); required for\n"
    "                       non-loopback binds\n"
    "  --cols N --rows N    initial size (run only; default: the attached tty)\n"
    "\n"
    "examples:\n"
    "  tnytty                                  # $SHELL in a scriptable session\n"
    "  tnytty run --listen 127.0.0.1:7681 -- htop\n"
    "  curl -s 127.0.0.1:7681/v1/sessions      # then .../ID/screen, .../ID/input\n"
    "  tnytty gui --titlebar opaque            # native window, system titlebar\n"
    "  tnytty icat photo.png\n"
    "\n"
    "docs: tnytty/docs/ in the tny monorepo\n";

/* ---- shared flag state ----------------------------------------------- */

typedef struct {
    const char *listen; /* HOST:PORT or NULL */
    const char *token;
    int cols, rows;
    char *const *cmd; /* NULL = $SHELL */
} opts;

static int parse_listen(const char *spec, char *host, size_t hostlen, int *port) {
    const char *colon = strrchr(spec, ':');
    if (!colon || colon == spec) return -1;
    size_t hl = (size_t)(colon - spec);
    if (hl >= hostlen) return -1;
    memcpy(host, spec, hl);
    host[hl] = '\0';
    char *end = NULL;
    long p = strtol(colon + 1, &end, 10);
    if (!end || *end || p < 1 || p > 65535) return -1;
    *port = (int)p;
    return 0;
}

static int parse_opts(int argc, char **argv, opts *o) {
    o->listen = NULL;
    o->token = getenv("TNYTTY_TOKEN");
    o->cols = 0;
    o->rows = 0;
    o->cmd = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            o->cmd = i + 1 < argc ? &argv[i + 1] : NULL;
            return 0;
        }
        if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) o->listen = argv[++i];
        else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) o->token = argv[++i];
        else if (strcmp(argv[i], "--cols") == 0 && i + 1 < argc) o->cols = atoi(argv[++i]);
        else if (strcmp(argv[i], "--rows") == 0 && i + 1 < argc) o->rows = atoi(argv[++i]);
        else {
            fprintf(stderr, "tnytty: unknown flag %s (see --help)\n", argv[i]);
            return -1;
        }
    }
    return 0;
}

static tt_http *maybe_listen(tt_api *api, const opts *o, char *token_buf, size_t token_cap,
                             const char *dflt_spec) {
    const char *spec = o->listen ? o->listen : dflt_spec;
    if (!spec) return NULL;
    char host[128];
    int port = 0;
    if (parse_listen(spec, host, sizeof host, &port) != 0) {
        fprintf(stderr, "tnytty: bad --listen %s (want HOST:PORT)\n", spec);
        exit(2);
    }
    bool loopback = strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0 ||
                    strcmp(host, "localhost") == 0;
    if (!loopback && (!api->token || !api->token[0])) {
        tt_rand_hex(token_buf, token_cap - 1);
        api->token = token_buf;
        fprintf(stderr, "tnytty: generated API token (docs/adr/0002): %s\n", token_buf);
    }
    char err[256];
    tt_http *h = tt_http_listen(api, host, port, err, sizeof err);
    if (!h) {
        fprintf(stderr, "tnytty: %s\n", err);
        exit(2);
    }
    fprintf(stderr, "tnytty: HTTP API on http://%s:%d (auth: %s)\r\n", host, port,
            api->token && api->token[0] ? "bearer token" : "none, loopback");
    return h;
}

/* ---- signal plumbing (self-pipe) ------------------------------------- */

static int sig_pipe[2] = {-1, -1};

static void on_signal(int signo) {
    unsigned char b = (unsigned char)signo;
    ssize_t r = write(sig_pipe[1], &b, 1);
    (void)r;
}

static void install_signals(bool winch) {
    if (pipe(sig_pipe) != 0) return;
    fcntl(sig_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(sig_pipe[1], F_SETFL, O_NONBLOCK);
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    if (winch) sigaction(SIGWINCH, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* ---- run: passthrough terminal --------------------------------------- */

static struct termios saved_tio;
static bool tio_saved = false;

static void restore_tty(void) {
    if (tio_saved) tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tio);
}

static void stdout_sink(void *user, const char *bytes, size_t len) {
    (void)user;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(STDOUT_FILENO, bytes + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        off += (size_t)w;
    }
}

static int cmd_run(int argc, char **argv) {
    opts o;
    if (parse_opts(argc, argv, &o) != 0) return 2;

    int cols = o.cols, rows = o.rows;
    struct winsize ws;
    if ((cols < 1 || rows < 1) && isatty(STDIN_FILENO) &&
        ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (cols < 1) cols = ws.ws_col;
        if (rows < 1) rows = ws.ws_row;
    }
    if (cols < 1) cols = 80;
    if (rows < 1) rows = 24;

    tt_registry reg;
    tt_registry_init(&reg, SCROLLBACK);
    tt_session *s = tt_session_create(&reg, o.cmd, cols, rows);
    if (!s) {
        fprintf(stderr, "tnytty: spawn failed: %s\n", strerror(errno));
        return 1;
    }
    s->attached = true;
    char token_buf[33];
    tt_api api = {&reg, o.token, TNYTTY_VERSION};
    tt_http *http = maybe_listen(&api, &o, token_buf, sizeof token_buf, NULL);

    bool interactive = isatty(STDIN_FILENO);
    if (interactive) {
        if (tcgetattr(STDIN_FILENO, &saved_tio) == 0) {
            tio_saved = true;
            atexit(restore_tty);
            struct termios raw = saved_tio;
            cfmakeraw(&raw);
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        }
    }
    install_signals(interactive);

    char scratch[16384];
    struct pollfd fds[8 + MAX_LOOP_FDS];
    int exit_code = 0;
    bool watch_stdin = true;
    for (;;) {
        int n = 0;
        int stdin_i = -1, sig_i = -1, pty_i = -1, headless_i = -1, headless_n = 0, http_i = -1;
        tt_session *headless[MAX_LOOP_FDS / 2] = {0};
        /* Back-pressure: stop reading the tty while the pty is far
         * behind, so the queue never reaches its cap (session.h). */
        if (watch_stdin && tt_session_pending(s) < TT_INPUT_HIGH_WATER) {
            stdin_i = n;
            fds[n].fd = STDIN_FILENO;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }
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
            headless_i = n;
            headless_n = tt_registry_poll_fill(&reg, fds + n, headless, MAX_LOOP_FDS / 2, false);
            n += headless_n;
        }
        if (http) {
            http_i = n;
            n += tt_http_fill(http, fds + n, (int)(sizeof fds / sizeof *fds) - n);
        }
        if (poll(fds, (nfds_t)n, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sig_i >= 0 && (fds[sig_i].revents & POLLIN)) {
            unsigned char sig;
            while (read(sig_pipe[0], &sig, 1) == 1) {
                if (sig == SIGWINCH && interactive && ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
                    tt_session_resize(s, ws.ws_col, ws.ws_row);
                } else if (sig == SIGINT || sig == SIGTERM) {
                    tt_session_destroy(&reg, s);
                    goto done;
                }
            }
        }
        if (stdin_i >= 0 && (fds[stdin_i].revents & (POLLIN | POLLHUP))) {
            ssize_t r = read(STDIN_FILENO, scratch, sizeof scratch);
            if (r > 0) tt_session_write(s, scratch, (size_t)r);
            else if (r == 0) watch_stdin = false; /* EOF: run on until the child exits */
        }
        if (pty_i >= 0 && (fds[pty_i].revents & POLLOUT)) tt_session_flush(s);
        if (pty_i >= 0 && (fds[pty_i].revents & (POLLIN | POLLHUP))) {
            tt_session_drain(s, scratch, sizeof scratch, TT_PUMP_READS_PER_TURN, stdout_sink, NULL);
            if (!s->alive) {
                exit_code = s->exit_code;
                if (!http) break;
                fprintf(stderr,
                        "\r\ntnytty: child exited (%d); API still serving — Ctrl-C to quit\r\n",
                        exit_code);
            }
        }
        if (headless_i >= 0)
            tt_registry_poll_handle(fds + headless_i, headless, headless_n, scratch,
                                    sizeof scratch);
        if (http && http_i >= 0) tt_http_handle(http, fds + http_i, n - http_i);
        if (!s->alive && !http) break;
    }
done:
    restore_tty();
    tt_http_free(http);
    tt_registry_free(&reg);
    return exit_code;
}

/* ---- serve: headless API --------------------------------------------- */

static int cmd_serve(int argc, char **argv) {
    opts o;
    if (parse_opts(argc, argv, &o) != 0) return 2;
    if (o.cmd) {
        fprintf(stderr, "tnytty serve: no command argument; create sessions via the API\n");
        return 2;
    }
    tt_registry reg;
    tt_registry_init(&reg, SCROLLBACK);
    char token_buf[33];
    tt_api api = {&reg, o.token, TNYTTY_VERSION};
    char dflt[32];
    snprintf(dflt, sizeof dflt, "%s:%d", DEFAULT_HOST, DEFAULT_PORT);
    tt_http *http = maybe_listen(&api, &o, token_buf, sizeof token_buf, dflt);
    install_signals(false);

    char scratch[16384];
    struct pollfd fds[MAX_LOOP_FDS];
    for (;;) {
        int n = 0;
        if (sig_pipe[0] >= 0) {
            fds[n].fd = sig_pipe[0];
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }
        int sess_start = n;
        tt_session *sessions[MAX_LOOP_FDS / 2] = {0};
        int nsessions = tt_registry_poll_fill(&reg, fds + n, sessions, MAX_LOOP_FDS / 2, false);
        n += nsessions;
        int http_i = n;
        n += tt_http_fill(http, fds + n, MAX_LOOP_FDS - n);
        if (poll(fds, (nfds_t)n, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sig_pipe[0] >= 0 && (fds[0].revents & POLLIN)) {
            unsigned char sig;
            bool quit = false;
            while (read(sig_pipe[0], &sig, 1) == 1)
                if (sig == SIGINT || sig == SIGTERM) quit = true;
            if (quit) break;
        }
        tt_registry_poll_handle(fds + sess_start, sessions, nsessions, scratch, sizeof scratch);
        tt_http_handle(http, fds + http_i, n - http_i);
    }
    tt_http_free(http);
    tt_registry_free(&reg);
    return 0;
}

/* ---- entry ------------------------------------------------------------ */

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        fputs(root_help, stdout);
        return 0;
    }
    if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("tnytty %s\n", TNYTTY_VERSION);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "icat") == 0) {
        if (argc != 3 || strcmp(argv[2], "--help") == 0) {
            fputs("usage: tnytty icat FILE|-   (PNG; kitty graphics protocol)\n",
                  argc == 3 ? stdout : stderr);
            return argc == 3 ? 0 : 2;
        }
        return tt_icat_main(argv[2]);
    }
    if (argc > 1 && strcmp(argv[1], "gui") == 0) return tt_gui_main(argc - 2, argv + 2);
    if (argc > 1 && strcmp(argv[1], "serve") == 0) return cmd_serve(argc - 2, argv + 2);
    if (argc > 1 && strcmp(argv[1], "run") == 0) return cmd_run(argc - 2, argv + 2);
    return cmd_run(argc - 1, argv + 1);
}
