/* tui_runner.c — the interactive shell as a runner client (docs/adr/0053).
 *
 * In isolation mode the TUI never runs a turn in-process: a detached serve
 * runner owns the backend, the engine, and every session.json write, and
 * this file translates its NDJSON stream back into the same normalized
 * events the in-process path renders. The runner survives this shell —
 * a TUI crash mid-turn leaves the turn finishing into the session, ready
 * for `tny resume`.
 *
 * Ownership rules while a runner is alive:
 *   - the runner is the sole session writer; t->session is a read replica,
 *     re-opened from disk after every turn_end;
 *   - ctx mutations (model/provider/effort/workspace) restart the runner —
 *     tui_prewarm_drop maps here, so the existing "drop before mutating"
 *     call sites already do the right thing; mid-turn the restart is
 *     deferred until the turn ends. */
#include "tui/tui.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include "util/tny_poll.h"

bool tui_runner_mode(const tui *t) {
    /* Caller-side TLS can disable future forks on macOS while an already
     * healthy runner is serving this shell. Keep using that runner until a
     * rebind drops it; the replacement then takes the safe in-process path. */
    return t->rc != NULL || tny_isolation_enabled(t->ctx);
}

int tui_runner_fd(const tui *t) { return t->rc ? tny_runner_client_fd(t->rc) : -1; }

int tui_runner_ensure(tui *t, bool quiet) {
    if (t->rc) return 0;
    if (t->background_view && (!t->session || t->session->lock_fd < 0)) {
        if (!quiet) tui_err(t, "saved view requires explicit ownership via /continue");
        return -1;
    }
    /* Reap runners that ended earlier (provider switches, /new, bye). In
     * runner mode this shell's only children are runners and inline-waited
     * editor spawns, so a WNOHANG sweep cannot steal anyone's status. */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    if (!t->session) t->session = session_new(t->ctx);
    if (!t->session) {
        if (!quiet) tui_err(t, "could not create a session");
        return -1;
    }
    /* A live writer elsewhere (background child, another shell's runner)
     * owns this session: binding our own runner would hijack its socket. */
    if (t->session->lock_fd < 0 && session_is_running(t->ctx, t->session->id)) {
        if (!quiet)
            tui_err(t, "session is running in another process (tny session stop to stop it)");
        return -1;
    }
    char err[512];
    tny_runner_opts opts = {0};
    opts.serve = true;
    pid_t pid = tny_runner_spawn(t->ctx, t->session, &opts, err, sizeof err);
    if (pid < 0) {
        if (!quiet) tui_err(t, err);
        return -1;
    }
    char *sock = tny_runner_sock_path(t->session->dir);
    t->rc = sock ? tny_runner_client_connect(sock, 5000, TNY_RUNNER_OWNER, true) : NULL;
    free(sock);
    if (!t->rc) {
        if (!quiet) tui_err(t, "cannot reach the session runner");
        kill(pid, SIGTERM);
        return -1;
    }
    t->rc_pid = pid;
    tny_settings_remember_use(t->ctx);
    return 0;
}

void tui_runner_drop(tui *t, const char *reason) {
    if (!t->rc) return;
    if (!t->background_view) tny_runner_client_end(t->rc, reason ? reason : "exit");
    tny_runner_client_close(t->rc);
    t->rc = NULL;
    t->rc_pid = 0;
    if (t->background_view) {
        t->session_readonly = true;
        t->dirty = true;
    }
}

/* The runner finalized a turn into session.json; our copy is behind. */
static void runner_refresh_session(tui *t) {
    if (!t->session) return;
    char *sid = xstrdup(t->session->id);
    session_close(t->session);
    t->session = session_open(t->ctx, sid);
    free(sid);
}

static void runner_gone(tui *t) {
    tny_runner_client_close(t->rc);
    t->rc = NULL;
    t->rc_pid = 0;
    if (t->background_view) {
        t->session_readonly = true;
        t->dirty = true;
    }
}

bool tui_runner_stop(tui *t, bool force) {
    if (!t->rc || !t->session) return false;
    char err[256];
    pid_t pid = t->background_view ? session_read_pid(t->ctx, t->session->id) : t->rc_pid;
    /* Graceful exit requests end before waiting. Force bypasses the socket
     * entirely: the runner may be stuck inside dispatch or a tool. */
    if (!force) tny_runner_client_end(t->rc, "exit");
    int rc = force ? session_kill(t->ctx, t->session->id, pid, err, sizeof err)
                   : session_stop(t->ctx, t->session->id, true, err, sizeof err);
    if (rc < 0 || rc == 2) {
        tui_err(t, err);
        tui_sysf(t, "stop from another terminal: tny session stop %s --kill", t->session->id);
        return false;
    }
    runner_gone(t);
    waitpid(pid, NULL, WNOHANG);
    runner_refresh_session(t);
    t->turn_active = false;
    t->turn_done = true;
    t->stop = TNY_STOP_INTERRUPTED;
    return true;
}

