/* tui.c — the interactive shell: raw-mode terminal, one poll loop over stdin
 * plus the backend fds, and lazy backend creation (nothing is spawned or
 * connected until the first prompt is submitted). See docs/tui.md. */
#include "tui/tui.h"
#include "mcp/mcp.h"
#include "util/tny_poll.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* 1 when the browser bootstrap owns stdio (site/assets/term-wasm.js). */
EM_JS(int, js_tui_page, (void), { return Module.tnyOut ? 1 : 0; });
#endif

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* ---- terminal ---- */

static struct termios g_saved;
static bool g_raw, g_restore_sgr;
static volatile sig_atomic_t g_winch, g_sigint;

static void term_restore(void) {
    if (!g_raw) return;
    g_raw = false;
    fputs(g_restore_sgr ? "\x1b[?2004l\x1b[0m\x1b[?25h"
                        : "\x1b[?2004l\x1b[?25h", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
}

static void on_winch(int s) { (void)s; g_winch = 1; }
static void on_sigint(int s) { (void)s; g_sigint = 1; }
static void on_fatal(int s) { term_restore(); _exit(128 + s); }

static bool tui_cancel_probe(void *ud) {
    tui *t = ud;
    if (!g_sigint && !t->want_cancel) return false;
    g_sigint = 0;
    t->want_cancel = false;
    return true;
}

static void install(int sig, void (*fn)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: poll must return EINTR */
    sigaction(sig, &sa, NULL);
}

static bool term_raw(bool restore_sgr) {
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) return false;
    struct termios r = g_saved;
    /* keep OPOST/ONLCR so plain printf from reused CLI commands still works */
    r.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    r.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    r.c_cc[VMIN] = 1;
    r.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &r) != 0) return false;
    g_raw = true;
    g_restore_sgr = restore_sgr;
    /* bracketed paste: pasted newlines land in the composer, not as Enter */
    fputs("\x1b[?2004h", stdout);
    fflush(stdout);
    atexit(term_restore);
    return true;
}

/* ---- helpers ---- */

/* Tool detail is untrusted: flatten every control byte (ESC included) so no
 * escape sequence reaches the transcript, whole or clipped in half. */
static void oneline(char *dst, size_t cap, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[j++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }
    dst[j] = 0;
}

void tui_drop_backend(tui *t) {
    t->bk_adopted = false;
    if (!t->engine) return;
    tny_engine_preserve_session_on_free(t->engine);
    tny_engine_free(t->engine);
    t->engine = NULL;
}

int tui_queue_image(tui *t, const char *path) {
    if (!t || !path || !*path || t->n_images >= TUI_MAX_IMAGES) return 0;
    char *use = NULL;
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        char *home = path_home();
        use = path[1] == '/' ? path_join(home, path + 2) : xstrdup(home);
        free(home);
    }
    const char *src = use ? use : path;
    if (!file_exists(src)) { free(use); return 0; }
    char *abs = path_abs(src);
    free(use);
    t->images[t->n_images++] = abs ? abs : xstrdup(src);
    t->images[t->n_images] = NULL;
    return t->n_images;
}

static void maybe_gap(tui *t, bool for_text) {
    if (t->gap == 0) return;
    if (!for_text && t->gap == 2) return;
    tui_bol(t);
    tui_write(t, "\n", 1);
    t->gap = 0;
}

/* ---- approvals ---- */

