/* cmd_ask.c — one noninteractive turn (docs/cli.md). Never blocks on an
 * approval: unresolved permissions end the run with exit 2. On native
 * builds the turn executes in a detached session runner (docs/adr/0053);
 * this process is only the renderer, so a crashed or killed caller never
 * takes the agent with it. wasm and TNY_ISOLATE=0 keep the in-process
 * path. */
#include "cli/cli.h"
#include "core/backend.h"
#include "core/event_jsonl.h"
#include "core/session.h"
#include "core/perm.h"
#include "core/runner.h"
#include "core/runtime.h"
#include "core/tasks.h"
#include "backends/openai/openai.h"
#include "mcp/mcp.h"
#include "util/tny_poll.h"
#include "util/process.h"
#include "util/util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

static volatile sig_atomic_t g_interrupted = 0, g_terminate = 0;
static void on_sigint(int sig) {
    if (sig == SIGHUP || sig == SIGTERM) g_terminate = sig;
    g_interrupted = 1;
}
static bool ask_cancel_probe(void *ud) {
    (void)ud;
    /* A job item's supervisor is its actual parent process: if this turn was
     * started by one and getppid() no longer reports it, the work is orphaned
     * and must stop at the same bounded points a ^C would (docs/adr/0093).
     * Ordinary runs have no expected parent and never consult the kernel for
     * one; no pid is ever signalled from a record. */
    if (tny_process_parent_lost()) return true;
    if (!g_interrupted) return false;
    g_interrupted = 0;
    return true;
}
/* Read-only companion for the event writer: a stalled consumer must not
 * swallow the interrupt the main loop still has to act on. */
static bool ask_interrupt_peek(void *ud) {
    (void)ud;
    return g_interrupted != 0;
}

/* One stable machine diagnostic (docs/cli.md). Machine mode gets a JSON
 * object on stderr — never an engine event, never on the event stream. */
static void ask_diag(bool machine, const char *code, const char *message, const char *example) {
    if (!machine) {
        fprintf(stderr, "tny: %s\n", message);
        if (example) fprintf(stderr, "Example: %s\n", example);
        return;
    }
    buf_t line;
    buf_init(&line);
    buf_appends(&line, "{\"schema_version\":1,\"kind\":\"ask_error\",\"code\":");
    jescape(&line, code);
    buf_appends(&line, ",\"message\":");
    jescape(&line, message);
    buf_appends(&line, "}\n");
    if (line.data) fwrite(line.data, 1, line.len, stderr);
    fflush(stderr);
    buf_free(&line);
}

/* connect() run off the main thread while stdin drains (safe per backend.h:
 * no terminal output, no ctx reads that anyone mutates meanwhile — same
 * contract the TUI pre-warm relies on, docs/adr/0002). Always joined before
 * rc/err are read. In-process path only; the isolated path overlaps by
 * letting the runner connect while this process reads stdin. */
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
    buf_t output; /* raw reply, as streamed: --json and the session keep it */
    bool json;
    bool text_seen; /* stdout: leading whitespace of the reply is dropped */
    bool any_out;   /* wrote answer bytes to stdout */
    bool ends_nl;   /* ...and the last one was a newline */
    bool turn_ended;
    tny_stop_reason stop;
    buf_t errline;
    buf_t host_tools;         /* TOOL_END log for host backends (JSON array items) */
    buf_t extension_messages; /* visible custom/user messages, JSON items */
    tny_engine *engine;
    tny_perm_mode perm_mode;
    bool print_usage;
    bool events;   /* --events=jsonl: stdout belongs to the event stream */
    bool quiet;    /* --progress=none: no successful human status/tool lines */
    bool terminal; /* an actual TURN_END was read and delivered */
} ask_state;

