/* cmd_status_ephemeral.c — status rendering with the active persistence mode.
 * Kept separate from cmd_misc.c so the ordinary status surface stays byte-for-
 * byte stable while ephemeral mode gains an explicit machine-readable field. */
#include "cli/cli.h"
#include "core/backend.h"
#include "core/session.h"
#include "util/util.h"

#include <stdio.h>
#include <string.h>

static bool status_wants_json(const cli_globals *g, int argc, char **argv) {
    if (g->json) return true;
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], "--json") == 0) return true;
    return false;
}

int cmd_status_ephemeral(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = status_wants_json(g, argc, argv);
    int n = 0;
    session_meta *m = session_list(ctx, false, 100, NULL, &n);
    session_meta_free(m, n);
    const char *bk = tny_provider_name(ctx);
    const char *model = ctx->model ? ctx->model : "default";
    bool auth = ctx->api_key != NULL || str_starts(ctx->base_url, "http://");
    if (json) {
        buf_t b;
        buf_init(&b);
        buf_appendf(&b,
                    "{\"kind\":\"status\",\"version\":\"%s\","
                    "\"backend\":\"%s\",\"model\":",
                    TNY_VERSION, bk);
        jescape(&b, model);
        if (ctx->reasoning_effort) {
            buf_appends(&b, ",\"reasoning_effort\":");
            jescape(&b, ctx->reasoning_effort);
        }
        buf_appendf(&b,
                    ",\"auth\":\"%s\",\"permission_mode\":\"%s\","
                    "\"sandbox\":\"%s\",\"ephemeral\":true,\"workspace\":",
                    auth ? "ok" : "missing", tny_perm_mode_name(ctx->perm_mode),
                    strcmp(ctx->sandbox_mode, "os") == 0 ? "os" : "none");
        jescape(&b, ctx->cwd);
        buf_appends(&b, ",\"task\":");
        if (ctx->task_name) {
            buf_appends(&b, "{\"name\":");
            jescape(&b, ctx->task_name);
            buf_appends(&b, ",\"source\":");
            jescape(&b, ctx->task_source ? ctx->task_source : "unknown");
            buf_appends(&b, ",\"digest\":");
            jescape(&b, ctx->task_digest);
            buf_appends(&b, "}");
        } else {
            buf_appends(&b, "null");
        }
        buf_appendf(&b,
                    ",\"sessions\":%d,\"agent_step_limit\":%d,"
                    "\"extensions_enabled\":%s,"
                    "\"extension_iteration_limit\":%d}\n",
                    n, ctx->max_steps, ctx->extensions_enabled ? "true" : "false",
                    ctx->max_extension_iterations);
        fwrite(b.data, 1, b.len, stdout);
        buf_free(&b);
    } else {
        printf("tny v%s\n", TNY_VERSION);
        printf("provider:   %s\n", bk);
        printf("model:      %s\n", model);
        if (ctx->reasoning_effort) printf("effort:     %s\n", ctx->reasoning_effort);
        printf("auth:       %s\n", auth ? "ok" : "missing (set OPENAI_API_KEY or run tny setup)");
        printf("permission: %s\n", tny_perm_mode_name(ctx->perm_mode));
        printf("sandbox:    %s\n",
               strcmp(ctx->sandbox_mode, "os") == 0 ? "os (unsupported: effective none)" : "none");
        printf("mode:       ephemeral (conversation artifacts stay in memory)\n");
        printf("workspace:  %s\n", ctx->cwd);
        if (ctx->task_name)
            printf("task:       %s (%s)\n", ctx->task_name,
                   ctx->task_source ? ctx->task_source : "unknown");
        else printf("task:       none\n");
        for (int i = 0; i < ctx->n_extra_dirs; i++) printf("extra dir:  %s\n", ctx->extra_dirs[i]);
        printf("sessions:   %d saved (not loaded by this process)\n", n);
        printf("extensions: %s (continuations: ", ctx->extensions_enabled ? "enabled" : "disabled");
        if (ctx->max_extension_iterations > 0) printf("max %d)\n", ctx->max_extension_iterations);
        else printf("unlimited)\n");
    }
    return 0;
}
