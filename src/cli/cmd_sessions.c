/* cmd_sessions.c — sessions list / session inspect / recover / resume entry. */
#include "cli/cli.h"
#include "core/session.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_sessions(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = g->json, all = false;
    int limit = 25;
    const char *cursor = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json = true;
        else if (strcmp(argv[i], "--all") == 0) all = true;
        else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) limit = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cursor") == 0 && i + 1 < argc) cursor = argv[++i];
    }
    if (limit < 1) limit = 1;
    if (limit > 100) limit = 100;
    int n = 0;
    session_meta *m = session_list(ctx, all, limit, cursor, &n);
    if (json) {
        buf_t b;
        buf_init(&b);
        buf_appends(&b, "{\"kind\":\"sessions\",\"sessions\":[");
        for (int i = 0; i < n; i++) {
            if (i) buf_appends(&b, ",");
            buf_appendf(&b, "{\"id\":\"%s\",\"turns\":%d,\"updated\":\"%s\","
                            "\"backend\":\"%s\",\"title\":",
                        m[i].id, m[i].turns, m[i].updated ? m[i].updated : "",
                        m[i].backend ? m[i].backend : "");
            jescape(&b, m[i].title ? m[i].title : "");
            buf_appends(&b, "}");
        }
        buf_appends(&b, "]}\n");
        fwrite(b.data, 1, b.len, stdout);
        buf_free(&b);
    } else {
        if (!n) printf("no saved sessions for %s\n", ctx->cwd);
        for (int i = 0; i < n; i++)
            printf("%s  %s  %2d turns  %-6s  %s\n", m[i].id,
                   m[i].updated ? m[i].updated : "                    ",
                   m[i].turns, m[i].backend ? m[i].backend : "?",
                   m[i].title ? m[i].title : "(untitled)");
    }
    session_meta_free(m, n);
    return 0;
}

int cmd_session(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = g->json;
    const char *id = NULL, *sub = NULL;
    if (argc >= 1 && (strcmp(argv[0], "recover") == 0 || strcmp(argv[0], "migrate") == 0)) {
        sub = argv[0];
        if (argc >= 2) id = argv[1];
    } else {
        for (int i = 0; i < argc; i++) {
            if (strcmp(argv[i], "--json") == 0) json = true;
            else if (!id) id = argv[i];
        }
    }
    if (!id) {
        fprintf(stderr, "tny: session <last|id>\nExample: tny session last\n");
        return 1;
    }
    if (sub && strcmp(sub, "recover") == 0) {
        char *nid = session_recover_copy(ctx, id);
        if (!nid) {
            fprintf(stderr, "tny: could not recover %s\n", id);
            return 2;
        }
        printf("recovered copy: %s\n", nid);
        free(nid);
        return 0;
    }
    if (sub && strcmp(sub, "migrate") == 0) {
        printf("sessions are already in the current format\n");
        return 0;
    }
    tny_session *s = session_open(ctx, id);
    if (!s) {
        fprintf(stderr, "tny: no session '%s' for this workspace\n"
                        "If session.json is corrupt: tny session recover %s\n", id, id);
        return 1;
    }
    if (json) {
        char *out = jwrite(s->doc);
        if (out) {
            fputs(out, stdout);
            fputs("\n", stdout);
            free(out);
        }
    } else {
        int64_t tin, tout;
        session_get_usage(s, &tin, &tout);
        printf("id:      %s\n", s->id);
        printf("title:   %s\n", session_title(s) ? session_title(s) : "(untitled)");
        printf("turns:   %d\n", session_turns(s));
        printf("tokens:  %lld in / %lld out\n", (long long)tin, (long long)tout);
        yyjson_mut_val *msgs = session_messages(s);
        size_t total = msgs ? yyjson_mut_arr_size(msgs) : 0;
        printf("messages: %zu\n", total);
        char *rec = session_recovery_read(s);
        if (rec) {
            printf("recoverable partial response: %zu bytes "
                   "(tny ask --resume %s --continue-recovery)\n", strlen(rec), s->id);
            free(rec);
        }
    }
    session_close(s);
    return 0;
}

int cmd_resume(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    const char *target = argc > 0 ? argv[0] : (g->resume ? g->resume : "last");
    if (g->resume_last) target = "last";
    tny_session *probe = session_open(ctx, target);
    if (!probe && !g->resume_picker) {
        fprintf(stderr, "tny: no session '%s' for this workspace (try `tny sessions`)\n",
                target);
        return 1;
    }
    char *id = probe ? xstrdup(probe->id) : NULL;
    if (probe) session_close(probe);
    int rc = cmd_tui_resume(ctx, g, id); /* tui owns interactive resume */
    free(id);
    return rc;
}
