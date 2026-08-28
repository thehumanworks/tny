/* cmd_sessions.c — sessions list / session inspect / recover / resume entry. */
#include "cli/cli.h"
#include "core/session.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cli_print_still_running(tny_ctx *ctx, const char *id) {
    pid_t p = session_read_pid(ctx, id);
    if (p > 0) fprintf(stderr, "tny: session %s is still running (pid %d)\n", id, (int)p);
    else fprintf(stderr, "tny: session %s is still running\n", id);
    fprintf(stderr,
            "  watch:     tny session %s\n"
            "  stop:      tny session stop %s\n"
            "  take over: tny ask --resume %s --steer \"new prompt\"\n",
            id, id, id);
}

/* One line, newlines flattened, truncated at a UTF-8 boundary. */
static void print_excerpt(const char *s, size_t max) {
    size_t len = strlen(s);
    size_t n = len;
    if (n > max) {
        n = max;
        while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--; /* boundary */
    }
    for (size_t i = 0; i < n; i++)
        putchar(s[i] == '\n' || s[i] == '\r' || s[i] == '\t' ? ' ' : s[i]);
    if (n < len) printf("… (%zu bytes)", len);
}

/* Print body text; guarantee exactly one trailing newline. */
static void print_block(const char *s) {
    size_t len = strlen(s);
    fwrite(s, 1, len, stdout);
    if (!len || s[len - 1] != '\n') printf("\n");
}

/* Readable transcript: full user/assistant text, one compact line per tool
 * call (⏺, mirroring the ask progress lines) and per tool result (✓). */
static void print_transcript(yyjson_mut_val *msgs) {
    size_t i, n;
    yyjson_mut_val *m;
    yyjson_mut_arr_foreach(msgs, i, n, m) {
        if (!m) break;
        const char *role = yyjson_mut_get_str(yyjson_mut_obj_get(m, "role"));
        const char *content = yyjson_mut_get_str(yyjson_mut_obj_get(m, "content"));
        if (!role) continue;
        if (strcmp(role, "tool") == 0) {
            printf("  ✓ ");
            print_excerpt(content ? content : "", 120);
            printf("\n");
            continue;
        }
        yyjson_mut_val *tcs = yyjson_mut_obj_get(m, "tool_calls");
        bool has_text = content && *content;
        if (has_text || !tcs) {
            printf("\n%s:\n", role);
            print_block(has_text ? content : "(empty)");
        }
        if (yyjson_mut_is_arr(tcs)) {
            if (!has_text) printf("\n%s:\n", role);
            size_t j, jn;
            yyjson_mut_val *tc;
            yyjson_mut_arr_foreach(tcs, j, jn, tc) {
                if (!tc) break;
                yyjson_mut_val *fn = yyjson_mut_obj_get(tc, "function");
                const char *name = fn ? yyjson_mut_get_str(yyjson_mut_obj_get(fn, "name")) : NULL;
                const char *args =
                    fn ? yyjson_mut_get_str(yyjson_mut_obj_get(fn, "arguments")) : NULL;
                printf("  ⏺ %s ", name ? name : "?");
                print_excerpt(args ? args : "", 100);
                printf("\n");
            }
        }
    }
}

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
            buf_appendf(&b,
                        "{\"id\":\"%s\",\"turns\":%d,\"updated\":\"%s\","
                        "\"backend\":\"%s\",\"title\":",
                        m[i].id, m[i].turns, m[i].updated ? m[i].updated : "",
                        m[i].backend ? m[i].backend : "");
            jescape(&b, m[i].title ? m[i].title : "");
            /* live probe wins; a stored "running" without a lock holder is
             * a crashed task → "stale" (docs/adr/0031 decision 5) */
            const char *st = m[i].running                                           ? "running"
                             : (m[i].status && strcmp(m[i].status, "running") == 0) ? "stale"
                                                                                    : m[i].status;
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
                               : (m[i].status && strcmp(m[i].status, "running") == 0) ? "  ⚠ stale"
                                                                                      : "";
            printf("%s  %s  %2d turns  %-6s  %s%s\n", m[i].id,
                   m[i].updated ? m[i].updated : "                    ", m[i].turns,
                   m[i].backend ? m[i].backend : "?", m[i].title ? m[i].title : "(untitled)", mark);
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
        if (!argv[i]) continue;
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
                   "\"status\":\"interrupted\"}\n",
                   s->id);
        else printf("session %s: interrupted\n", s->id);
        ret = 0;
    } else if (rc == 1) {
        /* clean no-op: nothing held the lock — report the final status */
        const char *st = session_status(s);
        if (json) {
            buf_t b;
            buf_init(&b);
            buf_appendf(&b,
                        "{\"kind\":\"session_stop\",\"session_id\":\"%s\","
                        "\"status\":",
                        s->id);
            if (st) jescape(&b, st);
            else buf_appends(&b, "null");
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
        fprintf(stderr,
                "tny: session %s did not stop; try: "
                "tny session stop %s --kill\n",
                s->id, s->id);
        ret = 2;
    } else {
        fprintf(stderr, "tny: %s\n", err);
    }
    session_close(s);
    return ret;
}

