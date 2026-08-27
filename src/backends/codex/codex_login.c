/* codex_login.c — ChatGPT sign-in through the app-server (docs/adr/0019).
 *
 * `tny --provider codex login` spawns (or attaches to) `codex app-server`
 * and calls account/login/start:
 *   - browser flow (default): the result carries authUrl; the app-server
 *     hosts the localhost OAuth callback, so the host must stay up until
 *     account/login/completed arrives.
 *   - device-code flow (--device): the result carries verificationUrl +
 *     userCode for a second device; the host polls.
 * Success drops auth.json under $CODEX_HOME (default ~/.codex) — the same
 * artifact tny_codex_auth_present() auto-detects. Hosts too old for the
 * login RPC fall back to `codex login`.
 * Never print the ready-line JSON or any token (CLAUDE.md). */
#include "backends/codex/codex.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CX_LOGIN_START_TIMEOUT_MS 30000
#define CX_LOGIN_WAIT_MS          (10 * 60 * 1000) /* browser round trip */

static volatile sig_atomic_t g_login_interrupted;

static void on_sigint(int sig) {
    (void)sig;
    g_login_interrupted = 1;
}

/* Best-effort browser launch; the URL is already on stdout either way. */
static void open_browser(const char *url) {
    pid_t pid = fork();
    if (pid != 0) return;
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, 0);
        dup2(devnull, 1);
        dup2(devnull, 2);
        if (devnull > 2) close(devnull);
    }
#ifdef __APPLE__
    execlp("open", "open", url, (char *)NULL);
#else
    execlp("xdg-open", "xdg-open", url, (char *)NULL);
#endif
    _exit(127);
}

/* Older app-servers: no account/login/start — the codex CLI owns the flow. */
static int login_via_cli(tny_ctx *ctx) {
    buf_t cmd;
    buf_init(&cmd);
    buf_appendf(&cmd, "%s login", ctx->codex_bin ? ctx->codex_bin : "codex");
    int rc = system(cmd.data);
    buf_free(&cmd);
    return rc == 0 ? 0 : 1;
}

static void login_cancel(cx_impl *o, const char *login_id) {
    if (!login_id) return;
    buf_t p;
    buf_init(&p);
    buf_appends(&p, "{\"loginId\":");
    jescape(&p, login_id);
    buf_appends(&p, "}");
    cx_request(o, "account/login/cancel", p.data, CXR_FREE);
    buf_free(&p);
    cx_flush(o);
}

int tny_codex_login(tny_ctx *ctx, bool device) {
    tny_backend *b = tny_backend_codex_new(ctx);
    if (!b) return 1;
    char err[512];
    fprintf(stderr, "connecting to codex app-server…\n");
    if (b->connect(b, err, sizeof err) != 0) {
        fprintf(stderr, "tny: %s\n", err);
        fprintf(stderr, "falling back to `%s login`\n", ctx->codex_bin ? ctx->codex_bin : "codex");
        b->destroy(b);
        return login_via_cli(ctx);
    }
    cx_impl *o = b->impl;
    o->login_done = false;
    o->login_ok = false;
    o->login_err[0] = 0;

    const char *params = device ? "{\"type\":\"chatgptDeviceCode\"}" : "{\"type\":\"chatgpt\"}";
    yyjson_doc *doc = cx_request_sync(o, "account/login/start", params, CX_LOGIN_START_TIMEOUT_MS,
                                      err, sizeof err);
    if (!doc) {
        /* -32601 (or any refusal): let the codex CLI own the ceremony */
        fprintf(stderr, "tny: %s\n", err);
        fprintf(stderr, "falling back to `%s login`\n", ctx->codex_bin ? ctx->codex_bin : "codex");
        b->destroy(b);
        return login_via_cli(ctx);
    }
    yyjson_val *res = jget(yyjson_doc_get_root(doc), "result");
    const char *auth_url = jget_str(res, "authUrl");
    const char *ver_url = jget_str(res, "verificationUrl");
    const char *user_code = jget_str(res, "userCode");
    const char *lid = jget_str(res, "loginId");
    char *login_id = lid ? xstrdup(lid) : NULL;
    if (ver_url && user_code) {
        printf("On any device, open:  %s\n", ver_url);
        printf("and enter the code:   %s\n", user_code);
    } else if (auth_url) {
        printf("Sign in with your ChatGPT account:\n\n  %s\n\n", auth_url);
        open_browser(auth_url);
    }
    fflush(stdout);
    yyjson_doc_free(doc);

    fprintf(stderr, "waiting for the sign-in to finish (Ctrl-C aborts)…\n");
    struct sigaction sa = {0}, old_sa;
    sa.sa_handler = on_sigint;
    g_login_interrupted = 0;
    sigaction(SIGINT, &sa, &old_sa);

    int rc = 1;
    int64_t deadline = now_ms() + CX_LOGIN_WAIT_MS;
    while (!o->login_done) {
        if (g_login_interrupted) {
            fprintf(stderr, "tny: login aborted\n");
            login_cancel(o, login_id);
            goto out;
        }
        if (cx_pump_once(o, 200) != 0 || cx_child_gone(o)) {
            fprintf(stderr, "tny: codex app-server went away during login\n");
            goto out;
        }
        if (now_ms() > deadline) {
            fprintf(stderr, "tny: login timed out after %d minutes\n", CX_LOGIN_WAIT_MS / 60000);
            login_cancel(o, login_id);
            goto out;
        }
    }
    if (o->login_ok) {
        printf("Signed in.%s\n", tny_codex_auth_present()
                                     ? " The codex login is saved — try `tny --provider codex "
                                       "ask \"hi\"`."
                                     : "");
        rc = 0;
    } else {
        fprintf(stderr, "tny: codex login failed: %s\n",
                o->login_err[0] ? o->login_err : "no detail from the host");
    }
out:
    sigaction(SIGINT, &old_sa, NULL);
    free(login_id);
    b->destroy(b);
    if (g_login_interrupted) return 130;
    return rc;
}
