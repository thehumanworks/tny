/* cmd_ask.c — one noninteractive turn (docs/cli.md). Never blocks on an
 * approval: unresolved permissions end the run with exit 2. */
#include "cli/cli.h"
#include "core/backend.h"
#include "core/session.h"
#include "core/perm.h"
#include "core/runtime.h"
#include "core/tasks.h"
#include "backends/openai/openai.h"
#include "mcp/mcp.h"
#include "util/util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

static volatile sig_atomic_t g_interrupted = 0;
static void on_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}
static bool ask_cancel_probe(void *ud) {
    (void)ud;
    if (!g_interrupted) return false;
    g_interrupted = 0;
    return true;
}

/* connect() run off the main thread while stdin drains (safe per backend.h:
 * no terminal output, no ctx reads that anyone mutates meanwhile — same
 * contract the TUI pre-warm relies on, docs/adr/0002). Always joined before
 * rc/err are read. */
typedef struct {
    tny_backend *bk;
    char err[512];
    int rc;
} connect_job;

static void *connect_job_main(void *arg) {
    connect_job *j = arg;
    j->rc = j->bk->connect(j->bk, j->err, sizeof j->err);
    return NULL;
}

/* Early-exit cleanup once the connect thread (if any) has been joined:
 * never leak a spawned host process. */
static void abort_backend(tny_backend *bk, bool connected) {
    if (!bk) return;
    if (connected) bk->disconnect(bk);
    bk->destroy(bk);
}

typedef struct {
    buf_t output;
    bool json;
    bool turn_ended;
    tny_stop_reason stop;
    buf_t errline;
    buf_t host_tools;         /* TOOL_END log for host backends (JSON array items) */
    buf_t extension_messages; /* visible custom/user messages, JSON items */
    tny_engine *engine;
    tny_perm_mode perm_mode;
    bool print_usage;
} ask_state;

