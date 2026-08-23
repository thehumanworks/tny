/* cmd_ask.c — one noninteractive turn (docs/cli.md). Never blocks on an
 * approval: unresolved permissions end the run with exit 2. */
#include "cli/cli.h"
#include "core/backend.h"
#include "core/session.h"
#include "core/perm.h"
#include "backends/openai/openai.h"
#include "mcp/mcp.h"
#include "util/util.h"
#include "util/tny_poll.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>

static volatile sig_atomic_t g_interrupted = 0;
static void on_sigint(int sig) { (void)sig; g_interrupted = 1; }

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
    buf_t host_tools; /* TOOL_END log for host backends (JSON array items) */
    tny_backend *bk;
    tny_perm_mode perm_mode;
} ask_state;

static void ask_event_cb(const tny_event *ev, void *ud) {
    ask_state *st = ud;
    switch (ev->kind) {
    case TNY_EV_TEXT_DELTA:
        buf_append(&st->output, ev->text, ev->text_len);
        if (!st->json) {
            fwrite(ev->text, 1, ev->text_len, stdout);
            fflush(stdout);
        }
        break;
    case TNY_EV_THINKING:
        break; /* stderr noise in scripts; skip */
    case TNY_EV_TOOL_START:
        fprintf(stderr, "⏺ %s %.120s\n", ev->tool_name,
                ev->tool_detail ? ev->tool_detail : "");
        break;
    case TNY_EV_TOOL_END:
        fprintf(stderr, "  %s %s\n", ev->tool_ok ? "✓" : "✗", ev->tool_name);
        if (st->bk->id != TNY_BK_OPENAI && ev->tool_name) {
            if (st->host_tools.len) buf_appends(&st->host_tools, ",");
            buf_appends(&st->host_tools, "{\"name\":");
            jescape(&st->host_tools, ev->tool_name);
            buf_appendf(&st->host_tools, ",\"status\":\"%s\"}",
                        ev->tool_ok ? "success" : "error");
        }
        break;
    case TNY_EV_PERMISSION:
        /* `tny ask` never blocks on approvals: yolo allows, otherwise deny */
        if (st->perm_mode == TNY_MODE_YOLO) {
            fprintf(stderr, "auto-approving (yolo): %s\n",
                    ev->perm_summary ? ev->perm_summary : "");
            st->bk->respond_permission(st->bk, ev->perm_id, TNY_PERM_DECISION_ALLOW);
        } else {
            fprintf(stderr, "denying (ask mode cannot approve): %s\n",
                    ev->perm_summary ? ev->perm_summary : "");
            st->bk->respond_permission(st->bk, ev->perm_id, TNY_PERM_DECISION_DENY);
        }
        break;
    case TNY_EV_STATUS:
        fprintf(stderr, "%.*s\n", (int)ev->text_len, ev->text);
        break;
    case TNY_EV_STEER_REJECTED: /* ask never steers */
        break;
    case TNY_EV_PLAN:
        fprintf(stderr, "plan: %.*s\n", (int)ev->text_len, ev->text);
        break;
    case TNY_EV_USAGE:
        fprintf(stderr, "tokens: %lld in, %lld out\n",
                (long long)ev->in_tokens, (long long)ev->out_tokens);
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

/* --output-schema VALUE: inline JSON when VALUE starts with '{', otherwise a
 * file path. Normalizes into ctx->output_schema (response_format JSON).
 * Returns 0 ok, -1 error (message already printed). */
static int load_output_schema(tny_ctx *ctx, const char *value) {
    if (ctx->backend != TNY_BK_OPENAI) {
        fprintf(stderr,
                "tny: --output-schema needs the openai-compatible provider "
                "(structured outputs ride on response_format)\n"
                "Example: tny --provider openai ask --output-schema schema.json \"…\"\n");
        return -1;
    }
    const char *text = value;
    size_t len = strlen(value);
    char *owned = NULL;
    while (*text == ' ' || *text == '\t') { text++; len--; }
    if (*text != '{') {
        owned = file_slurp(value, &len);
        if (!owned) {
            fprintf(stderr, "tny: --output-schema %s: cannot read file\n"
                    "Example: tny ask --output-schema schema.json \"…\"\n", value);
            return -1;
        }
        text = owned;
    }
    char *rf = tny_openai_response_format(text, len);
    free(owned);
    if (!rf) {
        fprintf(stderr, "tny: --output-schema: value is not a JSON object\n"
                "Example: tny ask --output-schema '{\"type\":\"object\",\"properties\":{}}' \"…\"\n");
        return -1;
    }
    free(ctx->output_schema);
    ctx->output_schema = rf;
    return 0;
}

int cmd_ask(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = g->json, use_stdin = false, ephemeral = ctx->no_save;
    bool continue_recovery = false;
    const char *resume = g->resume;
    const char *output_schema = NULL;
    const char *images[17] = {0};
    int n_images = 0;
    buf_t prompt;
    buf_init(&prompt);

    int i = 0;
    bool raw = false;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!raw && a[0] == '-' && a[1]) {
            if (strcmp(a, "--json") == 0) json = true;
            else if (strcmp(a, "--stdin") == 0) use_stdin = true;
            else if (strcmp(a, "--ephemeral") == 0 || strcmp(a, "--no-save") == 0)
                ephemeral = true;
            else if (strcmp(a, "--no-color") == 0) { /* colors already plain */ }
            else if (strcmp(a, "--continue-recovery") == 0) continue_recovery = true;
            else if (strcmp(a, "--auto") == 0) ctx->perm_mode = TNY_MODE_AUTO;
            else if (strcmp(a, "--yolo") == 0) ctx->perm_mode = TNY_MODE_YOLO;
            else if (strcmp(a, "--resume") == 0 && i + 1 < argc) resume = argv[++i];
            else if (strcmp(a, "--resume-id") == 0 && i + 1 < argc) resume = argv[++i];
            else if (strcmp(a, "--output-schema") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "tny: --output-schema requires a value\n"
                            "Example: tny ask --output-schema schema.json \"…\"\n");
                    buf_free(&prompt);
                    return 1;
                }
                output_schema = argv[++i];
            }
            else if (strcmp(a, "--image") == 0 && i + 1 < argc) {
                if (n_images < 16) images[n_images++] = argv[++i];
                else { fprintf(stderr, "tny: too many --image flags (max 16)\n"); return 1; }
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
    if (ephemeral && resume) {
        fprintf(stderr, "tny: --ephemeral is incompatible with --resume\n");
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

    /* Piped stdin can be slow (upstream producer): overlap the host connect
     * with the read. The argv-prompt path stays serial and untouched. */
    tny_backend *bk = NULL;
    connect_job job = {0};
    pthread_t connect_th;
    bool connecting = false;
    if (!prompt.len && (use_stdin || !isatty(0))) {
        bk = tny_backend_create((tny_backend_id)ctx->backend, ctx);
        if (bk) {
            job.bk = bk;
            if (pthread_create(&connect_th, NULL, connect_job_main, &job) == 0)
                connecting = true;
            /* pthread_create failed: fall back to the serial connect below */
        }
        char tmp[8192];
        size_t n;
        while ((n = fread(tmp, 1, sizeof tmp, stdin)) > 0) buf_append(&prompt, tmp, n);
        while (prompt.len && (prompt.data[prompt.len - 1] == '\n' ||
                              prompt.data[prompt.len - 1] == '\r'))
            prompt.data[--prompt.len] = 0;
        if (connecting) pthread_join(connect_th, NULL);
    }
    if (!prompt.len) {
        fprintf(stderr, "tny: ask needs a prompt\nExample: tny ask \"summarize this repository\"\n");
        abort_backend(bk, connecting && job.rc == 0);
        buf_free(&prompt);
        return 1;
    }

    /* session */
    tny_session *session = NULL;
    if (resume) {
        session = session_open(ctx, resume);
        if (!session) {
            fprintf(stderr, "tny: no session '%s' for this workspace\n", resume);
            abort_backend(bk, connecting && job.rc == 0);
            buf_free(&prompt);
            return 1;
        }
    } else {
        session = session_new(ctx);
    }

    if (continue_recovery && session) {
        char *rec = session_recovery_read(session);
        if (rec) {
            if (!json) { fputs(rec, stdout); fputs("\n", stdout); }
            session_recovery_clear(session);
            free(rec);
        }
    }

    /* backend (already created — and its connect already joined — on the
     * stdin path above) */
    if (!bk) bk = tny_backend_create((tny_backend_id)ctx->backend, ctx);
    if (!bk) { buf_free(&prompt); session_close(session); return 1; }
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
        bk->destroy(bk);
        session_close(session);
        buf_free(&prompt);
        return 1;
    }
    perm_engine *perm = perm_new(ctx);
    if (bk->id == TNY_BK_OPENAI)
        tny_backend_openai_bind(bk, session, perm, NULL, NULL);
    const char *hp = session_host_pointer(session);
    /* a host pointer only means something to the provider that minted it */
    const char *owner = session_backend(session);
    if (hp && owner && strcmp(owner, tny_provider_name(ctx)) != 0) hp = NULL;
    if (bk->create_or_resume && bk->create_or_resume(bk, hp, err, sizeof err) != 0) {
        fprintf(stderr, "tny: %s\n", err);
        bk->destroy(bk);
        perm_free(perm);
        session_close(session);
        buf_free(&prompt);
        return 1;
    }

    ask_state st = {0};
    buf_init(&st.output);
    buf_init(&st.errline);
    st.json = json;
    st.bk = bk;
    st.perm_mode = ctx->perm_mode;

    signal(SIGINT, on_sigint);
    signal(SIGPIPE, SIG_IGN);

    if (bk->send(bk, prompt.data, n_images ? images : NULL, ask_event_cb, &st,
                 err, sizeof err) != 0) {
        fprintf(stderr, "tny: %s\n", err);
        bk->destroy(bk);
        perm_free(perm);
        session_close(session);
        buf_free(&prompt);
        buf_free(&st.output);
        buf_free(&st.errline);
        return 2;
    }

    while (!st.turn_ended) {
        if (g_interrupted) {
            g_interrupted = 0;
            bk->cancel(bk);
            continue;
        }
        struct pollfd fds[8];
        int n = bk->pollfds ? bk->pollfds(bk, fds, 8) : 0;
        if (n > 0) tny_poll(fds, (nfds_t)n, 200);
        if (bk->dispatch && bk->dispatch(bk, fds, n) != 0 && !st.turn_ended) {
            st.turn_ended = true;
            st.stop = TNY_STOP_ERROR;
        }
    }

    /* Capture host pointers for the in-process session; session_save is a
     * no-op in ephemeral mode. */
    if (bk->session_pointer) {
        char *ptr = bk->session_pointer(bk);
        if (ptr) {
            session_set_host_pointer(session, ptr);
            session_set_meta(session, tny_provider_name(ctx), ctx->model);
            if (!session_title(session)) session_set_title(session, prompt.data);
            session_save(session);
            free(ptr);
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

    if (json) {
        buf_t out;
        buf_init(&out);
        buf_appends(&out, "{\"output\":");
        jescape(&out, st.output.data ? st.output.data : "");
        buf_appendf(&out, ",\"exit_code\":%d,\"provider\":\"%s\",\"model\":",
                    exit_code, tny_provider_name(ctx));
        jescape(&out, ctx->model ? ctx->model : "default");
        buf_appends(&out, ",\"session_id\":");
        jescape(&out, ctx->no_save ? "" : session->id);
        buf_appendf(&out, ",\"ephemeral\":%s", ctx->no_save ? "true" : "false");
        int steps = bk->id == TNY_BK_OPENAI ? tny_backend_openai_steps(bk) : 1;
        buf_appendf(&out, ",\"steps\":%d,\"tool_calls\":", steps);
        if (bk->id == TNY_BK_OPENAI) {
            buf_appends(&out, tny_backend_openai_toolcalls_json(bk));
        } else {
            buf_appends(&out, "[");
            if (st.host_tools.len) buf_append(&out, st.host_tools.data, st.host_tools.len);
            buf_appends(&out, "]");
        }
        if (st.errline.len) {
            buf_appends(&out, ",\"error\":");
            jescape(&out, st.errline.data);
        }
        buf_appends(&out, "}\n");
        fwrite(out.data, 1, out.len, stdout);
        buf_free(&out);
    } else if (st.output.len && st.output.data[st.output.len - 1] != '\n') {
        fputs("\n", stdout);
    }

    mcp_shutdown_all();
    bk->disconnect(bk);
    bk->destroy(bk);
    perm_free(perm);
    session_close(session);
    buf_free(&prompt);
    buf_free(&st.output);
    buf_free(&st.errline);
    return exit_code;
}
