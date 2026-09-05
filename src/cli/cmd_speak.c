/* CLI speech adapter. Text stays on stdin (ADR 0064); no chat startup. */
#include "cli/cli.h"
#include "core/speech.h"
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

int cmd_speak(const cli_globals *g, int argc, char **argv) {
    tny_speech_request r = {.cancelled = cancelled};
    bool json = g->json, check = false;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            help_for("speak");
            return 0;
        }
        if (strcmp(a, "--json") == 0) {
            json = true;
            continue;
        }
        if (strcmp(a, "--check") == 0) {
            check = true;
            continue;
        }
        const char **slot = strcmp(a, "--voice") == 0          ? &r.voice
                            : strcmp(a, "--tts-provider") == 0 ? &r.provider
                            : strcmp(a, "--output-file") == 0  ? &r.output_file
                                                               : NULL;
        if (!slot || i + 1 >= argc || !*argv[i + 1]) {
            fputs("tny: speak: invalid option; text must be on stdin (see tny speak --help)\n",
                  stderr);
            return 1;
        }
        *slot = argv[++i];
    }
    /* This service consumes only ChatGPT flag credentials from the context.
     * Loading a selected chat profile here could refresh unrelated accounts. */
    tny_ctx ctx = {.chatgpt_token = (char *)g->chatgpt_token,
                   .chatgpt_account_id = (char *)g->chatgpt_account_id};
    char err[256] = "";
    if (check) {
        bool available = tny_speech_available(&ctx, r.provider, !r.output_file, err, sizeof err);
        if (json) printf("{\"kind\":\"speak\",\"available\":%s}\n", available ? "true" : "false");
        else puts(available ? "speech available" : "speech unavailable");
        if (!available) fprintf(stderr, "tny: speak: %s\n", err);
        return available ? 0 : 1;
    }
    if (isatty(STDIN_FILENO)) {
        fputs("tny: speak: pipe text on stdin (see tny speak --help)\n", stderr);
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
    for (;;) {
        char chunk[4096];
        ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
        if (interrupted) {
            rc = 130;
            break;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (n > 0 && (size_t)n > TNY_SPEECH_TEXT_MAX - text.len)) {
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
    if (!rc && !utf8_valid_bytes(text.data, text.len)) rc = 1;
    if (!rc) {
        r.text = text.data;
        rc = tny_speech_run(&ctx, &r, err, sizeof err);
    } else
        snprintf(err, sizeof err,
                 rc == 130 ? "speech interrupted" : "invalid or oversized stdin text");
    buf_free(&text);
    if (have_int) sigaction(SIGINT, &oldint, NULL);
    if (have_term) sigaction(SIGTERM, &oldterm, NULL);
    if (rc) fprintf(stderr, "tny: speak: %s\n", err);
    else if (json)
        printf("{\"kind\":\"speak\",\"ok\":true,\"played\":%s}\n",
               r.output_file ? "false" : "true");
    return rc;
}
