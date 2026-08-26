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
            /* live probe wins; a stored "running" without a lock holder is
             * a crashed task → "stale" (docs/adr/0031 decision 5) */
            const char *st = m[i].running ? "running"
                : (m[i].status && strcmp(m[i].status, "running") == 0)
                    ? "stale" : m[i].status;
            if (st) {
                buf_appends(&b, ",\"status\":");
                jescape(&b, st);
            }
            buf_appends(&b, "}");
        }
        buf_appends(&b, "]}\n");
        fwrite(b.data, 1, b.len, stdout);
        buf_free(&b);
    } else {
        if (!n) printf("no saved sessions for %s\n", ctx->cwd);
        for (int i = 0; i < n; i++) {
            const char *mark = m[i].running ? "  ⏵ running"
                : (m[i].status && strcmp(m[i].status, "running") == 0)
                    ? "  ⚠ stale" : "";
            printf("%s  %s  %2d turns  %-6s  %s%s\n", m[i].id,
                   m[i].updated ? m[i].updated : "                    ",
                   m[i].turns, m[i].backend ? m[i].backend : "?",
                   m[i].title ? m[i].title : "(untitled)", mark);
        }
    }
    session_meta_free(m, n);
    return 0;
}

/* `tny session stop <id> [--kill] [--json]` (docs/adr/0031 decision 6). */
static int cmd_session_stop(tny_ctx *ctx, bool json, int argc, char **argv) {
    bool force_kill = false;
    const char *id = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--kill") == 0) force_kill = true;
        else if (strcmp(argv[i], "--json") == 0) json = true;
        else if (!id) id = argv[i];
    }
    if (!id) {
        fprintf(stderr, "tny: session stop <id> [--kill]\n"
                        "Example: tny session stop 4f2a1c90aa317b22\n");
        return 1;
    }
    tny_session_state *s = session_open(ctx, id);
    if (!s) {
        fprintf(stderr, "tny: no session '%s' for this workspace\n", id);
        return 1;
    }
    char err[256];
    int rc = session_stop(ctx, s->id, force_kill, err, sizeof err);
    int ret = 1;
    if (rc == 0) {
        if (json)
            printf("{\"kind\":\"session_stop\",\"session_id\":\"%s\","
                   "\"status\":\"interrupted\"}\n", s->id);
        else
            printf("session %s: interrupted\n", s->id);
        ret = 0;
    } else if (rc == 1) {
        /* clean no-op: nothing held the lock — report the final status */
        const char *st = session_status(s);
        if (json) {
            buf_t b;
            buf_init(&b);
            buf_appendf(&b, "{\"kind\":\"session_stop\",\"session_id\":\"%s\","
                            "\"status\":", s->id);
            if (st) jescape(&b, st); else buf_appends(&b, "null");
            buf_appends(&b, "}\n");
            fwrite(b.data, 1, b.len, stdout);
            buf_free(&b);
        } else if (st) {
            printf("session %s is not running (status: %s)\n", s->id, st);
        } else {
            printf("session %s is not running\n", s->id);
        }
        ret = 0;
    } else if (rc == 2) {
        fprintf(stderr, "tny: session %s did not stop; try: "
                        "tny session stop %s --kill\n", s->id, s->id);
        ret = 2;
    } else {
        fprintf(stderr, "tny: %s\n", err);
    }
    session_close(s);
    return ret;
}

