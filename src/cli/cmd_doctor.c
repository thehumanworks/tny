/* cmd_doctor.c — local health and preflight checks. May spawn host probes. */
#include "cli/cli.h"
#include "core/backend.h"
#include "core/extension_caps.h"
#include "core/extensions.h"
#include "core/sandbox.h"
#include "core/session.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

#ifndef __EMSCRIPTEN__
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
#endif

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

    bool codex = ctx->chatgpt_token || tny_codex_auth_present(); /* a ChatGPT credential */
    bool settings_ok = !file_exists(ctx->settings_path) || ctx->settings != NULL;
#ifdef __EMSCRIPTEN__
    bool python = false; /* no process/Python authority in wasm (ADR 0017/0028) */
#else
    bool python = on_path("python3");
#endif
    char *capabilities = tny_extension_capabilities_json((tny_backend_id)ctx->backend,
                                                         ctx->extensions_enabled, python);
    size_t extension_entries = 0;
    if (ctx->extensions_enabled) extension_entries = tny_extensions_entry_count(ctx->extensions);
    int n = 0;
    session_meta *m = session_list(ctx, false, 100, NULL, &n);
    session_meta_free(m, n);
    tny_sandbox_kind sandbox = tny_sandbox_effective(ctx);
    tny_sandbox_kind sandbox_available = tny_sandbox_available();
    const char *sandbox_note;
    if (ctx->perm_mode == TNY_MODE_YOLO)
        sandbox_note = "permission mode yolo disables the terminal sandbox";
    else if (sandbox != TNY_SANDBOX_NONE)
        sandbox_note = sandbox == TNY_SANDBOX_SEATBELT
                           ? "terminal children use macOS Seatbelt; outbound network remains open"
                           : "terminal children use Linux bubblewrap; network namespace is shared";
    else if (ctx->sandbox_mode && strcmp(ctx->sandbox_mode, "os") == 0)
        sandbox_note = "os sandbox requested but no supported wrapper is available";
    else if (ctx->sandbox_mode && strcmp(ctx->sandbox_mode, "auto") == 0 &&
             sandbox_available == TNY_SANDBOX_NONE)
        sandbox_note = "auto resolved to none; no supported wrapper is available";
    else sandbox_note = "terminal commands run without a tny OS wrapper";

    if (json) {
        buf_t b;
        buf_init(&b);
        buf_appendf(&b,
                    "{\"kind\":\"doctor\",\"version\":\"%s\",\"os\":\"%s\","
                    "\"arch\":\"%s\",",
                    TNY_VERSION, un.sysname, un.machine);
        buf_appendf(&b, "\"settings_ok\":%s,\"sessions\":%d,", settings_ok ? "true" : "false", n);
        buf_appendf(&b,
                    "\"extensions\":{\"enabled\":%s,\"entries\":%zu,"
                    "\"python3\":%s,\"capabilities\":",
                    ctx->extensions_enabled ? "true" : "false", extension_entries,
                    python ? "true" : "false");
        buf_appends(&b, capabilities ? capabilities : "{}");
        buf_appends(&b, "},");
        buf_appendf(&b, "\"tools\":\"%s\",\"sandbox\":\"%s\",\"sandbox_note\":",
                    tny_tool_profile_name(ctx->tool_profile), tny_sandbox_kind_name(sandbox));
        jescape(&b, sandbox_note);
        buf_appendf(&b, ",\"self_improve\":%s,", ctx->no_self_improve ? "false" : "true");
        buf_appends(&b, "\"providers\":[");
        for (int i = 0; i < TNY_BK_COUNT; i++) {
            if (i) buf_appends(&b, ",");
            buf_appendf(&b, "{\"name\":\"%s\",\"healthy\":%s,\"detail\":",
                        tny_backend_name((tny_backend_id)i), health[i] == 0 ? "true" : "false");
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
        if (!ctx->extensions_enabled) printf("off python extensions: disabled\n");
        else if (extension_entries && !python)
            printf("warn python extensions: %zu found, python3 is not on PATH\n",
                   extension_entries);
        else
            printf("ok  python extensions: %zu found%s\n", extension_entries,
                   extension_entries ? ", python3 available" : "");
        printf("%s sandbox: %s (%s)\n", sandbox == TNY_SANDBOX_NONE ? "note" : "ok ",
               tny_sandbox_kind_name(sandbox), sandbox_note);
        printf("ok  tools: %s\n", tny_tool_profile_name(ctx->tool_profile));
        printf("%s automatic workflow learning\n", ctx->no_self_improve ? "off" : "on ");
        printf("%s codex: %s\n", codex ? "ok " : "miss",
               codex ? "ChatGPT credential found (flag, env, ~/.tny/codex-auth.json, or "
                       "$CODEX_HOME/auth.json)"
                     : "no ChatGPT credential (run `tny --provider codex login`)");
        printf("\nproviders:\n");
        for (int i = 0; i < TNY_BK_COUNT; i++)
            printf("  %s %s\n", health[i] == 0 ? "ok " : "warn", lines[i]);
    }
    free(capabilities);
    return 0;
}