static void ask_event_cb(const tny_backend_event *ev, void *ud) {
    ask_state *st = ud;
    switch (ev->kind) {
    case TNY_EV_TEXT_DELTA:
        buf_append(&st->output, ev->text, ev->text_len);
        if (!st->json) {
            fwrite(ev->text, 1, ev->text_len, stdout);
            fflush(stdout);
        }
        break;
    case TNY_EV_THINKING: break; /* stderr noise in scripts; skip */
    case TNY_EV_TOOL_START:
        fprintf(stderr, "⏺ %s %.120s\n", ev->tool_name, ev->tool_detail ? ev->tool_detail : "");
        break;
    case TNY_EV_TOOL_END:
        fprintf(stderr, "  %s %s\n", ev->tool_ok ? "✓" : "✗", ev->tool_name);
        if (tny_engine_backend_id(st->engine) != TNY_BK_OPENAI && ev->tool_name) {
            if (st->host_tools.len) buf_appends(&st->host_tools, ",");
            buf_appends(&st->host_tools, "{\"name\":");
            jescape(&st->host_tools, ev->tool_name);
            buf_appendf(&st->host_tools, ",\"status\":\"%s\"}", ev->tool_ok ? "success" : "error");
        }
        break;
    case TNY_EV_TOOL_PROGRESS:
        fprintf(stderr, "  … %s %.120s\n", ev->tool_name ? ev->tool_name : "tool",
                ev->tool_detail ? ev->tool_detail : "");
        break;
    case TNY_EV_PERMISSION:
        /* `tny ask` never blocks on approvals: yolo allows, otherwise deny */
        if (st->perm_mode == TNY_MODE_YOLO) {
            fprintf(stderr, "auto-approving (yolo): %s\n",
                    ev->perm_summary ? ev->perm_summary : "");
            tny_engine_respond_permission(st->engine, ev->perm_id, TNY_PERM_DECISION_ALLOW);
        } else {
            fprintf(stderr, "denying (ask mode cannot approve): %s\n",
                    ev->perm_summary ? ev->perm_summary : "");
            tny_engine_respond_permission(st->engine, ev->perm_id, TNY_PERM_DECISION_DENY);
        }
        break;
    case TNY_EV_STATUS: fprintf(stderr, "%.*s\n", (int)ev->text_len, ev->text); break;
    case TNY_EV_CUSTOM_MESSAGE:
        if (ev->message_type)
            fprintf(stderr, "extension context (%s): %.*s\n", ev->message_type, (int)ev->text_len,
                    ev->text);
        else fprintf(stderr, "extension context: %.*s\n", (int)ev->text_len, ev->text);
        if (st->extension_messages.len) buf_appends(&st->extension_messages, ",");
        buf_appends(&st->extension_messages, "{\"kind\":\"custom\",\"custom_type\":");
        jescape(&st->extension_messages, ev->message_type ? ev->message_type : "tny_extension");
        buf_appends(&st->extension_messages, ",\"content\":");
        jescape(&st->extension_messages, ev->text ? ev->text : "");
        buf_appends(&st->extension_messages, "}");
        break;
    case TNY_EV_USER_MESSAGE:
        fprintf(stderr, "extension follow-up: %.*s\n", (int)ev->text_len, ev->text);
        if (st->extension_messages.len) buf_appends(&st->extension_messages, ",");
        buf_appends(&st->extension_messages, "{\"kind\":\"user\",\"content\":");
        jescape(&st->extension_messages, ev->text ? ev->text : "");
        buf_appends(&st->extension_messages, "}");
        break;
    case TNY_EV_STEER_REJECTED: /* ask never steers */ break;
    case TNY_EV_PLAN: fprintf(stderr, "plan: %.*s\n", (int)ev->text_len, ev->text); break;
    case TNY_EV_USAGE:
        if (!st->print_usage) break;
        /* streamed stdout may lack a trailing newline; finish that line so
         * the usage never glues onto the answer on a terminal */
        if (!st->json && st->output.len && st->output.data[st->output.len - 1] != '\n') {
            fputs("\n", stdout);
            fflush(stdout);
            buf_appends(&st->output, "\n");
        }
        if (ev->context_size > 0)
            fprintf(stderr, "context: %lld/%lld%s\n", (long long)ev->context_used,
                    (long long)ev->context_size, ev->has_cost ? " (cost reported)" : "");
        else
            fprintf(stderr, "tokens: %lld in, %lld out\n", (long long)ev->in_tokens,
                    (long long)ev->out_tokens);
        break;
    case TNY_EV_ERROR:
        buf_clear(&st->errline);
        buf_append(&st->errline, ev->text, ev->text_len);
        fprintf(stderr, "tny: %.*s\n", (int)ev->text_len, ev->text);
        break;
    case TNY_EV_TURN_END:
        st->turn_ended = true;
        st->stop = ev->stop;
        break;
    }
}

/* The foreground --json blob and the background session `result` are the
 * same bytes (docs/adr/0031 decision 3). engine may be NULL on background
 * failures before one existed. Malloc'd, includes the trailing newline. */
static char *ask_result_json(tny_ctx *ctx, ask_state *st, tny_engine *engine,
                             tny_session_state *session, int exit_code) {
    buf_t out;
    buf_init(&out);
    buf_appends(&out, "{\"output\":");
    jescape(&out, st->output.data ? st->output.data : "");
    buf_appendf(&out, ",\"exit_code\":%d,\"provider\":\"%s\",\"model\":", exit_code,
                tny_provider_name(ctx));
    jescape(&out, ctx->model ? ctx->model : "default");
    buf_appends(&out, ",\"session_id\":");
    jescape(&out, (ctx->no_save || !session) ? "" : session->id);
    buf_appendf(&out, ",\"ephemeral\":%s", ctx->no_save ? "true" : "false");
    buf_appends(&out, ",\"task\":");
    if (ctx->task_name) {
        buf_appends(&out, "{\"name\":");
        jescape(&out, ctx->task_name);
        buf_appends(&out, ",\"source\":");
        jescape(&out, ctx->task_source ? ctx->task_source : "unknown");
        buf_appends(&out, ",\"digest\":");
        jescape(&out, ctx->task_digest);
        buf_appends(&out, "}");
    } else buf_appends(&out, "null");
    int steps = engine ? tny_engine_openai_steps(engine) : 0;
    buf_appendf(&out, ",\"steps\":%d,\"tool_calls\":", steps);
    if (engine && tny_engine_backend_id(engine) == TNY_BK_OPENAI) {
        buf_appends(&out, tny_engine_openai_toolcalls_json(engine));
    } else {
        buf_appends(&out, "[");
        if (st->host_tools.len) buf_append(&out, st->host_tools.data, st->host_tools.len);
        buf_appends(&out, "]");
    }
    if (st->errline.len) {
        buf_appends(&out, ",\"error\":");
        jescape(&out, st->errline.data);
    }
    buf_appends(&out, ",\"extension_messages\":[");
    if (st->extension_messages.len)
        buf_append(&out, st->extension_messages.data, st->extension_messages.len);
    buf_appends(&out, "]");
    buf_appends(&out, "}\n");
    return buf_detach(&out);
}