/* HELLO is the live runner's display authority, not the saved row metadata. */
static void runner_hello(tui *t, const tny_runner_msg *m) {
    t->rc_pid = m->pid;
    t->ctx->perm_mode = m->perm_mode;
    if (m->turn_active) t->turn_active = true;
    if (m->provider) {
        free(t->ctx->provider_name);
        t->ctx->provider_name = xstrdup(m->provider);
    }
    if (m->model) {
        free(t->ctx->model);
        t->ctx->model = xstrdup(m->model);
    }
}

void tui_runner_dispatch(tui *t) {
    if (!t->rc) return;
    int alive = tny_runner_client_pump(t->rc);
    tny_runner_msg *m;
    while (t->rc && (m = tny_runner_client_pop(t->rc))) {
        switch (m->kind) {
        case TNY_RMSG_EVENT:
            if (m->ev.kind == TNY_EV_STATUS && m->ev.text &&
                str_starts(m->ev.text, "Background restart failed"))
                t->background_armed = false;
            tui_handle_backend_event(t, &m->ev);
            break;
        case TNY_RMSG_SNAPSHOT:
        case TNY_RMSG_RECOVERY:
            if (m->text && *m->text) {
                tui_bol(t);
                tui_write(t, m->text, strlen(m->text));
                tui_bol(t);
            }
            break;
        case TNY_RMSG_TURN_END:
            if (t->turn_active) {
                t->turn_active = false;
                t->turn_done = true;
                t->stop = m->ev.stop;
            }
            runner_refresh_session(t);
            break;
        case TNY_RMSG_TURN_ERR:
            if (m->text) tui_err(t, m->text);
            t->turn_active = false;
            buf_clear(&t->note);
            t->dirty = true;
            break;
        case TNY_RMSG_LOG:
            /* host stderr/diagnostics; raw prints would tear the drawn
             * screen, so surface them only under /trace */
            if (t->trace && m->text) tui_sys(t, m->text);
            break;
        case TNY_RMSG_ASK_USER: {
            char *answer = tui_ask_user(t, m->text ? m->text : "Question");
            if (answer && t->rc) tny_runner_client_ask_user_reply(t->rc, m->id, answer);
            free(answer);
            break;
        }
        case TNY_RMSG_BACKGROUNDED:
            t->rc_pid = m->pid;
            t->background_view = true;
            t->background_armed = false;
            tui_agents_open(t);
            break;
        case TNY_RMSG_HELLO: runner_hello(t, m); break;
        case TNY_RMSG_BYE: runner_gone(t); break;
        }
        tny_runner_msg_free(m);
    }
    if (t->rc && alive != 0) {
        runner_gone(t);
        if (t->turn_active) {
            tui_err(t, "the session runner exited unexpectedly");
            t->turn_active = false;
            t->turn_done = true;
            t->stop = TNY_STOP_ERROR;
            runner_refresh_session(t);
        }
    }
}

bool tui_runner_attach(tui *t, tny_session_state *session) {
    char *sock = tny_runner_sock_path(session->dir);
    tny_runner_client *client =
        sock ? tny_runner_client_connect(sock, 1000, TNY_RUNNER_OWNER, true) : NULL;
    free(sock);
    if (!client) return false;
    int64_t deadline = monotonic_ms() + 1500;
    while (monotonic_ms() < deadline) {
        int rc = tny_runner_client_pump(client);
        tny_runner_msg *hello = tny_runner_client_pop(client);
        if (hello) {
            bool ok = hello->kind == TNY_RMSG_HELLO;
            if (ok) {
                t->rc = client;
                t->turn_active = hello->turn_active;
                runner_hello(t, hello);
            }
            tny_runner_msg_free(hello);
            if (ok) return true;
            break;
        }
        if (rc != 0) break;
        struct pollfd p = {tny_runner_client_fd(client), POLLIN, 0};
        tny_poll(&p, 1, 50);
    }
    tny_runner_client_close(client);
    return false;
}
