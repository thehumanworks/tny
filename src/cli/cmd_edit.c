/* cmd_edit.c — stateless exact-match file editing (ADR 0064).
 * The stdin payload grammar and the printed result live in core/edit.c so
 * this verb and the in-process intercept (ADR 0063) stay byte-identical. */
#include "cli/cli.h"
#include "core/edit.h"
#include "util/util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t edit_interrupted;

static void edit_on_interrupt(int signal_number) {
    (void)signal_number;
    edit_interrupted = 1;
}

static bool edit_was_interrupted(void *userdata) {
    (void)userdata;
    return edit_interrupted != 0;
}

static int read_stdin(buf_t *input) {
    char chunk[8192];
    while (!edit_interrupted) {
        size_t n = fread(chunk, 1, sizeof chunk, stdin);
        if (n) buf_append(input, chunk, n);
        if (buf_oom(input)) return -1;
        if (n < sizeof chunk) {
            if (ferror(stdin)) {
                if (edit_interrupted || errno == EINTR) return -2;
                return -1;
            }
            break;
        }
    }
    return edit_interrupted ? -2 : 0;
}

static int edit_usage(const char *message) {
    buf_t err;
    buf_init(&err);
    tny_edit_usage(message, &err);
    if (err.len) fwrite(err.data, 1, err.len, stderr);
    buf_free(&err);
    return 1;
}

int cmd_edit(const cli_globals *g, int argc, char **argv) {
    bool json = g->json;
    const char *marker = "***";
    const char *path = NULL;
    bool positional = false;
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (!positional && strcmp(arg, "--") == 0) {
            positional = true;
        } else if (!positional && strcmp(arg, "--json") == 0) {
            json = true;
        } else if (!positional && strcmp(arg, "--marker") == 0) {
            if (++i >= argc) return edit_usage("--marker requires a value");
            marker = argv[i];
        } else if (!positional && (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)) {
            help_for("edit");
            return 0;
        } else if (!positional && arg[0] == '-') {
            char message[256];
            snprintf(message, sizeof message, "unknown option '%s'", arg);
            return edit_usage(message);
        } else if (path) {
            return edit_usage("expected exactly one FILE");
        } else {
            path = arg;
        }
    }
    if (!path) return edit_usage("missing FILE");
    if (!*marker || strchr(marker, '\n') || strchr(marker, '\r'))
        return edit_usage("--marker must be a non-empty single-line string");
    if (g->ssh) return edit_usage("--ssh is not supported by the standalone edit verb");

    struct sigaction action = {0};
    struct sigaction previous = {0};
    action.sa_handler = edit_on_interrupt;
    sigemptyset(&action.sa_mask);
    edit_interrupted = 0;
    sigaction(SIGINT, &action, &previous);

    buf_t input;
    buf_init(&input);
    int read_status = read_stdin(&input);
    if (read_status != 0) {
        buf_free(&input);
        sigaction(SIGINT, &previous, NULL);
        if (read_status == -2) return 130;
        return edit_usage("could not read stdin");
    }

    tny_edit_payload payload = {0};
    char parse_error[256];
    bool parsed = tny_edit_parse_payload(input.data, input.len, json, marker, &payload, parse_error,
                                         sizeof parse_error);
    buf_free(&input);
    if (!parsed) {
        sigaction(SIGINT, &previous, NULL);
        return edit_usage(parse_error);
    }
    if (edit_interrupted) {
        tny_edit_payload_free(&payload);
        sigaction(SIGINT, &previous, NULL);
        return 130;
    }

    /* Reserve the success line before the file can change: reporting a
     * completed edit must not be the allocation that fails. */
    buf_t out, err;
    buf_init(&out);
    buf_init(&err);
    buf_reserve(&out, strlen(path) * 2 + 160);
    if (buf_oom(&out)) {
        tny_edit_payload_free(&payload);
        sigaction(SIGINT, &previous, NULL);
        buf_free(&out);
        buf_free(&err);
        return edit_usage("out of memory while preparing output");
    }

    tny_edit_result result = {0};
    tny_edit_hooks hooks = {.interrupted = edit_was_interrupted};
    tny_edit_status status = tny_edit_file_exact(path, payload.old_text, payload.new_text,
                                                 payload.replace_all, &hooks, &result);
    tny_edit_payload_free(&payload);
    sigaction(SIGINT, &previous, NULL);
    int rc = tny_edit_render(path, json, status, &result, &out, &err);
    tny_edit_result_free(&result);
    if (err.len) fwrite(err.data, 1, err.len, stderr);
    if (out.len) fwrite(out.data, 1, out.len, stdout);
    buf_free(&out);
    buf_free(&err);
    return rc;
}