/* Background child: every exit path after the fork records the terminal
 * status and result, then releases the writer lock (docs/adr/0031). */
static void bg_finalize(tny_ctx *ctx, tny_session_state *session, ask_state *st, tny_engine *engine,
                        int exit_code, const char *status) {
    char *result = ask_result_json(ctx, st, engine, session, exit_code);
    session_set_status_finished(session, status, exit_code, result);
    free(result);
    session_save(session);
    session_lock_release(session);
}

/* --output-schema VALUE: inline JSON when VALUE starts with '{', otherwise a
 * file path. Normalizes into ctx->output_schema (response_format JSON).
 * Returns 0 ok, -1 error (message already printed). */
static int load_output_schema(tny_ctx *ctx, const char *value) {
    if (ctx->backend != TNY_BK_OPENAI) {
        fprintf(stderr, "tny: --output-schema needs the openai-compatible provider "
                        "(structured outputs ride on response_format)\n"
                        "Example: tny --provider openai ask --output-schema schema.json \"…\"\n");
        return -1;
    }
    const char *text = value;
    size_t len = strlen(value);
    char *owned = NULL;
    while (*text == ' ' || *text == '\t') {
        text++;
        len--;
    }
    if (*text != '{') {
        owned = file_slurp(value, &len);
        if (!owned) {
            fprintf(stderr,
                    "tny: --output-schema %s: cannot read file\n"
                    "Example: tny ask --output-schema schema.json \"…\"\n",
                    value);
            return -1;
        }
        text = owned;
    }
    char *rf = tny_openai_response_format(text, len);
    free(owned);
    if (!rf) {
        fprintf(
            stderr,
            "tny: --output-schema: value is not a JSON object\n"
            "Example: tny ask --output-schema '{\"type\":\"object\",\"properties\":{}}' \"…\"\n");
        return -1;
    }
    free(ctx->output_schema);
    ctx->output_schema = rf;
    return 0;
}