tny_perm_decision tui_ask_perm(tui *t, const char *tool, const char *summary) {
    if (t->ctx->perm_mode == TNY_MODE_YOLO) return TNY_PERM_DECISION_ALLOW;

    char line[512];
    oneline(line, sizeof line, summary && *summary ? summary : tool);
    tui_linef(t, "%s%s? %s%s", tui_attr(t, "\x1b[1m"), tui_c(t, "\x1b[33m"),
              line, tui_attr(t, "\x1b[0m"));
    if (!t->tty) {
        tui_sys(t, "  not a terminal: denied");
        return TNY_PERM_DECISION_DENY;
    }

    tui_pick_close(t);
    t->approval = true;
    t->dirty = true;
    tny_perm_decision d = TNY_PERM_DECISION_DENY;
    bool got = false;
    while (!got && !t->quit) {
        tui_render(t);
        struct pollfd pf = {STDIN_FILENO, POLLIN, 0};
        int pr = tny_poll(&pf, 1, 200);
        if (g_winch) { g_winch = 0; tui_size(t); t->dirty = true; }
        if (g_sigint) { g_sigint = 0; t->want_cancel = true; break; }
        if (pr <= 0) continue;
        char b[64];
        ssize_t n = read(STDIN_FILENO, b, sizeof b);
        if (n <= 0) { if (n == 0) t->quit = true; break; }
        for (ssize_t i = 0; i < n && !got; i++) {
            switch (b[i]) {
            case 'y': case 'Y': d = TNY_PERM_DECISION_ALLOW; got = true; break;
            case 'a': case 'A': d = TNY_PERM_DECISION_ALLOW_ALWAYS; got = true; break;
            case 'n': case 'N': case 27: d = TNY_PERM_DECISION_DENY; got = true; break;
            case 3: d = TNY_PERM_DECISION_DENY; t->want_cancel = true; got = true; break;
            default: break;
            }
        }
    }
    t->approval = false;
    tui_sys(t, d == TNY_PERM_DECISION_ALLOW          ? "  allowed once"
              : d == TNY_PERM_DECISION_ALLOW_ALWAYS  ? "  allowed for this session"
                                                     : "  denied");
    t->dirty = true;
    return d;
}

static tny_perm_decision perm_hook(const char *tool, const char *summary, void *ud) {
    return tui_ask_perm((tui *)ud, tool, summary);
}

/* ---- normalized event rendering ---- */

static void ev_cb(const tny_backend_event *ev, void *ud) {
    tui *t = ud;
    char line[512];

    /* reasoning streams on its own lines: break before anything else lands */
    if (ev->kind != TNY_EV_THINKING && t->in_thinking) {
        t->in_thinking = false;
        tui_bol(t);
    }

    switch (ev->kind) {
    case TNY_EV_TEXT_DELTA:
        maybe_gap(t, true);
        tui_write(t, ev->text, ev->text_len);
        buf_append(&t->last_reply, ev->text, ev->text_len);
        break;
    case TNY_EV_THINKING:
        maybe_gap(t, true);
        if (!t->in_thinking) {
            t->in_thinking = true;
            tui_bol(t);
            if (t->attr) tui_write(t, "\x1b[2m", 4);
            tui_write(t, "· ", 3); /* "·" is 2 bytes of UTF-8 plus the space */
            if (t->attr) tui_write(t, "\x1b[0m", 4);
        }
        tui_write_dim(t, ev->text, ev->text_len);
        break;
    case TNY_EV_TOOL_START:
        maybe_gap(t, false);
        oneline(line, sizeof line, ev->tool_detail);
        tui_linef(t, "%s⏺ %s%s %.100s", tui_c(t, "\x1b[36m"), ev->tool_name,
                  tui_attr(t, "\x1b[0m"), line);
        break;
    case TNY_EV_TOOL_END:
        oneline(line, sizeof line, ev->tool_detail);
        tui_linef(t, "  %s%s%s %s %.80s", tui_c(t, ev->tool_ok ? "\x1b[32m" : "\x1b[31m"),
                  ev->tool_ok ? "✓" : "✗", tui_attr(t, "\x1b[0m"), ev->tool_name, line);
        t->gap = 2; /* next model text starts a new iteration */
        break;
    case TNY_EV_TOOL_PROGRESS:
        oneline(line, sizeof line, ev->tool_detail);
        tui_note(t, "%s: %.360s",
                 ev->tool_name ? ev->tool_name : "tool", line);
        break;
    case TNY_EV_PERMISSION: {
        tny_perm_decision d;
        if (t->ctx->perm_mode == TNY_MODE_YOLO) {
            d = TNY_PERM_DECISION_ALLOW; /* silent: yolo is the normal mode */
        } else {
            d = tui_ask_perm(t, "approval requested", ev->perm_summary);
        }
        if (d == TNY_PERM_DECISION_ALLOW_ALWAYS &&
            !(ev->perm_options & TNY_PERM_ALLOW_ALWAYS))
            d = TNY_PERM_DECISION_ALLOW;
        tny_engine_respond_permission(t->engine, ev->perm_id, d);
        break;
    }
    case TNY_EV_PLAN:
        tui_bol(t);
        tui_linef(t, "%s── plan ──%s", tui_attr(t, "\x1b[2m"), tui_attr(t, "\x1b[0m"));
        tui_write(t, ev->text, ev->text_len);
        tui_bol(t);
        tui_linef(t, "%s──────────%s", tui_attr(t, "\x1b[2m"), tui_attr(t, "\x1b[0m"));
        break;
    case TNY_EV_USAGE:
        t->in_tok += ev->in_tokens;
        t->out_tok += ev->out_tokens;
        t->dirty = true;
        break;
    case TNY_EV_STATUS:
        snprintf(line, sizeof line, "%.*s", (int)ev->text_len, ev->text);
        tui_note(t, "%s", line);
        if (t->trace) tui_sys(t, line);
        break;
    case TNY_EV_CUSTOM_MESSAGE:
        tui_bol(t);
        tui_linef(t, "%s◆ %s%s %.*s", tui_attr(t, "\x1b[2m"),
                  ev->message_type ? ev->message_type : "extension",
                  tui_attr(t, "\x1b[0m"), (int)ev->text_len, ev->text);
        t->gap = 1;
        break;
    case TNY_EV_USER_MESSAGE:
        tui_bol(t);
        tui_linef(t, "%s› %.*s%s %sextension%s",
                  tui_attr(t, "\x1b[1m"), (int)ev->text_len, ev->text,
                  tui_attr(t, "\x1b[0m"), tui_attr(t, "\x1b[2m"),
                  tui_attr(t, "\x1b[0m"));
        t->gap = 1;
        break;
    case TNY_EV_ERROR:
        snprintf(line, sizeof line, "%.*s", (int)ev->text_len, ev->text);
        tui_err(t, line);
        break;
    case TNY_EV_STEER_REJECTED:
        /* the host would not take it mid-turn: send it right after instead.
         * The event carries the rejected text (docs/adr/0013), so nothing
         * here depends on which steer — or how many — went out. */
        if (ev->text && ev->text_len) {
            char *s = xstrndup(ev->text, ev->text_len);
            tui_queue_push(t, s, true);
            free(s);
        }
        break;
    case TNY_EV_TURN_END:
        t->turn_active = false;
        t->turn_done = true;
        t->stop = ev->stop;
        break;
    }
}