static void ask_event_cb(const tny_backend_event *ev, void *ud) {
    ask_state *st = ud;
    switch (ev->kind) {
    case TNY_EV_TEXT_DELTA: {
        buf_append(&st->output, ev->text, ev->text_len);
        if (st->json || st->events) break; /* the reply rides the event stream */
        const char *text = ev->text;
        size_t len = ev->text_len;
        if (!st->text_seen) {
            /* a model that opens with blank lines: keep them off the terminal
             * until the first visible byte (the buffer above stays raw) */
            size_t ws = str_ws_prefix(text, len);
            if (ws == len) break;
            text += ws;
            len -= ws;
            st->text_seen = true;
        }
        fwrite(text, 1, len, stdout);
        fflush(stdout);
        st->any_out = true;
        st->ends_nl = text[len - 1] == '\n';
        break;
    }
    case TNY_EV_THINKING: break; /* stderr noise in scripts; skip */
    case TNY_EV_TOOL_START:
        if (!st->quiet)
            fprintf(stderr, "⏺ %s %.120s\n", ev->tool_name, ev->tool_detail ? ev->tool_detail : "");
        break;
    case TNY_EV_TOOL_END:
        if (!st->quiet) fprintf(stderr, "  %s %s\n", ev->tool_ok ? "✓" : "✗", ev->tool_name);
        if (tny_engine_backend_id(st->engine) != TNY_BK_OPENAI && ev->tool_name) {
            if (st->host_tools.len) buf_appends(&st->host_tools, ",");
            buf_appends(&st->host_tools, "{\"name\":");
            jescape(&st->host_tools, ev->tool_name);
            buf_appendf(&st->host_tools, ",\"status\":\"%s\"}", ev->tool_ok ? "success" : "error");
        }
        break;
    case TNY_EV_TOOL_PROGRESS:
        if (!st->quiet)
            fprintf(stderr, "  … %s %.120s\n", ev->tool_name ? ev->tool_name : "tool",
                    ev->tool_detail ? ev->tool_detail : "");
        break;
    case TNY_EV_PERMISSION:
        /* `tny ask` never blocks on approvals: yolo allows, otherwise deny.
         * The refusal stays visible under --progress=none — it explains a
         * turn that did not do what was asked. */
        if (st->perm_mode == TNY_MODE_YOLO) {
            if (!st->quiet)
                fprintf(stderr, "auto-approving (yolo): %s\n",
                        ev->perm_summary ? ev->perm_summary : "");
            tny_engine_respond_permission(st->engine, ev->perm_id, TNY_PERM_DECISION_ALLOW);
        } else {
            fprintf(stderr, "denying (ask mode cannot approve): %s\n",
                    ev->perm_summary ? ev->perm_summary : "");
            tny_engine_respond_permission(st->engine, ev->perm_id, TNY_PERM_DECISION_DENY);
        }
        break;
    case TNY_EV_STATUS:
        if (!st->quiet) fprintf(stderr, "%.*s\n", (int)ev->text_len, ev->text);
        break;
    case TNY_EV_CUSTOM_MESSAGE:
        /* the message itself still reaches the result blob and the stream */
        if (!st->quiet) {
            if (ev->message_type)
                fprintf(stderr, "extension context (%s): %.*s\n", ev->message_type,
                        (int)ev->text_len, ev->text);
            else fprintf(stderr, "extension context: %.*s\n", (int)ev->text_len, ev->text);
        }
        if (st->extension_messages.len) buf_appends(&st->extension_messages, ",");
        buf_appends(&st->extension_messages, "{\"kind\":\"custom\",\"custom_type\":");
        jescape(&st->extension_messages, ev->message_type ? ev->message_type : "tny_extension");
        buf_appends(&st->extension_messages, ",\"content\":");
        jescape(&st->extension_messages, ev->text ? ev->text : "");
        buf_appends(&st->extension_messages, "}");
        break;
    case TNY_EV_USER_MESSAGE:
        if (!st->quiet) fprintf(stderr, "extension follow-up: %.*s\n", (int)ev->text_len, ev->text);
        if (st->extension_messages.len) buf_appends(&st->extension_messages, ",");
        buf_appends(&st->extension_messages, "{\"kind\":\"user\",\"content\":");
        jescape(&st->extension_messages, ev->text ? ev->text : "");
        buf_appends(&st->extension_messages, "}");
        break;
    case TNY_EV_STEER_REJECTED: /* ask never steers */ break;
    case TNY_EV_PLAN:
        if (!st->quiet) fprintf(stderr, "plan: %.*s\n", (int)ev->text_len, ev->text);
        break;
    case TNY_EV_USAGE:
        if (!st->print_usage || st->quiet) break;
        /* streamed stdout may lack a trailing newline; finish that line so
         * the usage never glues onto the answer on a terminal */
        if (!st->json && !st->events && st->any_out && !st->ends_nl) {
            fputs("\n", stdout);
            fflush(stdout);
            st->ends_nl = true;
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

/* The foreground --json blob and the session `result` are the same bytes
 * (docs/adr/0031 decision 3); the shared builder lives with the runner. */
static char *ask_result_json(tny_ctx *ctx, ask_state *st, tny_engine *engine,
                             tny_session_state *session, int exit_code) {
    return tny_turn_result_json(ctx, engine, session, st->output.data ? st->output.data : "",
                                st->host_tools.len ? st->host_tools.data : NULL,
                                st->extension_messages.len ? st->extension_messages.data : NULL,
                                st->errline.len ? st->errline.data : NULL, exit_code);
}

/* The whole exit-status decision, in one place so it can be read and tested
 * on its own (docs/adr/0090). Order matters: an undelivered stream outranks
 * whatever the turn reported, and a turn whose terminal event never arrived
 * is a failure rather than the DONE that a zeroed stop reason would imply. */
int cli_ask_exit_status(int stream, bool terminal, int stop) {
    if (stream == TNY_EVENT_WRITE_IO) return 2;
    if (stream == TNY_EVENT_WRITE_CANCELLED) return 130;
    if (!terminal) return 2;
    switch ((tny_stop_reason)stop) {
    case TNY_STOP_DONE: return 0;
    case TNY_STOP_INTERRUPTED: return 130;
    default: return 2;
    }
}

/* Refresh the ADR-0031 status fields after an in-process turn on a session
 * that carries them, so `tny session <id>` reflects this run. */
static void ask_finalize_status(tny_ctx *ctx, tny_session_state *session, ask_state *st,
                                tny_engine *engine, int exit_code, const char *status) {
    char *result = ask_result_json(ctx, st, engine, session, exit_code);
    session_set_status_finished(session, status, exit_code, result);
    free(result);
    session_save(session);
    session_lock_release(session);
}

/* --output-schema VALUE: inline JSON when VALUE starts with '{', otherwise a
 * file path. Normalizes into ctx->output_schema (response_format JSON).
 * Returns 0 ok, -1 error (message already printed). */
static int load_output_schema(tny_ctx *ctx, const char *value, bool events) {
    if (ctx->backend != TNY_BK_OPENAI) {
        ask_diag(events, "invalid_option",
                 "--output-schema needs the openai-compatible provider "
                 "(structured outputs ride on response_format)",
                 "tny --provider openai ask --output-schema schema.json \"…\"");
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
            char message[512];
            snprintf(message, sizeof message, "--output-schema %s: cannot read file", value);
            ask_diag(events, "invalid_option", message,
                     "tny ask --output-schema schema.json \"…\"");
            return -1;
        }
        text = owned;
    }
    char *rf = tny_openai_response_format(text, len);
    free(owned);
    if (!rf) {
        ask_diag(events, "invalid_option", "--output-schema: value is not a JSON object",
                 "tny ask --output-schema '{\"type\":\"object\",\"properties\":{}}' \"…\"");
        return -1;
    }
    free(ctx->output_schema);
    ctx->output_schema = rf;
    return 0;
}

#ifndef __EMSCRIPTEN__
/* ---- isolated foreground: render the runner's stream (docs/adr/0053) ---- */

typedef struct {
    bool json;
    bool print_usage;
    bool steer_takeover;
    bool text_seen; /* leading whitespace of the reply is dropped */
    bool any_out;   /* wrote answer bytes to stdout */
    bool ends_nl;   /* ...and the last one was a newline */
    tny_runner_client *rc;
    pid_t pid; /* the runner: cancels are op + SIGTERM (see loop) */
} ask_client;

/* stdout gets the reply minus its leading whitespace; the runner's NDJSON
 * and the session keep the raw deltas */
static void ask_client_text(ask_client *a, const char *text, size_t len) {
    if (a->json || !text || !len) return;
    if (!a->text_seen) {
        size_t ws = str_ws_prefix(text, len);
        if (ws == len) return;
        text += ws;
        len -= ws;
        a->text_seen = true;
    }
    fwrite(text, 1, len, stdout);
    fflush(stdout);
    a->any_out = true;
    a->ends_nl = text[len - 1] == '\n';
}

static void ask_client_render(ask_client *a, const tny_runner_msg *m) {
    const tny_backend_event *ev = &m->ev;
    switch (ev->kind) {
    case TNY_EV_TEXT_DELTA: ask_client_text(a, ev->text, ev->text_len); break;
    case TNY_EV_THINKING: break;
    case TNY_EV_TOOL_START:
        fprintf(stderr, "⏺ %s %.120s\n", ev->tool_name, ev->tool_detail ? ev->tool_detail : "");
        break;
    case TNY_EV_TOOL_END:
        fprintf(stderr, "  %s %s\n", ev->tool_ok ? "✓" : "✗", ev->tool_name);
        break;
    case TNY_EV_TOOL_PROGRESS:
        fprintf(stderr, "  … %s %.120s\n", ev->tool_name ? ev->tool_name : "tool",
                ev->tool_detail ? ev->tool_detail : "");
        break;
    case TNY_EV_PERMISSION:
        /* only forwarded outside yolo; ask never blocks on approvals */
        fprintf(stderr, "denying (ask mode cannot approve): %s\n",
                ev->perm_summary ? ev->perm_summary : "");
        tny_runner_client_perm(a->rc, ev->perm_id, TNY_PERM_DECISION_DENY);
        break;
    case TNY_EV_STATUS: fprintf(stderr, "%.*s\n", (int)ev->text_len, ev->text); break;
    case TNY_EV_CUSTOM_MESSAGE:
        if (ev->message_type)
            fprintf(stderr, "extension context (%s): %.*s\n", ev->message_type, (int)ev->text_len,
                    ev->text);
        else fprintf(stderr, "extension context: %.*s\n", (int)ev->text_len, ev->text);
        break;
    case TNY_EV_USER_MESSAGE:
        fprintf(stderr, "extension follow-up: %.*s\n", (int)ev->text_len, ev->text);
        break;
    case TNY_EV_PLAN: fprintf(stderr, "plan: %.*s\n", (int)ev->text_len, ev->text); break;
    case TNY_EV_USAGE:
        if (!a->print_usage) break;
        if (!a->json && a->any_out && !a->ends_nl) {
            fputs("\n", stdout);
            fflush(stdout);
            a->ends_nl = true;
        }
        if (ev->context_size > 0)
            fprintf(stderr, "context: %lld/%lld%s\n", (long long)ev->context_used,
                    (long long)ev->context_size, ev->has_cost ? " (cost reported)" : "");
        else
            fprintf(stderr, "tokens: %lld in, %lld out\n", (long long)ev->in_tokens,
                    (long long)ev->out_tokens);
        break;
    case TNY_EV_ERROR: fprintf(stderr, "tny: %.*s\n", (int)ev->text_len, ev->text); break;
    case TNY_EV_STEER_REJECTED:
    case TNY_EV_TURN_END: break;
    }
}

/* Stream until turn_end, then wait out the runner's bye/EOF so its session
 * writes are all on disk before this process returns — the same "teardown
 * precedes exit" contract the in-process turn kept. The runner outlives us
 * after a crash. Explicit interrupts and terminal hangup stop the runner;
 * a second interrupt or expired grace period kills it out of band. */
static int ask_isolated_loop(ask_client *a, tny_ctx *ctx, const char *session_id) {
    signal(SIGINT, on_sigint);
    signal(SIGHUP, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGPIPE, SIG_IGN);
    int exit_code = -1;
    bool done = false, cancelled = false, finishing = false;
    int64_t finish_deadline = 0, cancel_started = 0;
    while (!done) {
        struct pollfd pf = {tny_runner_client_fd(a->rc), POLLIN, 0};
        int pr = tny_poll(&pf, 1, 200);
        if (finishing && now_ms() > finish_deadline) break; /* wedged wind-down */
        if (g_interrupted) {
            g_interrupted = 0;
            if (!cancelled) {
                cancelled = true;
                cancel_started = now_ms();
                fprintf(stderr, "tny: cancelling… press ctrl-c again to force stop\n");
                tny_runner_client_cancel(a->rc, false);
                /* The op alone cannot reach an engine blocked inside a
                 * bounded extension hook or connect — the runner's loop is
                 * not reading the socket then. SIGTERM sets its cancel
                 * probe, which those blocking sections re-check
                 * (docs/adr/0053). */
                if (a->pid > 0) kill(a->pid, SIGTERM);
            } else {
                cancel_started = now_ms() - TNY_PROCESS_CANCEL_GRACE_MS;
            }
        }
        if (cancelled && now_ms() - cancel_started >= TNY_PROCESS_CANCEL_GRACE_MS) {
            char err[256];
            if (session_kill(ctx, session_id, a->pid, err, sizeof err) < 0) {
                fprintf(stderr, "tny: %s (tny session stop %s --kill)\n", err, session_id);
                return 2;
            }
            fprintf(stderr, "tny: session %s stopped\n", session_id);
            return g_terminate ? 128 + g_terminate : 130;
        }
        int alive = 0;
        if (pr > 0) alive = tny_runner_client_pump(a->rc);
        tny_runner_msg *m;
        while ((m = tny_runner_client_pop(a->rc))) {
            switch (m->kind) {
            case TNY_RMSG_EVENT: ask_client_render(a, m); break;
            case TNY_RMSG_RECOVERY:
                if (!a->json && !a->steer_takeover && m->text) {
                    fputs(m->text, stdout);
                    fputs("\n", stdout);
                }
                break;
            case TNY_RMSG_SNAPSHOT:
                if (m->text) ask_client_text(a, m->text, strlen(m->text));
                break;
            case TNY_RMSG_LOG:
                /* host stderr and runner diagnostics: the same trail the
                 * in-process turn printed to this terminal */
                if (m->text) fprintf(stderr, "%s\n", m->text);
                break;
            case TNY_RMSG_TURN_ERR:
                if (m->text) fprintf(stderr, "tny: %s\n", m->text);
                break;
            case TNY_RMSG_TURN_END:
                exit_code = m->exit_code;
                if (a->json && m->result_json) fputs(m->result_json, stdout);
                else if (a->any_out && !a->ends_nl) fputs("\n", stdout);
                finishing = true;
                finish_deadline = now_ms() + 10000;
                break;
            case TNY_RMSG_HELLO: break;
            case TNY_RMSG_ASK_USER: break; /* one-shot owners never answer */
            case TNY_RMSG_BYE:
                if (finishing) done = true;
                break;
            }
            tny_runner_msg_free(m);
        }
        if (!done && alive != 0) {
            if (finishing) break; /* wind-down complete: the socket closed */
            fprintf(stderr,
                    "tny: the session runner exited before finishing the turn "
                    "(tny session %s for its status)\n",
                    session_id);
            return 2;
        }
    }
    return g_terminate ? 128 + g_terminate : exit_code < 0 ? 2 : exit_code;
}
#endif

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

    /* --events=jsonl decides how every later failure is reported, so the
     * mode is resolved before the ordinary parse can fail (docs/adr/0090).
     * Everything after `--` is prompt text, never a mode. */
    bool events = false, quiet = false;
    for (int k = 0; k < argc; k++) {
        if (strcmp(argv[k], "--") == 0) break;
        if (strcmp(argv[k], "--events=jsonl") == 0 ||
            (strcmp(argv[k], "--events") == 0 && k + 1 < argc && strcmp(argv[k + 1], "jsonl") == 0))
            events = true;
    }

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
            else if (strcmp(a, "--events") == 0 || str_starts(a, "--events=")) {
                const char *value = a[strlen("--events")] == '=' ? a + strlen("--events=")
                                    : i + 1 < argc               ? argv[++i]
                                                                 : NULL;
                if (!value || strcmp(value, "jsonl") != 0) {
                    ask_diag(events, "invalid_option", "ask: --events accepts only jsonl",
                             "tny ask --events=jsonl \"summarize this repository\"");
                    buf_free(&prompt);
                    return 1;
                }
                events = true;
            } else if (strcmp(a, "--progress") == 0 || str_starts(a, "--progress=")) {
                const char *value = a[strlen("--progress")] == '=' ? a + strlen("--progress=")
                                    : i + 1 < argc                 ? argv[++i]
                                                                   : NULL;
                if (!value || (strcmp(value, "none") != 0 && strcmp(value, "auto") != 0)) {
                    ask_diag(events, "invalid_option", "ask: --progress accepts auto or none",
                             "tny ask --progress=none \"summarize this repository\"");
                    buf_free(&prompt);
                    return 1;
                }
                quiet = strcmp(value, "none") == 0;
            } else if (strcmp(a, "--auto") == 0) ctx->perm_mode = TNY_MODE_AUTO;
            else if (strcmp(a, "--yolo") == 0) ctx->perm_mode = TNY_MODE_YOLO;
            else if (strcmp(a, "--task") == 0) {
                if (i + 1 >= argc) {
                    ask_diag(events, "invalid_option", "ask: --task requires a value",
                             "tny ask --task review \"inspect the current diff\"");
                    buf_free(&prompt);
                    return 1;
                }
                task_name = argv[++i];
            } else if (strcmp(a, "--resume") == 0 && i + 1 < argc) resume = argv[++i];
            else if (strcmp(a, "--resume-id") == 0 && i + 1 < argc) resume = argv[++i];
            else if (strcmp(a, "--output-schema") == 0) {
                if (i + 1 >= argc) {
                    ask_diag(events, "invalid_option", "--output-schema requires a value",
                             "tny ask --output-schema schema.json \"…\"");
                    buf_free(&prompt);
                    return 1;
                }
                output_schema = argv[++i];
            } else if (strcmp(a, "--image") == 0 && i + 1 < argc) {
                if (n_images < 16) images[n_images++] = argv[++i];
                else {
                    ask_diag(events, "invalid_option", "too many --image flags (max 16)", NULL);
                    buf_free(&prompt);
                    return 1;
                }
            } else if (strcmp(a, "--") == 0) raw = true;
            else {
                char message[256];
                snprintf(message, sizeof message, "ask: unknown flag %s", a);
                ask_diag(events, "invalid_option", message, "tny ask --json \"hi\"");
                buf_free(&prompt);
                return 1;
            }
        } else {
            if (prompt.len) buf_appends(&prompt, " ");
            buf_appends(&prompt, a);
        }
    }
    if (task_name && tny_task_apply(ctx, task_name) != 0) {
        char message[256];
        snprintf(message, sizeof message,
                 "unknown or invalid task '%s' (run `tny tasks` to list tasks)", task_name);
        ask_diag(events, "invalid_option", message, NULL);
        buf_free(&prompt);
        return 1;
    }
    /* One owner per stdout, one process per event stream: the final JSON
     * blob and a detached runner both contradict a foreground event stream
     * (docs/adr/0090). Refuse instead of silently picking a winner. */
    if (events && json) {
        ask_diag(events, "option_conflict",
                 "ask: --events=jsonl is incompatible with --json (both own stdout)",
                 "tny ask --events=jsonl \"summarize this repository\"");
        buf_free(&prompt);
        return 1;
    }
    if (events && background) {
        ask_diag(events, "option_conflict",
                 "ask: --events=jsonl is incompatible with --background "
                 "(a detached turn has no foreground stream; read the session instead)",
                 "tny ask -B \"audit the Makefile\" && tny session last --wait --json");
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
        ask_diag(events, "option_conflict", "--ephemeral is incompatible with --resume", NULL);
        buf_free(&prompt);
        return 1;
    }
    if (steer && !resume) {
        ask_diag(events, "invalid_option", "--steer requires --resume",
                 "tny ask --resume last --steer \"drop that — check the tests instead\"");
        buf_free(&prompt);
        return 1;
    }
    if (ephemeral && continue_recovery) {
        ask_diag(events, "option_conflict", "--ephemeral is incompatible with --continue-recovery",
                 NULL);
        buf_free(&prompt);
        return 1;
    }
    if (output_schema && load_output_schema(ctx, output_schema, events) != 0) {
        buf_free(&prompt);
        return 1;
    }
    /* --image against a provider configured without image input fails here,
     * before a session is created or opened and before any provider work
     * (docs/adr/0089). The engine keeps the same gate for every other
     * caller; this one only avoids a pointless session. */
    if (n_images && tny_image_input_refused(ctx)) {
        fprintf(stderr, "tny: %s\n", TNY_IMAGE_INPUT_REFUSAL);
        buf_free(&prompt);
        return 1;
    }
    ctx->no_save = ephemeral;
    ctx->json_out = json;

    /* The event stream is this process's own engine: the detached runner
     * renders human output and owns a private wire, so machine mode takes
     * the in-process path that already owns the canonical envelope. */
    bool isolate = tny_isolation_enabled(ctx) && !events;

    /* Resolve the session task snapshot before any provider connection is
     * started. This keeps every resume spelling provider-independent. */
    tny_session_state *session = resume ? session_open(ctx, resume) : session_new(ctx);
    if (!session) {
        char message[256];
        if (resume) snprintf(message, sizeof message, "no session '%s' for this workspace", resume);
        else snprintf(message, sizeof message, "could not create a session");
        ask_diag(events, "session", message, NULL);
        buf_free(&prompt);
        return 1;
    }
    if (resume) {
        char task_err[192];
        if (session_task_reconcile(session, task_err, sizeof task_err) != 0) {
            char message[384];
            snprintf(message, sizeof message, "cannot resume session %s: %s", session->id,
                     task_err);
            ask_diag(events, "session", message, NULL);
            session_close(session);
            buf_free(&prompt);
            return 1;
        }
    }

    /* Writer lock (docs/adr/0031 decision 7, widened by 0053): whoever runs
     * a turn on a saved session holds <dir>/lock, so resuming a live session
     * fails loudly instead of corrupting it. Released by session_close at
     * the end (or carried into the detached runner across the fork). */
    if (session && (background || resume || isolate)) {
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
                session_close(session);
                buf_free(&prompt);
                return 2;
            }
            if (src < 0) {
                fprintf(stderr, "tny: %s\n", serr);
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
                if (events) ask_diag(events, "session_busy", "session is still running", NULL);
                else cli_print_still_running(ctx, sid);
                free(sid);
                session_close(session);
                buf_free(&prompt);
                return 1;
            }
            free(sid);
        }
        if (lrc != 0) {
            if (events) ask_diag(events, "session_busy", "session is still running", NULL);
            else cli_print_still_running(ctx, session->id);
            session_close(session);
            buf_free(&prompt);
            return 1;
        }
    }

    if (resume) {
        char task_err[192];
        if (session_reload_locked(session, task_err, sizeof task_err) != 0) {
            char message[384];
            snprintf(message, sizeof message, "cannot resume session %s: %s", session->id,
                     task_err);
            ask_diag(events, "session", message, NULL);
            session_close(session);
            buf_free(&prompt);
            return 1;
        }
    }