int cmd_ask(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = g->json, use_stdin = false, ephemeral = ctx->no_save;
    bool continue_recovery = false, background = false;
    bool steer = false, steer_takeover = false;
    const char *usage_env = getenv("TNY_PRINT_USAGE");
    bool print_usage = usage_env && strcmp(usage_env, "1") == 0;
    const char *resume = g->resume;
    const char *output_schema = NULL;
    const char *images[17] = {0};
    int n_images = 0;
    buf_t prompt;
    buf_init(&prompt);

    int i = 0;
    bool raw = false;
    const char *task_name = NULL;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!raw && a[0] == '-' && a[1]) {
            if (strcmp(a, "--json") == 0) json = true;
            else if (strcmp(a, "--stdin") == 0) use_stdin = true;
            else if (strcmp(a, "--ephemeral") == 0 || strcmp(a, "--no-save") == 0) ephemeral = true;
            else if (strcmp(a, "--no-color") == 0) ctx->no_color = true;
            else if (strcmp(a, "--continue-recovery") == 0) continue_recovery = true;
            else if (strcmp(a, "--steer") == 0) steer = true;
            else if (strcmp(a, "--background") == 0 || strcmp(a, "-B") == 0) background = true;
            else if (strcmp(a, "--print-usage") == 0) print_usage = true;
            else if (strcmp(a, "--auto") == 0) ctx->perm_mode = TNY_MODE_AUTO;
            else if (strcmp(a, "--yolo") == 0) ctx->perm_mode = TNY_MODE_YOLO;
            else if (strcmp(a, "--task") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr,
                            "tny: ask: --task requires a value\n"
                            "Example: tny ask --task review \"inspect the current diff\"\n");
                    buf_free(&prompt);
                    return 1;
                }
                task_name = argv[++i];
            } else if (strcmp(a, "--resume") == 0 && i + 1 < argc) resume = argv[++i];
            else if (strcmp(a, "--resume-id") == 0 && i + 1 < argc) resume = argv[++i];
            else if (strcmp(a, "--output-schema") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "tny: --output-schema requires a value\n"
                                    "Example: tny ask --output-schema schema.json \"…\"\n");
                    buf_free(&prompt);
                    return 1;
                }
                output_schema = argv[++i];
            } else if (strcmp(a, "--image") == 0 && i + 1 < argc) {
                if (n_images < 16) images[n_images++] = argv[++i];
                else {
                    fprintf(stderr, "tny: too many --image flags (max 16)\n");
                    buf_free(&prompt);
                    return 1;
                }
            } else if (strcmp(a, "--") == 0) raw = true;
            else {
                fprintf(stderr, "tny: ask: unknown flag %s\nExample: tny ask --json \"hi\"\n", a);
                buf_free(&prompt);
                return 1;
            }
        } else {
            if (prompt.len) buf_appends(&prompt, " ");
            buf_appends(&prompt, a);
        }
    }
    if (task_name && tny_task_apply(ctx, task_name) != 0) {
        fprintf(stderr, "tny: unknown or invalid task '%s' (run `tny tasks` to list tasks)\n",
                task_name);
        buf_free(&prompt);
        return 1;
    }
    if (background && ephemeral) {
        fprintf(stderr, "tny: --background is incompatible with --ephemeral "
                        "(the printed id must point at a saved session)\n"
                        "Example: tny ask -B \"refactor the parser\"\n");
        buf_free(&prompt);
        return 1;
    }
#ifdef __EMSCRIPTEN__
    if (background) {
        fprintf(stderr, "tny: --background is not available in the browser build\n");
        buf_free(&prompt);
        return 1;
    }