static void drain_engine_events(tui *t) {
    if (!t->engine) return;
    tny_owned_event *owned;
    while ((owned = tny_engine_pop_event(t->engine))) {
        ev_cb(&owned->ev, t);
        tny_owned_event_free(owned);
    }
}

/* ---- mid-turn input queue (docs/adr/0011) ---- */

void tui_queue_push(tui *t, const char *text, bool front) {
    char **q = realloc(t->queue, sizeof(char *) * (size_t)(t->n_queue + 1));
    if (!q) { tui_sys(t, "message dropped: out of memory"); return; }
    t->queue = q;
    if (front) {
        memmove(q + 1, q, sizeof(char *) * (size_t)t->n_queue);
        q[0] = xstrdup(text);
    } else q[t->n_queue] = xstrdup(text);
    t->n_queue++;
    t->dirty = true;
}

void tui_queue_clear(tui *t) {
    for (int i = 0; i < t->n_queue; i++) free(t->queue[i]);
    free(t->queue);
    t->queue = NULL;
    t->n_queue = 0;
    t->dirty = true;
}

/* Pop the oldest queued message; malloc'd, NULL when empty. */
static char *queue_pop(tui *t) {
    if (!t->n_queue) return NULL;
    char *s = t->queue[0];
    memmove(t->queue, t->queue + 1, sizeof(char *) * (size_t)(t->n_queue - 1));
    t->n_queue--;
    t->dirty = true;
    return s;
}

/* ---- session / backend ---- */

void tui_new_session(tui *t, bool clear_screen) {
    if (t->turn_active) { tui_sys(t, "finish the turn first"); return; }
    char *previous = t->session && t->session->id
        ? xstrdup(t->session->id) : NULL;
    if (t->engine)
        tny_engine_end_session(t->engine, clear_screen ? "clear" : "new");
    tui_drop_backend(t);
    if (t->session) {
        session_save(t->session);
        session_close(t->session);
        t->session = NULL;
    }
    t->session = session_new(t->ctx);
    if (t->session)
        session_set_extension_start(t->session,
                                    clear_screen ? "clear" : "new", previous);
    free(previous);
    tui_prewarm_start(t); /* the next first prompt should not pay startup */
    t->in_tok = t->out_tok = 0;
    buf_clear(&t->last_reply);
    if (clear_screen) {
        tui_raw_begin(t);
        fputs("\x1b[H\x1b[2J\x1b[3J", stdout);
        tui_raw_end(t);
    }
    tui_sys(t, "new session");
}

