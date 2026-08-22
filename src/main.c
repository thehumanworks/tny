/* tny — fast, tiny coding-agent harness.
 * argv fast paths first: --version / --help must not touch the filesystem. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cli/cli.h"
#include "core/backend.h"

int main(int argc, char **argv) {
    /* fast paths: no allocation, no config */
    if (argc >= 2) {
        const char *a = argv[1];
        if (strcmp(a, "--version") == 0 || strcmp(a, "-v") == 0) {
            fputs(TNY_VERSION "\n", stdout);
            return 0;
        }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0 || strcmp(a, "help") == 0) {
            help_root();
            return 0;
        }
    }

    cli_globals g = {0};
    int ci = cli_parse_globals(argc, argv, &g);
    if (ci < 0) return 1;

    const char *cmd = ci < argc ? argv[ci] : NULL;
    int cargc = cmd ? argc - ci - 1 : 0;
    char **cargv = cmd ? argv + ci + 1 : NULL;

    /* per-command --help without loading config */
    if (cmd && cargc >= 1 &&
        (strcmp(cargv[0], "--help") == 0 || strcmp(cargv[0], "-h") == 0)) {
        if (help_for(cmd)) return 0;
    }
    if (cmd && strcmp(cmd, "--version") == 0) { fputs(TNY_VERSION "\n", stdout); return 0; }

    tny_ctx *ctx = cli_make_ctx(&g);
    if (!ctx) return 1;

    int rc;
    if (!cmd) {
        if (g.resume_picker || g.resume_last || g.resume)
            rc = cmd_resume(ctx, &g, 0, NULL);
        else
            rc = cmd_tui(ctx, &g);
    } else if (strcmp(cmd, "ask") == 0) {
        rc = cmd_ask(ctx, &g, cargc, cargv);
    } else if (strcmp(cmd, "resume") == 0) {
        rc = cmd_resume(ctx, &g, cargc, cargv);
    } else if (strcmp(cmd, "acp") == 0) {
        rc = cmd_acp_server(ctx, &g, cargc, cargv);
    } else if (strcmp(cmd, "sessions") == 0) {
        rc = cmd_sessions(ctx, &g, cargc, cargv);
    } else if (strcmp(cmd, "session") == 0) {
        rc = cmd_session(ctx, &g, cargc, cargv);
    } else if (strcmp(cmd, "providers") == 0 || strcmp(cmd, "backends") == 0) {
        rc = cmd_backends(ctx, &g, cargc, cargv);
    } else if (strcmp(cmd, "models") == 0) {
        rc = cmd_models(ctx, &g, cargc, cargv);
    } else if (strcmp(cmd, "permissions") == 0) {
        rc = cmd_permissions(ctx, &g, cargc, cargv);
    } else if (strcmp(cmd, "workspace") == 0) {
        rc = cmd_workspace(ctx, &g, cargc, cargv);
    } else if (strcmp(cmd, "status") == 0) {
        rc = cmd_status(ctx, &g, cargc, cargv);
    } else if (strcmp(cmd, "doctor") == 0) {
        rc = cmd_doctor(ctx, &g, cargc, cargv);
    } else if (strcmp(cmd, "usage") == 0) {
        rc = cmd_usage(ctx, &g, cargc, cargv);
    } else if (strcmp(cmd, "login") == 0) {
        rc = cmd_login(ctx, &g, cargc, cargv);
    } else if (strcmp(cmd, "logout") == 0) {
        rc = cmd_logout(ctx, &g, cargc, cargv);
    } else if (strcmp(cmd, "setup") == 0) {
        rc = cmd_setup(ctx, &g, cargc, cargv);
    } else {
        fprintf(stderr, "tny: unknown command '%s'\n\n", cmd);
        help_root();
        rc = 1;
    }

    tny_ctx_free(ctx);
    free(g.add_dirs);
    free(g.agent_argv);
#ifdef __EMSCRIPTEN__
    /* an Asyncified main's return value is dropped after an unwind; only an
     * explicit exit() carries the code to the host (docs/adr/0017) */
    exit(rc);
#endif
    return rc;
}