#endif
    if (ephemeral && resume) {
        fprintf(stderr, "tny: --ephemeral is incompatible with --resume\n");
        buf_free(&prompt);
        return 1;
    }
    if (steer && !resume) {
        fprintf(stderr,
                "tny: --steer requires --resume\n"
                "Example: tny ask --resume last --steer \"drop that — check the tests instead\"\n");
        buf_free(&prompt);
        return 1;
    }
    if (ephemeral && continue_recovery) {
        fprintf(stderr, "tny: --ephemeral is incompatible with --continue-recovery\n");
        buf_free(&prompt);
        return 1;
    }
    if (output_schema && load_output_schema(ctx, output_schema) != 0) {
        buf_free(&prompt);
        return 1;
    }
    ctx->no_save = ephemeral;
    ctx->json_out = json;

    /* Resolve the session task snapshot before any provider connection is
     * started. This keeps every resume spelling provider-independent. */
    tny_session_state *session = resume ? session_open(ctx, resume) : session_new(ctx);
    if (!session) {
        if (resume) fprintf(stderr, "tny: no session '%s' for this workspace\n", resume);
        else fprintf(stderr, "tny: could not create a session\n");
        buf_free(&prompt);
        return 1;
    }
    if (resume) {
        char task_err[192];
        if (session_task_reconcile(session, task_err, sizeof task_err) != 0) {
            fprintf(stderr, "tny: cannot resume session %s: %s\n", session->id, task_err);
            session_close(session);
            buf_free(&prompt);
            return 1;
        }
    }

    /* Piped stdin can be slow (upstream producer): overlap the host connect
     * with the read. The argv-prompt path stays serial and untouched. */
    tny_backend *bk = NULL;
    connect_job job = {0};
    pthread_t connect_th;
    bool connecting = false;
    if (!prompt.len && (use_stdin || !isatty(0))) {
        /* background: the fork must precede any pthread_create, so skip the
         * overlap and connect serially in the child (docs/adr/0031). */
        if (!background) {
            bk = tny_backend_create((tny_backend_id)ctx->backend, ctx);
            if (bk) {
                job.bk = bk;
                if (pthread_create(&connect_th, NULL, connect_job_main, &job) == 0)
                    connecting = true;
                /* pthread_create failed: fall back to the serial connect below */
            }
        }
        char tmp[8192];
        size_t n;
        while ((n = fread(tmp, 1, sizeof tmp, stdin)) > 0) buf_append(&prompt, tmp, n);
        while (prompt.len &&
               (prompt.data[prompt.len - 1] == '\n' || prompt.data[prompt.len - 1] == '\r'))
            prompt.data[--prompt.len] = 0;
        if (connecting) pthread_join(connect_th, NULL);
    }
    if (!prompt.len) {
        fprintf(stderr,
                "tny: ask needs a prompt\nExample: tny ask \"summarize this repository\"\n");
        abort_backend(bk, connecting && job.rc == 0);
        session_close(session);
        buf_free(&prompt);
        return 1;
    }

    /* Writer lock (docs/adr/0031 decision 7): whoever runs a turn on a saved
     * session holds <dir>/lock, so resuming a live session fails loudly
     * instead of corrupting it. Released by session_close at the end (or
     * carried into the background child across the fork). */
    if (session && (background || resume)) {
        int lrc = session_lock_acquire(session);
        if (lrc != 0 && steer) {
            /* Interrupt-and-redirect (docs/adr/0031 decision 7a): stop the
             * running turn, take the lock, resume with the new prompt. With
             * -B this runs in the parent, before the fork. Steer never
             * SIGKILLs on its own. */
            char serr[256];
            int src = session_stop(ctx, session->id, false, serr, sizeof serr);
            if (src == 2) {
                fprintf(stderr,
                        "tny: session %s did not stop; try: "
                        "tny session stop %s --kill\n",
                        session->id, session->id);
                abort_backend(bk, connecting && job.rc == 0);
                session_close(session);
                buf_free(&prompt);
                return 2;
            }
            if (src < 0) {
                fprintf(stderr, "tny: %s\n", serr);
                abort_backend(bk, connecting && job.rc == 0);
                session_close(session);
                buf_free(&prompt);
                return 1;
            }
            /* Stopped (or it finished on its own). Reopen before resuming:
             * the child rewrote session.json while finalizing, and our
             * pre-stop doc would clobber its status/partial. Brief retry
             * for the now-freeing lock. */
            char *sid = xstrdup(session->id);
            session_close(session); /* held no lock: close is release-free */
            session = NULL;
            for (int t = 0; t < 20; t++) {
                session = session_open(ctx, sid);
                char task_err[192];
                if (session && session_task_reconcile(session, task_err, sizeof task_err) == 0 &&
                    (lrc = session_lock_acquire(session)) == 0)
                    break;
                if (session) {
                    session_close(session);
                    session = NULL;
                }
                lrc = -1;
                struct timespec ts = {0, 50L * 1000000L};
                nanosleep(&ts, NULL);
            }
            if (lrc == 0) {
                /* fold the interrupted partial into this resume via the
                 * --continue-recovery machinery; steer keeps the replay off
                 * stdout — the partial belongs to the abandoned turn */
                steer_takeover = true;
                continue_recovery = true;
            } else {
                cli_print_still_running(ctx, sid);
                free(sid);
                abort_backend(bk, connecting && job.rc == 0);
                session_close(session);
                buf_free(&prompt);
                return 1;
            }
            free(sid);
        }
        if (lrc != 0) {
            cli_print_still_running(ctx, session->id);
            abort_backend(bk, connecting && job.rc == 0);
            session_close(session);
            buf_free(&prompt);
            return 1;
        }
    }

    ask_state st = {0};
    buf_init(&st.output);
    buf_init(&st.errline);
    buf_init(&st.extension_messages);
    st.json = json && !background; /* child streams plain text into task.log */
    st.perm_mode = ctx->perm_mode;
    st.print_usage = print_usage;