static bool ensure_backend(tui *t) {
    if (!t->session) t->session = session_new(t->ctx);
    if (!t->session) { tui_err(t, "could not create a session"); return false; }
    if (t->engine) return true;

    char err[512];
    err[0] = 0;
    tny_engine *engine = tny_engine_new(t->ctx, t->session, t->perm,
                                        perm_hook, t);
    if (!engine) { tui_err(t, "could not create the runtime"); return false; }
    tny_engine_set_cancel_probe(engine, tui_cancel_probe, t);
    tny_backend *bk = tui_prewarm_take(t);
    if (bk) {
        if (tny_engine_prepare(engine, bk, TNY_ENGINE_PREPARE_RESUMED,
                               err, sizeof err) != 0) {
            tny_engine_preserve_session_on_free(engine);
            tny_engine_free(engine);
            tui_err(t, err);
            return false;
        }
        t->engine = engine;
        t->bk_adopted = true;
        tny_settings_remember_use(t->ctx);
        return true;
    }
    bk = tny_backend_create((tny_backend_id)t->ctx->backend, t->ctx);
    if (!bk || tny_engine_prepare(engine, bk, TNY_ENGINE_PREPARE_FRESH,
                           err, sizeof err) != 0) {
        tny_engine_preserve_session_on_free(engine);
        tny_engine_free(engine);
        tui_err(t, err);
        return false;
    }
    t->engine = engine;
    tny_settings_remember_use(t->ctx); /* next launch defaults to this provider */
    return true;
}

static void after_turn(tui *t) {
    tui_bol(t);
    tui_write(t, "\n", 1);
    t->gap = 0;
    switch (t->stop) {
    case TNY_STOP_DONE: break;
    case TNY_STOP_INTERRUPTED: tui_sys(t, "interrupted"); break;
    case TNY_STOP_DENIED: tui_sys(t, "stopped: permission denied"); break;
    case TNY_STOP_STEP_LIMIT: tui_sys(t, "stopped: step limit reached"); break;
    case TNY_STOP_ERROR: tui_sys(t, "stopped: error"); break;
    }
    if (!t->tty) /* dumb mode has no status row: leave one in the transcript */
        tui_sysf(t, "── %s  %s  %lld/%lld tok ──", tny_provider_name(t->ctx),
                 t->ctx->model ? t->ctx->model : "default",
                 (long long)t->in_tok, (long long)t->out_tok);
    t->cancel_ms = 0;
    buf_clear(&t->note);
    t->dirty = true;
    if (t->n_queue) {
        if (t->stop == TNY_STOP_DONE && !t->quit) {
            char *next = queue_pop(t);
            tui_submit(t, next);
            free(next);
        } else {
            char m[80];
            snprintf(m, sizeof m, "dropped %d queued message%s", t->n_queue,
                     t->n_queue == 1 ? "" : "s");
            tui_sys(t, m);
            tui_queue_clear(t);
        }
    }
}

void tui_cancel_turn(tui *t) {
    if (!t->turn_active || !t->engine) return;
    if (t->cancel_ms) return;
    if (t->n_queue) {
        char m[80];
        snprintf(m, sizeof m, "dropped %d queued message%s", t->n_queue,
                 t->n_queue == 1 ? "" : "s");
        tui_sys(t, m);
        tui_queue_clear(t);
    }
    t->cancel_ms = now_ms();
    tui_note(t, "cancelling…");
    tny_engine_cancel(t->engine);
}

