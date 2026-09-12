/* Standalone image CLI: same service as native tools (ADR 0075). */
#include "cli/cli.h"
#include "core/image_service.h"
#include "cli/cmd_control.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void on_signal(int sig) {
    (void)sig;
    interrupted = 1;
}
static bool cancelled(void *ud) {
    (void)ud;
    return interrupted != 0;
}

int cmd_image_service(const cli_globals *g, int argc, char **argv) {
    tny_image_request r = {.cancelled = cancelled};
    bool json = g->json, check = false;
    while (argc > 0 && strcmp(argv[0], "--json") == 0) {
        json = true;
        argc--;
        argv++;
    }
    if (argc > 0 && strcmp(argv[0], "attach") == 0) return cmd_image(json, argc, argv);
    int parsed = tny_image_options(argc, argv, &r, &json, &check);
    if (parsed < 0) {
        help_for("image");
        return 0;
    }
    if (parsed || g->ssh) {
        fputs("tny: image: invalid options or --ssh; see tny image --help\n", stderr);
        return 1;
    }
    if (g->cwd && chdir(g->cwd) != 0) {
        fputs("tny: image: cannot enter --cwd\n", stderr);
        return 1;
    }
    tny_image_result result = {0};
    /* This service consumes only ChatGPT flag credentials from the context.
     * Loading a selected chat profile here could refresh unrelated accounts. */
    tny_ctx ctx = {.chatgpt_token = (char *)g->chatgpt_token,
                   .chatgpt_account_id = (char *)g->chatgpt_account_id};
    char err[256] = "";
    if (check) {
        bool available = tny_image_available(&ctx, r.provider, r.edit, err, sizeof err);
        if (json) printf("{\"kind\":\"image\",\"available\":%s}\n", available ? "true" : "false");
        else puts(available ? "image provider available" : "image provider unavailable");
        if (!available) fprintf(stderr, "tny: image: %s\n", err);
        return available ? 0 : 1;
    }
    /* A pure replay reruns a recorded prompt, so a terminal is not an error
     * there; piped text still overrides that prompt explicitly. */
    bool have_stdin = !isatty(STDIN_FILENO);
    if (!have_stdin && !r.replay) {
        fputs("tny: image: pipe text on stdin (see tny image --help)\n", stderr);
        return 1;
    }
    interrupted = 0;
    struct sigaction sa = {0}, oldint = {0}, oldterm = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    bool have_int = sigaction(SIGINT, &sa, &oldint) == 0;
    bool have_term = sigaction(SIGTERM, &sa, &oldterm) == 0;
    buf_t text;
    buf_init(&text);
    int rc = 0;
    while (have_stdin) {
        char chunk[4096];
        ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
        if (interrupted) {
            rc = 130;
            break;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (n > 0 && (size_t)n > TNY_IMAGE_PROMPT_MAX - text.len)) {
            rc = 1;
            break;
        }
        if (!n) break;
        buf_append(&text, chunk, (size_t)n);
        if (text.oom) {
            rc = 1;
            break;
        }
    }
    if (!rc && have_stdin && !utf8_valid_bytes(text.data, text.len)) rc = 1;
    if (!rc) {
        /* Empty piped text is not an override: a replay then keeps the
         * recorded prompt, and generate/edit still fail validation. */
        r.prompt = text.len ? text.data : r.replay ? NULL : "";
        rc = tny_image_run(&ctx, &r, &result, err, sizeof err);
    } else
        snprintf(err, sizeof err,
                 rc == 130 ? "image operation interrupted" : "invalid or oversized stdin text");
    buf_free(&text);
    if (have_int) sigaction(SIGINT, &oldint, NULL);
    if (have_term) sigaction(SIGTERM, &oldterm, NULL);
    char warning[320];
    /* Ordinary stdout stays the destination path; guidance goes to stderr. */
    if (!rc && tny_image_size_warning(result.requested_size,
                                      tny_image_size_status_name(result.size_status), result.width,
                                      result.height, warning, sizeof warning))
        fprintf(stderr, "tny: image: warning: %s\n", warning);
    /* Provenance is guidance, so it joins the warning on stderr and never
     * changes what a script reading stdout sees. */
    if (*result.manifest_path) fprintf(stderr, "tny: image: manifest: %s\n", result.manifest_path);
    if (rc) fprintf(stderr, "tny: image: %s\n", err);
    bool retained = result.code && strcmp(result.code, TNY_IMAGE_CODE_MANIFEST) == 0;
    if (json && (!rc || result.code)) {
        buf_t out;
        buf_init(&out);
        if (retained) tny_image_retained_json(&r, &result, err, &out);
        else if (rc) tny_image_error_json(&r, &result, err, &out);
        else tny_image_result_json(&r, &result, &out);
        if (out.oom || fwrite(out.data, 1, out.len, stdout) != out.len) rc = rc ? rc : 1;
        buf_free(&out);
    } else if (!rc) puts(r.output_file);
    return rc;
}