#ifndef __EMSCRIPTEN__
    if (background) {
        /* Detach (docs/adr/0031 decision 4) — this ordering is load-bearing:
         * status save, flush, fork; the parent then only reports and exits. */
        session_set_status_running(session);
        if (session_save(session) != 0) {
            fprintf(stderr, "tny: cannot write session %s\n", session->id);
            session_close(session);
            buf_free(&prompt);
            buf_free(&st.output);
            buf_free(&st.errline);
            buf_free(&st.extension_messages);
            return 1;
        }
        fflush(NULL); /* buffered stdio must not replay into task.log */
        pid_t child = fork();
        if (child < 0) {
            fprintf(stderr, "tny: fork failed; cannot background\n");
            session_close(session);
            buf_free(&prompt);
            buf_free(&st.output);
            buf_free(&st.errline);
            buf_free(&st.extension_messages);
            return 1;
        }
        if (child > 0) {
            /* Parent: report the launch and _exit. No cleanup here — closing
             * or freeing could release the flock the child inherited (it
             * lives on the shared open file description). */
            if (json)
                printf("{\"kind\":\"ask_background\",\"session_id\":\"%s\","
                       "\"pid\":%d}\n",
                       session->id, (int)child);
            else printf("%s\n", session->id);
            fflush(stdout);
            _exit(0);
        }
        /* Child: the sole session writer from here on. */
        setsid(); /* own group: survives terminal close; `session stop`
                   * signals the group so spawned hosts die with us */
        session_write_pid(session, getpid());
        ctx->no_host_registry = true;            /* decision 8: invisible process must
                                                  * not become the attach target */
        if (!freopen("/dev/null", "r", stdin)) { /* best effort */
        }
        char *logf = path_join(session->dir, "task.log");
        if (logf) {
            if (!freopen(logf, "a", stdout)) { /* keep inherited stdout */
            }
            if (!freopen(logf, "a", stderr)) { /* keep inherited stderr */
            }
            free(logf);
        }
        setvbuf(stderr, NULL, _IONBF, 0); /* stderr stays unbuffered in the log */
    }