#ifndef __EMSCRIPTEN__
    if (isolate && !background) {
        /* Foreground isolation (docs/adr/0053): fork the runner before
         * reading stdin so its provider connect overlaps the pipe drain
         * (the 0004 overlap, processified), then render its stream. */
        char err[512];
        tny_runner_opts opts = {0};
        signal(SIGPIPE, SIG_IGN); /* a dying runner must not SIGPIPE us mid-send */
        pid_t child = tny_runner_spawn(ctx, session, &opts, err, sizeof err);
        if (child > 0) {
            /* The runner inherited the flock's open-file description.
             * Keeping our copy would pin the writer lock after SIGKILL
             * and prevent verified force-stop / status repair. */
            session_lock_release(session);
            char *sock = tny_runner_sock_path(session->dir);
            tny_runner_client *rc =
                sock ? tny_runner_client_connect(sock, 5000, TNY_RUNNER_OWNER, false) : NULL;
            free(sock);
            if (!rc) {
                fprintf(stderr, "tny: cannot reach the session runner\n");
                kill(child, SIGTERM);
                session_close(session);
                buf_free(&prompt);
                return 1;
            }
            if (!prompt.len && (use_stdin || !isatty(0))) {
                char tmp[8192];
                size_t n;
                while ((n = fread(tmp, 1, sizeof tmp, stdin)) > 0) buf_append(&prompt, tmp, n);
                while (prompt.len &&
                       (prompt.data[prompt.len - 1] == '\n' || prompt.data[prompt.len - 1] == '\r'))
                    prompt.data[--prompt.len] = 0;
            }
            if (!prompt.len) {
                fprintf(stderr, "tny: ask needs a prompt\n"
                                "Example: tny ask \"summarize this repository\"\n");
                tny_runner_client_end(rc, "no prompt");
                tny_runner_client_close(rc);
                session_close(session);
                buf_free(&prompt);
                return 1;
            }
            ask_client a = {0};
            a.json = json;
            a.print_usage = print_usage;
            a.steer_takeover = steer_takeover;
            a.rc = rc;
            a.pid = child;
            int rrc = tny_runner_client_turn(rc, prompt.data, n_images ? images : NULL,
                                             continue_recovery);
            int code = rrc == 0 ? ask_isolated_loop(&a, ctx, session->id)
                                : (fprintf(stderr, "tny: cannot reach the session runner\n"), 2);
            tny_runner_client_close(rc);
            session_close(session);
            buf_free(&prompt);
            return code;
        }
        fprintf(stderr, "tny: %s; running in-process\n", err);
        /* fall through to the in-process path below */
    }
