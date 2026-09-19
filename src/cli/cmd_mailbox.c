/* Explicit local-operator mailbox surface. Nested members use private identity. */
#include "cli/cli.h"
#include "core/team_runtime.h"
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t mailbox_stop;
static void mailbox_signal(int signum) {
    (void)signum;
    mailbox_stop = 1;
}
static bool mailbox_is_cancelled(void *userdata) {
    (void)userdata;
    return mailbox_stop != 0;
}

int cmd_mailbox(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    (void)g;
    char err[320] = "";
    char *request = tny_team_mailbox_parse_argv(argc, argv, err, sizeof err);
    if (!request) {
        fprintf(stderr, "tny: %s\n", err);
        return 1;
    }
    yyjson_doc *doc = jparse(request, strlen(request));
    struct sigaction action = {0}, prior_int, prior_term;
    action.sa_handler = mailbox_signal;
    sigemptyset(&action.sa_mask);
    mailbox_stop = 0;
    if (sigaction(SIGINT, &action, &prior_int) || sigaction(SIGTERM, &action, &prior_term)) {
        yyjson_doc_free(doc);
        free(request);
        return 1;
    }
    tools_env env = {.ctx = ctx, .cancelled = mailbox_is_cancelled};
    buf_t out;
    buf_init(&out);
    int rc =
        doc ? tny_team_mailbox_run(&env, yyjson_doc_get_root(doc), true, &out, err, sizeof err) : 1;
    if (out.len) {
        fwrite(out.data, 1, out.len, stdout);
        fputc('\n', stdout);
    }
    if (rc) fprintf(stderr, "tny: mailbox: %s\n", err[0] ? err : "invalid request");
    buf_free(&out);
    yyjson_doc_free(doc);
    free(request);
    sigaction(SIGINT, &prior_int, NULL);
    sigaction(SIGTERM, &prior_term, NULL);
    return rc;
}