void tui_submit(tui *t, const char *text) {
    tui_overlay_clear(t); /* the menu interaction is over */
    const char *s = text;
    while (*s == ' ' || *s == '\t') s++;
    if (t->wiz_step) { tui_wizard_feed(t, s); return; }
    if (!*s) { t->dirty = true; return; }

    if (*s == '/') {
        tui_hist_add(t, s);
        tui_bol(t);
        tui_command(t, s);
        t->dirty = true;
        return;
    }
    if (t->turn_active) {
        tui_hist_add(t, s);
        char err[256];
        if (t->engine && !t->cancel_ms && !t->n_queue &&
            tny_engine_steer(t->engine, s, err, sizeof err) == 0) {
            /* into the running turn: echo it so the transcript reads in
             * order; the backend owns the text now and hands it back via
             * STEER_REJECTED if the host refuses it (docs/adr/0013) */
            tui_bol(t);
            tui_linef(t, "%s› %s%s %ssteer%s", tui_attr(t, "\x1b[1m"), s,
                      tui_attr(t, "\x1b[0m"), tui_attr(t, "\x1b[2m"),
                      tui_attr(t, "\x1b[0m"));
            t->gap = 1;
        } else {
            tui_queue_push(t, s, false); /* sent when this turn ends */
        }
        t->dirty = true;
        return;
    }

    tui_hist_add(t, s);
    tui_linef(t, "%s› %s%s", tui_attr(t, "\x1b[1m"), s, tui_attr(t, "\x1b[0m"));
    t->gap = 1; /* one blank line before the first agent output */
    /* connect/send below can block for a while: show the echoed prompt and a
     * status note now so Enter never looks like a freeze */
    tui_note(t, t->engine ? "sending…" : "starting %s…", tny_provider_name(t->ctx));
    tui_render_force(t);
    if (!ensure_backend(t)) {
        buf_clear(&t->note);
        t->dirty = true;
        return;
    }

    buf_clear(&t->prompt_text);
    buf_appends(&t->prompt_text, s);
    buf_clear(&t->last_reply);

    tui_note(t, "sending…");
    tui_render_force(t);
    char err[512];
    const char **imgs = t->n_images ? (const char **)t->images : NULL;
    t->turn_done = false;
    int rc = tny_engine_start(t->engine, s, imgs, err, sizeof err);
    if (rc != 0 && t->bk_adopted) {
        /* the pre-warmed, pre-resumed host may have died while the shell sat
         * idle: one clean retry through the ordinary lazy path */
        tui_drop_backend(t);
        if (!ensure_backend(t)) {
            buf_clear(&t->note);
            t->dirty = true;
            return;
        }
        rc = tny_engine_start(t->engine, s, imgs, err, sizeof err);
    }
    if (rc != 0) {
        buf_clear(&t->note);
        tui_err(t, err);
        t->dirty = true;
        return;
    }
    t->bk_adopted = false;
    buf_clear(&t->note); /* the spinner in the status row takes over */
    for (int i = 0; i < t->n_images; i++) { free(t->images[i]); t->images[i] = NULL; }
    t->n_images = 0;
    t->turn_active = true;
    t->cancel_ms = 0;
    drain_engine_events(t);
    t->dirty = true;
}

/* ---- run loop ---- */

static void banner(tui *t) {
    tui_linef(t, "%stny %s%s  %s  %s  %s", tui_attr(t, "\x1b[1m"), TNY_VERSION,
              tui_attr(t, "\x1b[0m"), tny_provider_name(t->ctx),
              t->ctx->model ? t->ctx->model : "default model",
              tny_perm_mode_name(t->ctx->perm_mode));
    tui_sys(t, "/help for commands · @ files · $ skills · ctrl-c twice to exit");
    if (!t->tty)
        tui_sys(t, "not a terminal: status bar disabled, approvals auto-deny");
}