#endif

    /* Piped stdin can be slow (upstream producer): overlap the host connect
     * with the read. The argv-prompt path stays serial and untouched. */
    tny_backend *bk = NULL;
    connect_job job = {0};
    pthread_t connect_th;
    bool connecting = false;
    if (!prompt.len && (use_stdin || !isatty(0))) {
        /* background: the fork must precede any pthread_create, so skip the
         * overlap and connect serially in the runner (docs/adr/0031). */
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
        ask_diag(events, "no_prompt", "ask needs a prompt",
                 "tny ask \"summarize this repository\"");
        abort_backend(bk, connecting && job.rc == 0);
        session_close(session);
        buf_free(&prompt);
        return 1;
    }

#ifndef __EMSCRIPTEN__
    if (background) {
        /* Detach (docs/adr/0031 decision 4, runner-ized by 0053) — the
         * ordering is load-bearing: status save, then spawn, then the
         * parent only reports and exits. The runner inherits the flock. */
        session_set_status_running(session);
        if (session_save(session) != 0) {
            fprintf(stderr, "tny: cannot write session %s\n", session->id);
            session_close(session);
            buf_free(&prompt);
            return 1;
        }
        char err[512];
        tny_runner_opts opts = {0};
        opts.initial_prompt = prompt.data;
        opts.initial_images = n_images ? images : NULL;
        opts.continue_recovery = continue_recovery;
        opts.no_host_registry = true; /* decision 8: invisible process must
                                       * not become the attach target */
        pid_t child = tny_runner_spawn(ctx, session, &opts, err, sizeof err);
        if (child < 0) {
            fprintf(stderr, "tny: %s; cannot background\n", err);
            session_close(session);
            buf_free(&prompt);
            return 1;
        }
        /* Parent: report the launch and _exit. No cleanup here — the child
         * owns the session files from now on. */
        if (json)
            printf("{\"kind\":\"ask_background\",\"session_id\":\"%s\","
                   "\"pid\":%d}\n",
                   session->id, (int)child);
        else printf("%s\n", session->id);
        fflush(stdout);
        _exit(0);
    }