int cmd_session(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = g->json;
    const char *id = NULL, *sub = NULL;
    if (argc >= 1 && (strcmp(argv[0], "recover") == 0 || strcmp(argv[0], "migrate") == 0 ||
                      strcmp(argv[0], "stop") == 0)) {
        sub = argv[0];
        if (argc >= 2) id = argv[1];
    } else {
        for (int i = 0; i < argc; i++) {
            if (strcmp(argv[i], "--json") == 0) json = true;
            else if (!id) id = argv[i];
        }
    }
    if (!id && !(sub && strcmp(sub, "stop") == 0)) {
        fprintf(stderr, "tny: session <last|id>\nExample: tny session last\n");
        return 1;
    }
    if (ctx->no_save) {
        fprintf(stderr, "tny: --ephemeral cannot open, migrate, or recover saved sessions\n");
        return 1;
    }
    if (sub && strcmp(sub, "stop") == 0)
        return cmd_session_stop(ctx, json, argc - 1, argv + 1);
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
    tny_session_state *s = session_open(ctx, id);
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
        /* Background status (docs/adr/0031): the lock probe is the liveness
         * truth; a stored "running" without a holder is a crashed task. */
        const char *st = session_status(s);
        bool live = session_is_running(ctx, s->id);
        if (live) {
            pid_t p = session_read_pid(ctx, s->id);
            if (p > 0) printf("status:  running (pid %d)\n", (int)p);
            else printf("status:  running\n");
        } else if (st && strcmp(st, "running") == 0) {
            printf("status:  running (stale — process gone)\n");
        } else if (st) {
            yyjson_mut_val *root = yyjson_mut_doc_get_root(s->doc);
            yyjson_mut_val *ec = yyjson_mut_obj_get(root, "exit_code");
            if (ec)
                printf("status:  %s (exit code %d)\n", st,
                       (int)yyjson_mut_get_int(ec));
            else
                printf("status:  %s\n", st);
        }
        printf("turns:   %d\n", session_turns(s));
        printf("tokens:  %lld in / %lld out\n", (long long)tin, (long long)tout);
        yyjson_mut_val *msgs = session_messages(s);
        size_t total = msgs ? yyjson_mut_arr_size(msgs) : 0;
        printf("messages: %zu\n", total);
        /* Stored result: the read surface for host-backend answers (their
         * transcripts hold only a resume pointer). */
        if (!live && st && strcmp(st, "running") != 0) {
            yyjson_mut_val *root = yyjson_mut_doc_get_root(s->doc);
            yyjson_mut_val *res = yyjson_mut_obj_get(root, "result");
            const char *out = res
                ? yyjson_mut_get_str(yyjson_mut_obj_get(res, "output")) : NULL;
            if (out && *out) {
                printf("result:\n%s", out);
                if (out[strlen(out) - 1] != '\n') printf("\n");
            }
        }
        char *rec = session_recovery_read(s);
        if (rec) {
            if (live)
                printf("partial output: %zu bytes (live)\n", strlen(rec));
            else
                printf("recoverable partial response: %zu bytes "
                       "(tny ask --resume %s --continue-recovery)\n",
                       strlen(rec), s->id);
            free(rec);
        }
    }
    session_close(s);
    return 0;
}

int cmd_resume(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    if (ctx->no_save) {
        fprintf(stderr, "tny: --ephemeral cannot resume a saved session\n");
        return 1;
    }
    const char *target = argc > 0 ? argv[0] : (g->resume ? g->resume : "last");
    if (g->resume_last) target = "last";
    tny_session_state *probe = session_open(ctx, target);
    if (!probe && !g->resume_picker) {
        fprintf(stderr, "tny: no session '%s' for this workspace (try `tny sessions`)\n",
                target);
        return 1;
    }
    if (probe && session_lock_acquire(probe) != 0) {
        /* another process (a background child or foreground resume) is
         * running a turn on this session (docs/adr/0031 decision 7) */
        pid_t p = session_read_pid(ctx, probe->id);
        if (p > 0)
            fprintf(stderr, "tny: session %s is still running (pid %d)\n",
                    probe->id, (int)p);
        else
            fprintf(stderr, "tny: session %s is still running\n", probe->id);
        session_close(probe);
        return 1;
    }
    char *id = probe ? xstrdup(probe->id) : NULL;
    /* Keep the probe open on purpose: the TUI does its own session_open and
     * never sees this state, and closing it would release the writer lock
     * mid-resume. One leaked state for a process-lifetime lock is the least
     * invasive correct option. */
    int rc = cmd_tui_resume(ctx, g, id); /* tui owns interactive resume */
    free(id);
    return rc;
}
