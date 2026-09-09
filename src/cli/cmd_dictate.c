/* Thin dictation CLI: progress on stderr, prompt text (or JSON) on stdout. */
#include "cli/cli.h"
#include "core/dictation.h"
#include "json/json.h"
#include "util/tny_poll.h"
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
static bool cancelled(void *ud) {
    (void)ud;
    return interrupted != 0;
}

int cmd_dictate(const cli_globals *g, int argc, char **argv) {
    tny_dictation_request r = {.cancelled = cancelled};
    bool json = g->json, check = false;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            help_for("dictate");
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
        if (strcmp(a, "--seconds") == 0) {
            if (i + 1 >= argc) goto invalid;
            i++;
            if (!*argv[i]) goto invalid;
            char *end;
            errno = 0;
            long n = strtol(argv[i], &end, 10);
            if (errno || *end || n < 1 || n > TNY_DICTATION_SECONDS_MAX) goto invalid;
            r.seconds = (int)n;
            continue;
        }
        const char **slot = strcmp(a, "--stt-provider") == 0 ? &r.provider
                            : strcmp(a, "--input-file") == 0 ? &r.input_file
                            : strcmp(a, "--device") == 0     ? &r.device
                                                             : NULL;
        if (!slot || i + 1 >= argc) goto invalid;
        i++;
        if (!*argv[i]) goto invalid;
        *slot = argv[i];
    }
    if (r.input_file && (r.seconds || r.device)) goto invalid;
    /* Do not load or refresh the conversation profile for a standalone service. */
    tny_ctx ctx = {.xai_api_key = (char *)g->xai_api_key,
                   .chatgpt_token = (char *)g->chatgpt_token,
                   .chatgpt_account_id = (char *)g->chatgpt_account_id};
    char err[256] = "";
    if (check) {
        bool ok = tny_dictation_available(&ctx, r.provider, !r.input_file, err, sizeof err);
        if (json) printf("{\"kind\":\"dictate\",\"available\":%s}\n", ok ? "true" : "false");
        else puts(ok ? "dictation available" : "dictation unavailable");
        if (!ok) fprintf(stderr, "tny: dictate: %s\n", err);
        return ok ? 0 : 1;
    }
    bool keyboard = !r.input_file && isatty(STDIN_FILENO);
    if (!r.input_file && !r.seconds && !keyboard) {
        fputs("tny: dictate: microphone needs a terminal or --seconds N\n", stderr);
        return 1;
    }
    interrupted = 0;
    struct sigaction sa = {0}, oldint = {0}, oldterm = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    bool have_int = sigaction(SIGINT, &sa, &oldint) == 0;
    bool have_term = sigaction(SIGTERM, &sa, &oldterm) == 0;
    tny_dictation *d = tny_dictation_start(&ctx, &r, err, sizeof err);
    int rc = interrupted ? 130 : 1;
    if (d) {
        tny_dictation_state state = tny_dictation_get_state(d);
        fputs(state == TNY_DICTATION_RECORDING
                  ? "Listening… Enter to transcribe, Ctrl-C to cancel (5-minute limit).\n"
                  : "Transcribing… Ctrl-C to cancel.\n",
              stderr);
        while (tny_dictation_get_state(d) != TNY_DICTATION_DONE) {
            if (interrupted) {
                tny_dictation_cancel(d);
                break;
            }
            tny_dictation_step(d);
            tny_dictation_state next = tny_dictation_get_state(d);
            if (next == TNY_DICTATION_DONE) break;
            if (next != state) {
                state = next;
                fputs("Transcribing… Ctrl-C to cancel.\n", stderr);
            }
            struct pollfd pf[2] = {
                {tny_dictation_fd(d), POLLIN, 0},
                {keyboard && state == TNY_DICTATION_RECORDING ? STDIN_FILENO : -1, POLLIN, 0}};
            int p = tny_poll(pf, 2, 40);
            if (p < 0 && errno != EINTR) {
                tny_dictation_cancel(d);
                break;
            }
            if (pf[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
                char keys[128];
                ssize_t n = read(STDIN_FILENO, keys, sizeof keys);
                if (n <= 0 && (n == 0 || errno != EINTR)) tny_dictation_cancel(d);
                else if (n > 0 && (memchr(keys, '\n', (size_t)n) || memchr(keys, '\r', (size_t)n)))
                    tny_dictation_finish(d);
            }
        }
        if (interrupted && !tny_dictation_result(d, NULL, NULL)) {
            rc = 130;
            snprintf(err, sizeof err, "dictation interrupted");
        } else {
            const char *text, *error;
            rc = tny_dictation_result(d, &text, &error);
            if (rc) snprintf(err, sizeof err, "%s", error);
            else if (json) {
                buf_t out = {0};
                buf_appends(&out, "{\"kind\":\"dictate\",\"provider\":");
                jescape(&out, tny_dictation_provider_name(d));
                buf_appends(&out, ",\"text\":");
                jescape(&out, text);
                buf_appends(&out, "}\n");
                if (out.oom || fwrite(out.data, 1, out.len, stdout) != out.len) rc = 1;
                buf_free(&out);
            } else if (puts(text) == EOF) rc = 1;
        }
        tny_dictation_free(d);
    }
    if (!rc && fflush(stdout)) rc = 1;
    if (have_int) sigaction(SIGINT, &oldint, NULL);
    if (have_term) sigaction(SIGTERM, &oldterm, NULL);
    if (rc) fprintf(stderr, "tny: dictate: %s\n", *err ? err : "dictation output failed");
    return rc;
invalid:
    fputs("tny: dictate: invalid options (see tny dictate --help)\n", stderr);
    return 1;
}