#endif

    /* ---- in-process turn (wasm, TNY_ISOLATE=0, ephemeral) ---- */

    ask_state st = {0};
    buf_init(&st.output);
    buf_init(&st.errline);
    buf_init(&st.extension_messages);
    st.json = json;
    st.perm_mode = ctx->perm_mode;
    st.print_usage = print_usage;
    st.events = events;
    st.quiet = quiet;

    /* MCP servers warm on detached threads while the provider connects
     * (docs/adr/0049). Native loop only. */
    if (ctx->backend == TNY_BK_OPENAI) mcp_warm_start(ctx);

    if (continue_recovery && session) {
        char *rec = session_recovery_read(session);
        if (rec) {
            session_set_extension_start(session, "recovery", NULL);
            /* machine mode: stdout carries events only; the replayed partial
             * belongs to the previous turn and has no canonical event */
            if (!json && !events && !steer_takeover) {
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
        /* the constructor already explained itself on stderr in human mode */
        if (events) ask_diag(events, "provider", "cannot create the provider client", NULL);
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
        ask_diag(events, "provider", err, NULL);
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
        ask_diag(events, "internal", "out of memory", NULL);
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
        ask_diag(events, "provider", err, NULL);
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
    /* A supervisor that died before this child was ready gets no request at
     * all: the turn is refused before it starts. */
    if (tny_process_parent_lost()) {
        ask_diag(events, "parent_lost", "the job supervisor that started this turn is gone", NULL);
        tny_engine_free(engine);
        perm_free(perm);
        session_close(session);
        buf_free(&prompt);
        buf_free(&st.output);
        buf_free(&st.errline);
        buf_free(&st.extension_messages);
        return 2;
    }

    signal(SIGINT, on_sigint);
    signal(SIGPIPE, SIG_IGN);

    if (tny_engine_start(engine, prompt.data, n_images ? images : NULL, err, sizeof err) != 0) {
        /* No turn was accepted, so no event exists to emit: the machine
         * diagnostic is the only honest output (docs/adr/0090). */
        ask_diag(events, "start_failed", err, NULL);
        tny_engine_free(engine);
        perm_free(perm);
        session_close(session);
        buf_free(&prompt);
        buf_free(&st.output);
        buf_free(&st.errline);
        buf_free(&st.extension_messages);
        return 2;
    }

    tny_event_writer writer = {0};
    tny_event_write_rc stream = TNY_EVENT_WRITE_OK;
    if (events) {
        fflush(stdout); /* stdout is the stream's from here: no stdio behind it */
        tny_event_writer_init(&writer, fileno(stdout), ask_interrupt_peek, NULL);
    }
    bool parent_gone = false;
    while (!st.turn_ended) {
        /* The engine's cancel probe is consulted at tool and control
         * boundaries; a turn blocked on a provider response would not see a
         * lost job supervisor until those bytes arrived. This bounded pump is
         * the seam that already turns ^C into a cancellation, so the
         * parent-loss watch rides it too — once (docs/adr/0093). */
        bool cancel_now = false;
        if (!parent_gone && tny_process_parent_lost()) {
            parent_gone = true;
            cancel_now = true;
        }
        if (g_interrupted) {
            g_interrupted = 0;
            cancel_now = true;
        }
        if (cancel_now) {
            tny_engine_cancel(engine);
            continue;
        }
        tny_owned_event *owned = NULL;
        tny_engine_next next = tny_engine_next_event(engine, 200, &owned, err, sizeof err);
        if (next == TNY_ENGINE_NEXT_EVENT) {
            if (events) stream = tny_event_writer_emit(&writer, owned);
            /* The terminal counts only once its line is actually out. */
            if (stream == TNY_EVENT_WRITE_OK && owned->ev.kind == TNY_EV_TURN_END)
                st.terminal = true;
            ask_event_cb(&owned->ev, &st);
            tny_owned_event_free(owned);
            if (stream != TNY_EVENT_WRITE_OK) break;
        } else if (next == TNY_ENGINE_NEXT_TIMEOUT) {
            continue;
        } else if (next == TNY_ENGINE_NEXT_DRAINED) {
            break;
        } else if (!st.turn_ended) {
            ask_diag(events, "engine", err, NULL);
            st.turn_ended = true;
            st.stop = TNY_STOP_ERROR;
        }
    }
    if (!events) st.terminal = st.turn_ended;

    /* A stream that failed or was abandoned still owns this turn's cleanup:
     * stop the engine, keep the real output, and never report success. */
    if (stream != TNY_EVENT_WRITE_OK) {
        tny_engine_cancel(engine);
        int64_t deadline = now_ms() + TNY_PROCESS_CANCEL_GRACE_MS;
        for (;;) {
            tny_owned_event *owned = NULL;
            tny_engine_next next = tny_engine_next_event(engine, 100, &owned, err, sizeof err);
            if (next == TNY_ENGINE_NEXT_EVENT) {
                ask_event_cb(&owned->ev, &st); /* buffers text; stdout is gone */
                tny_owned_event_free(owned);
            } else if (next != TNY_ENGINE_NEXT_TIMEOUT) break;
            if (st.turn_ended || now_ms() >= deadline) break;
        }
        if (stream == TNY_EVENT_WRITE_IO)
            ask_diag(events, "stream_io", "the event stream could not be written", NULL);
        else ask_diag(events, "cancelled", "interrupted while the event stream was blocked", NULL);
    } else if (events && !st.terminal) {
        /* Nothing may stand in for a terminal the backend never sent. */
        ask_diag(events, "no_terminal", "the turn ended without a turn_end event", NULL);
    }

    int exit_code = cli_ask_exit_status(stream, st.terminal, st.stop);
    if (exit_code == 0) tny_settings_remember_use(ctx); /* next launch defaults to this provider */

    const char *stname = exit_code == 0 ? "done" : exit_code == 130 ? "interrupted" : "error";
    /* A foreground turn on a session that carries ADR-0031 status fields
     * (a resumed runner/background session) must refresh them, or
     * `tny session <id>` keeps reporting the pre-resume status and
     * result. Legacy sessions (no status field) stay untouched. */
    if (session && !ctx->no_save && session_status(session)) {
        if (st.stop == TNY_STOP_DONE) session_recovery_clear(session);
        ask_finalize_status(ctx, session, &st, engine, exit_code, stname);
    }
    if (json) {
        char *out = ask_result_json(ctx, &st, engine, session, exit_code);
        fputs(out, stdout);
        free(out);
    } else if (st.any_out && !st.ends_nl) {
        fputs("\n", stdout);
    }

    mcp_shutdown_all();
    tny_event_writer_free(&writer);
    tny_engine_free(engine);
    perm_free(perm);
    session_close(session);
    buf_free(&prompt);
    buf_free(&st.output);
    buf_free(&st.errline);
    buf_free(&st.extension_messages);
    return exit_code;
}
