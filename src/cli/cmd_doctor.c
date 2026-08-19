/* cmd_doctor.c — local health and preflight checks. May spawn host probes. */
#include "cli/cli.h"
#include "core/backend.h"
#include "core/session.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

static bool on_path(const char *bin) {
    if (strchr(bin, '/')) return access(bin, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) return false;
    char *dup = xstrdup(path);
    bool found = false;
    for (char *p = strtok(dup, ":"); p && !found; p = strtok(NULL, ":")) {
        char *full = path_join(p, bin);
        if (access(full, X_OK) == 0) found = true;
        free(full);
    }
    free(dup);
    return found;
}

static const char *ACP_AGENTS[] = {
    "gemini", "claude-agent-acp", "agent", "goose", "opencode", "copilot", NULL
};

int cmd_doctor(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = g->json;
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], "--json") == 0) json = true;

    struct utsname un;
    uname(&un);

    /* backend one-liners */
    char lines[TNY_BK_COUNT][256];
    int health[TNY_BK_COUNT];
    for (int i = 0; i < TNY_BK_COUNT; i++) {
        snprintf(lines[i], sizeof lines[i], "no probe");
        health[i] = 1;
        tny_backend *bk = tny_backend_create((tny_backend_id)i, ctx);
        if (bk && bk->doctor) health[i] = bk->doctor(ctx, lines[i], sizeof lines[i]);
        if (bk) bk->destroy(bk);
    }

    bool bridge = on_path(ctx->bridge_bin);
    bool codex = on_path(ctx->codex_bin);
    bool settings_ok = !file_exists(ctx->settings_path) || ctx->settings != NULL;
    int n = 0;
    session_meta *m = session_list(ctx, false, 100, NULL, &n);
    session_meta_free(m, n);

    buf_t acp_found;
    buf_init(&acp_found);
    for (int i = 0; ACP_AGENTS[i]; i++)
        if (on_path(ACP_AGENTS[i])) {
            if (acp_found.len) buf_appends(&acp_found, ", ");
            buf_appends(&acp_found, ACP_AGENTS[i]);
        }

    if (json) {
        buf_t b;
        buf_init(&b);
        buf_appendf(&b, "{\"kind\":\"doctor\",\"version\":\"%s\",\"os\":\"%s\","
                        "\"arch\":\"%s\",", TNY_VERSION, un.sysname, un.machine);
        buf_appendf(&b, "\"settings_ok\":%s,\"sessions\":%d,",
                    settings_ok ? "true" : "false", n);
        buf_appendf(&b, "\"sandbox\":\"none\",\"sandbox_note\":\"os sandbox not "
                        "implemented in this build; commands run unsandboxed\",");
        buf_appendf(&b, "\"hosts\":{\"cursor_sdk_bridge\":%s,\"codex\":%s,\"acp_agents\":",
                    bridge ? "true" : "false", codex ? "true" : "false");
        jescape(&b, acp_found.len ? acp_found.data : "");
        buf_appends(&b, "},\"backends\":[");
        for (int i = 0; i < TNY_BK_COUNT; i++) {
            if (i) buf_appends(&b, ",");
            buf_appendf(&b, "{\"name\":\"%s\",\"healthy\":%s,\"detail\":",
                        tny_backend_name((tny_backend_id)i),
                        health[i] == 0 ? "true" : "false");
            jescape(&b, lines[i]);
            buf_appends(&b, "}");
        }
        buf_appends(&b, "]}\n");
        fwrite(b.data, 1, b.len, stdout);
        buf_free(&b);
    } else {
        printf("tny v%s on %s %s\n\n", TNY_VERSION, un.sysname, un.machine);
        printf("%s settings: %s\n", settings_ok ? "ok " : "FAIL",
               file_exists(ctx->settings_path) ? ctx->settings_path : "(none yet)");
        printf("ok  sessions: %d in this workspace\n", n);
        printf("note sandbox: os sandbox not implemented in this build; "
               "approved commands run unsandboxed\n");
        printf("%s cursor-sdk-bridge: %s\n", bridge ? "ok " : "miss",
               bridge ? ctx->bridge_bin : "not on PATH (set CURSOR_SDK_BRIDGE_BIN)");
        printf("%s codex: %s\n", codex ? "ok " : "miss",
               codex ? ctx->codex_bin : "not on PATH");
        printf("%s ACP agents: %s\n", acp_found.len ? "ok " : "miss",
               acp_found.len ? acp_found.data : "none detected");
        printf("\nbackends:\n");
        for (int i = 0; i < TNY_BK_COUNT; i++)
            printf("  %s %s\n", health[i] == 0 ? "ok " : "warn", lines[i]);
    }
    buf_free(&acp_found);
    return 0;
}
