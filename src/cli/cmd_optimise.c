#include "cli/cli.h"
#include "core/optimise.h"
#include "core/ssh.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void on_signal(int sig) {
    (void)sig;
    interrupted = 1;
}

int cmd_optimise(const cli_globals *g, int argc, char **argv) {
    tny_optimise_request r = {.provider = g->backend, .model = g->model};
    bool json = g->json, use_stdin = false, literal = false;
    buf_t prompt = {0};
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!literal && (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0)) {
            buf_free(&prompt);
            help_for("optimise");
            return 0;
        }
        if (!literal && strcmp(a, "--") == 0) {
            literal = true;
            continue;
        }
        if (!literal && strcmp(a, "--stdin") == 0) {
            use_stdin = true;
            continue;
        }
        if (!literal && strcmp(a, "--json") == 0) {
            json = true;
            continue;
        }
        const char **slot = !literal && strcmp(a, "--provider") == 0 ? &r.provider
                            : !literal && strcmp(a, "--model") == 0  ? &r.model
                                                                     : NULL;
        if (slot) {
            if (i + 1 >= argc || !*argv[i + 1]) goto invalid;
            *slot = argv[++i];
            continue;
        }
        if (!literal && a[0] == '-') goto invalid;
        if (prompt.len) buf_appends(&prompt, " ");
        buf_appends(&prompt, a);
        if (prompt.oom || prompt.len > TNY_OPTIMISE_TEXT_MAX) goto invalid;
    }
    if (use_stdin && prompt.len) goto invalid;
    if (use_stdin || (!prompt.len && !isatty(STDIN_FILENO))) {
        char chunk[4096];
        size_t n;
        while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0) {
            if (n > TNY_OPTIMISE_TEXT_MAX - prompt.len || memchr(chunk, 0, n)) goto invalid;
            buf_append(&prompt, chunk, n);
            if (prompt.oom) goto invalid;
        }
        if (ferror(stdin)) goto invalid;
    }
    r.text = prompt.data;
    tny_ctx *ctx = tny_ctx_load(g->cwd);
    if (!ctx) {
        buf_free(&prompt);
        return 1;
    }
    ctx->chatgpt_token = g->chatgpt_token ? xstrdup(g->chatgpt_token) : NULL;
    ctx->chatgpt_account_id = g->chatgpt_account_id ? xstrdup(g->chatgpt_account_id) : NULL;
    for (int i = 0; i < g->n_add_dirs; i++) {
        char **dirs = realloc(ctx->extra_dirs, sizeof *dirs * (size_t)(ctx->n_extra_dirs + 1));
        if (!dirs) {
            tny_ctx_free(ctx);
            buf_free(&prompt);
            return 1;
        }
        ctx->extra_dirs = dirs;
        ctx->extra_dirs[ctx->n_extra_dirs++] = path_abs(g->add_dirs[i]);
    }
    int rc = 1;
    ctx->backend = TNY_BK_OPENAI; /* service tools are native; do not resolve the chat profile */
    if (g->max_steps) {
        int limit = tny_parse_max_steps(g->max_steps);
        if (limit < 0) {
            fputs("tny: optimise: invalid step limit\n", stderr);
            goto done;
        }
        if (limit > 0 && (ctx->max_steps <= 0 || limit < ctx->max_steps)) ctx->max_steps = limit;
    }
    if (g->ssh && cli_ssh_attach(ctx, g->ssh, g->ssh_cwd) != 0) goto done;
    interrupted = 0;
    struct sigaction sa = {0}, oldint = {0}, oldterm = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    bool have_int = sigaction(SIGINT, &sa, &oldint) == 0;
    bool have_term = sigaction(SIGTERM, &sa, &oldterm) == 0;
    char error[256] = "";
    tny_optimise *o = tny_optimise_start(ctx, &r, error, sizeof error);
    if (o) {
        fprintf(stderr, "Optimising prompt with %s / %s…\n", tny_optimise_provider(o),
                tny_optimise_model(o));
        while (tny_optimise_result(o, NULL, NULL) < 0) {
            if (interrupted) tny_optimise_cancel(o);
            tny_optimise_step(o);
            if (tny_optimise_result(o, NULL, NULL) >= 0) break;
            struct pollfd fds[TNY_BACKEND_POLLFD_MAX];
            int n = tny_optimise_pollfds(o, fds, TNY_BACKEND_POLLFD_MAX);
            if (tny_poll(fds, n > 0 ? (nfds_t)n : 0, 40) < 0 && errno != EINTR)
                tny_optimise_cancel(o);
        }
        const char *text, *err;
        rc = tny_optimise_result(o, &text, &err);
        if (interrupted) rc = 130;
        if (rc) snprintf(error, sizeof error, "%s", interrupted ? "optimisation cancelled" : err);
        else if (json) {
            buf_t out = {0};
            buf_appends(&out, "{\"kind\":\"optimise\",\"provider\":");
            jescape(&out, tny_optimise_provider(o));
            buf_appends(&out, ",\"model\":");
            jescape(&out, tny_optimise_model(o));
            buf_appends(&out, ",\"text\":");
            jescape(&out, text);
            buf_appends(&out, "}\n");
            if (out.oom || fwrite(out.data, 1, out.len, stdout) != out.len) rc = 1;
            buf_free(&out);
        } else if (puts(text) == EOF) rc = 1;
        if (!rc && fflush(stdout)) rc = 1;
        tny_optimise_free(o);
    }
    if (have_int) sigaction(SIGINT, &oldint, NULL);
    if (have_term) sigaction(SIGTERM, &oldterm, NULL);
    if (interrupted) rc = 130;
    if (rc) fprintf(stderr, "tny: optimise: %s\n", *error ? error : "optimisation failed");
done:
    if (ctx->ssh_host) ssh_disconnect(ctx);
    tny_ctx_free(ctx);
    buf_free(&prompt);
    return rc;
invalid:
    buf_free(&prompt);
    fputs("tny: optimise: invalid options or prompt (see tny optimise --help)\n", stderr);
    return 1;
}