static int tui_run(tny_ctx *ctx, const cli_globals *g, const char *session_id) {
    tui t;
    memset(&t, 0, sizeof t);
    t.ctx = ctx;
    t.g = g;
    buf_init(&t.out);
    buf_init(&t.partial);
    buf_init(&t.input);
    buf_init(&t.overlay);
    buf_init(&t.note);
    buf_init(&t.last_reply);
    buf_init(&t.prompt_text);
    t.perm = perm_new(ctx);
    t.tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    tny_color_resolve(ctx, t.tty, &t.color, &t.attr);
    if (t.tty && !term_raw(t.attr)) {
        t.tty = false;
        tny_color_resolve(ctx, false, &t.color, &t.attr);
    }
#ifdef __EMSCRIPTEN__
    /* the page terminal is xterm.js: already raw, always a tty — the
     * Emscripten termios/isatty stubs must not demote it (docs/adr/0017) */
    if (js_tui_page()) {
        t.tty = true;
        tny_color_resolve(ctx, true, &t.color, &t.attr);
    }
#endif
    tui_size(&t);

    install(SIGWINCH, on_winch);
    install(SIGINT, on_sigint);
    install(SIGTERM, on_fatal);
    install(SIGHUP, on_fatal);
    signal(SIGPIPE, SIG_IGN);

    tui_hist_load(&t);
    banner(&t);

    if (session_id) {
        t.session = session_open(ctx, session_id);
        if (t.session) {
            session_get_usage(t.session, &t.in_tok, &t.out_tok);
            tui_linef(&t, "  resumed %s  %s", t.session->id,
                      session_title(t.session) ? session_title(t.session) : "(untitled)");
        } else {
            tui_err(&t, "session not found; starting a new one");
        }
    } else if (g->resume_picker) {
        tui_command(&t, "/sessions");
    }
    /* warm the provider's host now — after the session is open, so a resumed
     * session's host pointer rides along and the first prompt starts
     * instantly; a failure stays silent and resurfaces on the lazy path */
    tui_prewarm_start(&t);

    t.dirty = true;
    while (!t.quit) {
        tui_render(&t);

        struct pollfd fds[9];
        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        int nb = 0;
        if (t.turn_active && t.engine)
            nb = tny_engine_pollfds(t.engine, fds + 1, 8);
        int pr = tny_poll(fds, (nfds_t)(1 + nb), t.turn_active ? 40 : 400);
        if (pr < 0 && errno != EINTR) break;

        if (g_winch) { g_winch = 0; tui_size(&t); t.dirty = true; }
        if (g_sigint) {
            g_sigint = 0;
            if (t.turn_active) tui_cancel_turn(&t);
            else { t.quit = true; t.exit_code = 130; }
        }
        if (t.turn_active && now_ms() - t.spin_ms >= 120) {
            t.spin_ms = now_ms();
            t.spin = (t.spin + 1) % 10;
            if (!t.note.len) t.dirty = true; /* an explicit note wins the row */
        }
        /* POLLNVAL is stdin-is-gone (macOS polls /dev/null that way): treat
         * it as EOF. Excluding it livelocks the loop at 100% CPU — poll
         * returns instantly forever and read never runs. */
        if (pr > 0 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            if (tui_read_input(&t) < 0) t.quit = true;
        }
        if (t.turn_active && t.engine) {
            tny_engine_dispatch(t.engine, fds + 1, nb);
            drain_engine_events(&t);
        }
        if (t.want_cancel) { t.want_cancel = false; tui_cancel_turn(&t); }
        if (t.turn_done) { t.turn_done = false; after_turn(&t); }
        /* a host that never confirms the cancel must not wedge the shell */
        if (t.turn_active && t.cancel_ms && now_ms() - t.cancel_ms > 5000) {
            tui_drop_backend(&t);
            t.turn_active = false;
            t.stop = TNY_STOP_INTERRUPTED;
            after_turn(&t);
        }
    }

    if (t.turn_active && t.engine) {
        tny_engine_cancel(t.engine);
        drain_engine_events(&t);
    }
    tui_raw_begin(&t);
    fflush(stdout);
    term_restore();

    tui_prewarm_drop(&t);
    if (t.engine) tny_engine_end_session(t.engine, "exit");
    tui_drop_backend(&t);
    if (t.session) {
        session_save(t.session);
        session_close(t.session);
    }
    mcp_shutdown_all();
    perm_free(t.perm);
    tui_items_clear(&t);
    tui_files_free(&t);
    tui_hist_free(&t);
    tui_wizard_cancel(&t);
    for (int i = 0; i < t.n_images; i++) free(t.images[i]);
    tui_queue_clear(&t);
    buf_free(&t.out);
    buf_free(&t.partial);
    buf_free(&t.input);
    buf_free(&t.overlay);
    buf_free(&t.note);
    buf_free(&t.last_reply);
    buf_free(&t.prompt_text);
    return t.exit_code;
}

int cmd_tui(tny_ctx *ctx, const cli_globals *g) { return tui_run(ctx, g, NULL); }

int cmd_tui_resume(tny_ctx *ctx, const cli_globals *g, const char *session_id) {
    return tui_run(ctx, g, session_id);
}