int cmd_session(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = g->json, wait = false;
    long timeout_s = -1;
    const char *id = NULL, *sub = NULL;
    if (argc >= 1 && (strcmp(argv[0], "recover") == 0 || strcmp(argv[0], "migrate") == 0 ||
                      strcmp(argv[0], "stop") == 0)) {
        sub = argv[0];
        if (argc >= 2) id = argv[1];
    } else {
        for (int i = 0; i < argc; i++) {
            if (strcmp(argv[i], "--json") == 0) json = true;
            else if (strcmp(argv[i], "--wait") == 0) wait = true;
            else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
                char *end = NULL;
                timeout_s = strtol(argv[++i], &end, 10);
                if (!end || *end || timeout_s < 0) {
                    fprintf(stderr, "tny: --timeout expects a non-negative number of seconds\n");
                    return 1;
                }
                wait = true;
            } else if (!id) id = argv[i];
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
    if (sub && strcmp(sub, "stop") == 0) return cmd_session_stop(ctx, json, argc - 1, argv + 1);
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
        fprintf(stderr,
                "tny: no session '%s' for this workspace\n"
                "If session.json is corrupt: tny session recover %s\n",
                id, id);
        return 1;
    }
    /* --wait (docs/adr/0041): block on the writer-lock probe — the same
     * liveness truth the status line uses — until the background turn
     * finalizes, then print the finished document. Polling the lock instead
     * of the pid is immune to PID reuse and needs no IPC into the child.
     * The wait goes through tny_poll (the one blocking seam, adr/0017). */
    bool timed_out = false;
    if (wait && session_is_running(ctx, s->id)) {
        char sid[32];
        snprintf(sid, sizeof sid, "%s", s->id);
        session_close(s);
        s = NULL;
        long waited_ms = 0;
        while (session_is_running(ctx, sid)) {
            if (timeout_s >= 0 && waited_ms >= timeout_s * 1000L) {
                timed_out = true;
                break;
            }
            tny_poll(NULL, 0, 200);
            waited_ms += 200;
        }
        s = session_open(ctx, sid);
        if (!s) {
            fprintf(stderr, "tny: session %s vanished while waiting\n", sid);
            return 2;
        }
    }
    int ret = 0;
    if (wait) {
        const char *st = session_status(s);
        yyjson_mut_val *ec = yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc), "exit_code");
        if (timed_out) {
            fprintf(stderr, "tny: session %s still running after %lds (--timeout)\n", s->id,
                    timeout_s);
            ret = 124;
        } else if (st && strcmp(st, "running") == 0) {
            ret = 2; /* stale: writer crashed without finalizing */
        } else if (ec) {
            ret = (int)yyjson_mut_get_int(ec);
        } else if (st && strcmp(st, "done") != 0) {
            ret = 2;
        }
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
            if (ec) printf("status:  %s (exit code %d)\n", st, (int)yyjson_mut_get_int(ec));
            else printf("status:  %s\n", st);
        }
        printf("turns:   %d\n", session_turns(s));
        printf("tokens:  %lld in / %lld out\n", (long long)tin, (long long)tout);
        yyjson_mut_val *msgs = session_messages(s);
        size_t total = msgs ? yyjson_mut_arr_size(msgs) : 0;
        printf("messages: %zu\n", total);
        if (total) print_transcript(msgs);
        /* Stored result: the read surface for host-backend answers (their
         * transcripts hold only a resume pointer). */
        if (!live && st && strcmp(st, "running") != 0) {
            yyjson_mut_val *root = yyjson_mut_doc_get_root(s->doc);
            yyjson_mut_val *res = yyjson_mut_obj_get(root, "result");
            const char *out = res ? yyjson_mut_get_str(yyjson_mut_obj_get(res, "output")) : NULL;
            if (out && *out) {
                printf("\nresult:\n");
                print_block(out);
            }
        }
        char *rec = session_recovery_read(s);
        if (rec) {
            if (live) printf("\npartial output (live, %zu bytes):\n", strlen(rec));
            else
                printf("\nrecoverable partial response (%zu bytes; resume: "
                       "tny ask --resume %s --continue-recovery):\n",
                       strlen(rec), s->id);
            print_block(rec);
            free(rec);
        } else if (live) {
            /* Nothing streamed yet: say so instead of leaving an empty
             * screen, and point at the child's progress log. */
            printf("\n(no output yet — partial text appears here as the "
                   "task streams; tool progress: %s/task.log)\n",
                   s->dir);
        }
    }
    session_close(s);
    return ret;
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
        fprintf(stderr, "tny: no session '%s' for this workspace (try `tny sessions`)\n", target);
        return 1;
    }
    if (probe && session_lock_acquire(probe) != 0) {
        /* another process (a background child or foreground resume) is
         * running a turn on this session (docs/adr/0031 decision 7) */
        cli_print_still_running(ctx, probe->id);
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
