/* cmd_jobs.c — `tny jobs …`: the shell surface of the durable job service
 * (docs/jobs.md, docs/adr/0093). Stdin and rendering only: the argv grammar,
 * every decision, record and process belong to core/jobs.c, which the typed
 * job_* tools and the terminal interception drive with the same requests. */
#include "cli/cli.h"
#include "core/jobs.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void jobs_usage(const char *message) {
    if (message) fprintf(stderr, "tny: jobs: %s\n", message);
    fputs("Usage: tny jobs <submit|status|wait|cancel|retry|logs|list|rm> …\n"
          "Run `tny jobs --help` for the full grammar.\n",
          stderr);
}

/* Bounded stdin: a prompt or a request document. A terminal is not stdin. */
static char *jobs_read_stdin(size_t *len_out) {
    *len_out = 0;
    if (isatty(STDIN_FILENO)) return NULL;
    buf_t body;
    buf_init(&body);
    for (;;) {
        char chunk[4096];
        ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
        if (n < 0) {
            buf_free(&body);
            return NULL;
        }
        if (!n) break;
        if (body.len + (size_t)n > TNY_JOBS_REQUEST_MAX) {
            buf_free(&body);
            return NULL;
        }
        buf_append(&body, chunk, (size_t)n);
    }
    if (buf_oom(&body) || !body.len) {
        buf_free(&body);
        return NULL;
    }
    *len_out = body.len;
    return buf_detach(&body);
}

int cmd_jobs(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    if (argc < 1) {
        jobs_usage(NULL);
        return 1;
    }
    /* The hidden supervisor entry point: its descriptors are the private
     * payload, the acknowledgment pipe and the live owner lock. */
    if (strcmp(argv[0], "_worker") == 0)
        return argc == 2 ? tny_jobs_worker_main(ctx, argv[1], 0, 1, 3) : 1;

    /* Stdin is read only when the arguments do not already carry the prompt or
     * the request document: `--prompt TEXT` must never block on a pipe nobody
     * is going to close. */
    bool wants_stdin = strcmp(argv[0], "submit") == 0;
    for (int k = 1; k < argc && wants_stdin; k++) {
        if (strcmp(argv[k], "--prompt") == 0) wants_stdin = false;
        if (strcmp(argv[k], "--request") == 0 && k + 1 < argc && strcmp(argv[k + 1], "-") != 0)
            wants_stdin = false;
    }
    size_t stdin_len = 0;
    char *stdin_text = wants_stdin ? jobs_read_stdin(&stdin_len) : NULL;
    char *request = NULL;
    bool json_flag = false;
    const char *error = NULL;
    tny_jobs_op op =
        tny_jobs_parse_argv(argc, argv, stdin_text, stdin_len, &request, &json_flag, &error);
    if (stdin_text) secure_free(stdin_text);
    if (op == TNY_JOBS_OP_NONE) {
        jobs_usage(error);
        free(request);
        return 1;
    }
    bool json = json_flag || g->json;
    yyjson_doc *doc = jparse(request, strlen(request));
    yyjson_val *args = doc ? yyjson_doc_get_root(doc) : NULL;
    buf_t out;
    buf_init(&out);
    char err[320] = "";
    int rc = 1;
    if (args) rc = tny_jobs_run(ctx, op, args, &out, err, sizeof err);
    else snprintf(err, sizeof err, "the request could not be parsed");
    yyjson_doc_free(doc);
    secure_free(request);

    if (err[0] && rc != 0) fprintf(stderr, "tny: jobs: %s\n", err);
    if (out.len) {
        if (json) fwrite(out.data, 1, out.len, stdout);
        else {
            buf_t human;
            buf_init(&human);
            tny_jobs_render_human(op, out.data, &human);
            if (human.len) fwrite(human.data, 1, human.len, stdout);
            buf_free(&human);
        }
    }
    buf_free(&out);
    return rc;
}
