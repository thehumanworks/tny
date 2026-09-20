/* Stdin/stdout adapter only. Tools and terminal interception use the same core
 * parser/detail/run APIs with captured runtime identity, not this operator path. */
#include "cli/cli.h"
#include "core/team_control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int cmd_team(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    (void)g;
    if (argc == 0 || strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0) {
        fputs(
            "Usage: tny team <start|status|collect|wait-any|cancel|verify|review|review-read> "
            "--request FILE|- "
            "[--json]\n"
            "All responses are JSON. start requires kind:ask, dag:true, one lead and >=2 workers.\n"
            "Run identity is the durable job id; task identity is its item index.\n"
            "cancel requires expected_attempt. wait-any timeout returns 124 and never cancels.\n"
            "verify currently refuses execution and reports unverified. See "
            "docs/team-control.md.\n",
            stdout);
        return argc ? 0 : 1;
    }
    /* An agent's terminal command must be intercepted with captured caller
     * context. A nested subprocess cannot promote itself to local-operator
     * authority by supplying a session ID in argv or the environment. This is
     * a defensive refusal, not a sandbox against arbitrary same-user programs. */
    const char *nested = getenv("TNY_NESTED");
    if (nested && strcmp(nested, "1") == 0) {
        fputs("tny: team: nested CLI requires the trusted team terminal adapter\n", stderr);
        return 1;
    }
    bool wants_stdin = false;
    for (int i = 1; i + 1 < argc; ++i)
        if (strcmp(argv[i], "--request") == 0 && strcmp(argv[i + 1], "-") == 0) wants_stdin = true;
    buf_t input = {0};
    if (wants_stdin) {
        if (isatty(STDIN_FILENO)) {
            fputs("tny: team: request stdin is a terminal\n", stderr);
            return 1;
        }
        for (;;) {
            char bytes[4096];
            size_t count = fread(bytes, 1, sizeof bytes, stdin);
            if (input.len + count > TNY_JOBS_REQUEST_MAX) {
                input.oom = true;
                break;
            }
            buf_append(&input, bytes, count);
            if (buf_oom(&input) || count < sizeof bytes) break;
        }
        if (ferror(stdin) || buf_oom(&input)) {
            buf_free(&input);
            fputs("tny: team: cannot read bounded request\n", stderr);
            return 1;
        }
    }
    const char *error = NULL;
    char *request = NULL;
    tny_team_op op = tny_team_parse_argv(argc, argv, input.data, input.len, &request, &error);
    buf_free(&input);
    if (op == TNY_TEAM_NONE) {
        fprintf(stderr, "tny: team: %s\n", error ? error : "invalid request");
        return 1;
    }
    yyjson_doc *doc = jparse(request, strlen(request));
    secure_free(request);
    tny_team_caller caller = {.local_operator = true};
    buf_t out = {0};
    char err[320] = "";
    int rc = doc ? tny_team_run(ctx, &caller, op, yyjson_doc_get_root(doc), &out, err, sizeof err,
                                NULL, NULL)
                 : 1;
    yyjson_doc_free(doc);
    if (out.len && fwrite(out.data, 1, out.len, stdout) != out.len) rc = 2;
    if (rc && err[0]) fprintf(stderr, "tny: team: %s\n", err);
    buf_free(&out);
    return rc;
}
