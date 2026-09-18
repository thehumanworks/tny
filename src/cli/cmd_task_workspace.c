/* Standalone CLI adapter; dispatch registration belongs to the lead. */
#include "cli/cli.h"
#include "core/tools_workspace.h"
#include "core/perm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Until the lead adds the declaration to cli.h. */
int cmd_task_workspace(tny_ctx *ctx, const cli_globals *g, int argc, char **argv);
int cmd_task_workspace(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    const char *usage = "Usage: tny task-workspace inspect|integrate|cleanup --run ID --task N "
                        "--attempt N [--json]\nPreparation is scheduler-only. No automatic "
                        "integration or cleanup. Verification remains unverified.\n";
    if (argc == 1 && (strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0)) {
        fputs(usage, stdout);
        return 0;
    }
    char *request = NULL;
    bool json = false;
    const char *error = NULL;
    tny_workspace_op op = tny_workspace_parse_argv(argc, argv, &request, &json, &error);
    if (op == TNY_WORKSPACE_NONE) {
        fputs(usage, stderr);
        return 1;
    }
    yyjson_doc *doc = jparse(request, strlen(request));
    free(request);
    yyjson_val *args = doc ? yyjson_doc_get_root(doc) : NULL;
    char *detail = tny_workspace_detail(ctx, op, args, &error);
    perm_engine *perm = detail ? perm_new(ctx) : NULL;
    int rc = 1;
    if (!detail || !perm) {
        fprintf(stderr, "tny: task-workspace: %s\n", error ? error : "cannot check permission");
    } else if (perm_check(perm, tny_workspace_permission_tool(op), detail) != PERM_ALLOW) {
        fputs("tny: task-workspace: operation requires its own permission grant\n", stderr);
        rc = 2;
    } else {
        buf_t out = {0}, human = {0};
        char err[512] = "";
        rc = tny_workspace_run(ctx, op, args, &out, err, sizeof err);
        if (json || (g && g->json)) {
            if (out.len) fwrite(out.data, 1, out.len, stdout);
        } else {
            tny_workspace_render_human(out.data, &human);
            if (human.len) fwrite(human.data, 1, human.len, stdout);
        }
        if (err[0]) fprintf(stderr, "tny: task-workspace: %s\n", err);
        buf_free(&out);
        buf_free(&human);
    }
    perm_free(perm);
    free(detail);
    if (doc) yyjson_doc_free(doc);
    return rc;
}