#endif

    if (continue_recovery && session) {
        char *rec = session_recovery_read(session);
        if (rec) {
            session_set_extension_start(session, "recovery", NULL);
            if (!json && !steer_takeover) {
                fputs(rec, stdout);
                fputs("\n", stdout);
            }
            session_recovery_clear(session);
            free(rec);
        }
    }

    /* backend (already created — and its connect already joined — on the
     * stdin path above) */
    if (!bk) bk = tny_backend_create((tny_backend_id)ctx->backend, ctx);
    if (!bk) {
        if (background) {
            buf_appends(&st.errline, "backend create failed");
            bg_finalize(ctx, session, &st, NULL, 1, "error");
        }
        buf_free(&prompt);
        session_close(session);
        buf_free(&st.output);
        buf_free(&st.errline);
        buf_free(&st.extension_messages);
        return 1;
    }
    char err[512];
    int crc;
    if (connecting) {
        crc = job.rc;
        if (crc != 0) snprintf(err, sizeof err, "%s", job.err);
    } else {
        crc = bk->connect(bk, err, sizeof err);
    }
    if (crc != 0) {
        fprintf(stderr, "tny: %s\n", err);
        if (background) {
            buf_appends(&st.errline, err);
            bg_finalize(ctx, session, &st, NULL, 1, "error");
        }
        bk->destroy(bk);
        session_close(session);
        buf_free(&prompt);
        buf_free(&st.output);
        buf_free(&st.errline);
        buf_free(&st.extension_messages);
        return 1;
    }
    perm_engine *perm = perm_new(ctx);
    tny_engine *engine = tny_engine_new(ctx, session, perm, NULL, NULL);
    if (!perm || !engine) {
        fprintf(stderr, "tny: out of memory\n");
        if (background) {
            buf_appends(&st.errline, "out of memory");
            bg_finalize(ctx, session, &st, engine, 1, "error");
        }
        if (engine) tny_engine_free(engine);
        else bk->destroy(bk);
        perm_free(perm);
        session_close(session);
        buf_free(&prompt);
        buf_free(&st.output);
        buf_free(&st.errline);
        buf_free(&st.extension_messages);
        return 1;
    }
    if (tny_engine_prepare(engine, bk, TNY_ENGINE_PREPARE_CONNECTED, err, sizeof err) != 0) {
        fprintf(stderr, "tny: %s\n", err);
        if (background) {
            buf_appends(&st.errline, err);
            bg_finalize(ctx, session, &st, engine, 1, "error");
        }
        tny_engine_free(engine);
        perm_free(perm);
        session_close(session);
        buf_free(&prompt);
        buf_free(&st.output);
        buf_free(&st.errline);
        buf_free(&st.extension_messages);
        return 1;
    }

    st.engine = engine;
    tny_engine_set_cancel_probe(engine, ask_cancel_probe, NULL);

    signal(SIGINT, on_sigint);
    signal(SIGPIPE, SIG_IGN);
    if (background) signal(SIGTERM, on_sigint); /* `session stop` cancels exactly like ^C */

    if (tny_engine_start(engine, prompt.data, n_images ? images : NULL, err, sizeof err) != 0) {
        fprintf(stderr, "tny: %s\n", err);
        if (background) {
            buf_appends(&st.errline, err);
            bg_finalize(ctx, session, &st, engine, 2, "error");
        }
        tny_engine_free(engine);
        perm_free(perm);
        session_close(session);
        buf_free(&prompt);
        buf_free(&st.output);
        buf_free(&st.errline);
        buf_free(&st.extension_messages);
        return 2;
    }

    int64_t last_ckpt = now_ms();
    while (!st.turn_ended) {
        if (g_interrupted) {
            g_interrupted = 0;
            tny_engine_cancel(engine);
            continue;
        }
        tny_owned_event *owned = NULL;
        tny_engine_next next = tny_engine_next_event(engine, 200, &owned, err, sizeof err);
        if (next == TNY_ENGINE_NEXT_EVENT) {
            int evk = owned->ev.kind;
            ask_event_cb(&owned->ev, &st);
            tny_owned_event_free(owned);
            /* background: checkpoint the partial answer so a crashed child
             * leaves something recoverable (~2 s cadence, docs/adr/0031) */
            if (background && evk == TNY_EV_TEXT_DELTA && now_ms() - last_ckpt >= 2000) {
                session_recovery_write(session, st.output.data ? st.output.data : "");
                last_ckpt = now_ms();
            }
        } else if (next == TNY_ENGINE_NEXT_TIMEOUT) {
            continue;
        } else if (next == TNY_ENGINE_NEXT_DRAINED) {
            break;
        } else if (!st.turn_ended) {
            fprintf(stderr, "tny: %s\n", err);
            st.turn_ended = true;
            st.stop = TNY_STOP_ERROR;
        }
    }

    int exit_code;
    switch (st.stop) {
    case TNY_STOP_DONE: exit_code = 0; break;
    case TNY_STOP_INTERRUPTED: exit_code = 130; break;
    default: exit_code = 2; break;
    }
    if (st.stop == TNY_STOP_DONE)
        tny_settings_remember_use(ctx); /* next launch defaults to this provider */

    const char *stname = st.stop == TNY_STOP_DONE          ? "done"
                         : st.stop == TNY_STOP_INTERRUPTED ? "interrupted"
                                                           : "error";
    if (background) {
        /* Child end: never print the --json blob (stdout is task.log; the
         * streamed text already went there). Record status + result instead. */
        if (st.stop == TNY_STOP_DONE) session_recovery_clear(session);
        bg_finalize(ctx, session, &st, engine, exit_code, stname);
        if (st.output.len && st.output.data[st.output.len - 1] != '\n') fputs("\n", stdout);
    } else {
        /* A foreground turn on a session that carries ADR-0031 status
         * fields (a resumed background session) must refresh them, or
         * `tny session <id>` keeps reporting the pre-resume status and
         * result. Legacy sessions (no status field) stay untouched. */
        if (session && !ctx->no_save && session_status(session)) {
            if (st.stop == TNY_STOP_DONE) session_recovery_clear(session);
            bg_finalize(ctx, session, &st, engine, exit_code, stname);
        }
        if (json) {
            char *out = ask_result_json(ctx, &st, engine, session, exit_code);
            fputs(out, stdout);
            free(out);
        } else if (st.output.len && st.output.data[st.output.len - 1] != '\n') {
            fputs("\n", stdout);
        }
    }

    mcp_shutdown_all();
    tny_engine_free(engine);
    perm_free(perm);
    session_close(session);
    buf_free(&prompt);
    buf_free(&st.output);
    buf_free(&st.errline);
    buf_free(&st.extension_messages);
    return exit_code;
}
