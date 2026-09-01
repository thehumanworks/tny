/* runner.c — the detached session-runner process and its wire client
 * (docs/adr/0053). The runner generalizes the ADR-0031 background child:
 * setsid() group leader, sole session writer, hosts and MCP servers as
 * children, finalize on every exit path — plus an AF_UNIX NDJSON socket so
 * callers can watch, steer, approve, and cancel the turn live. Client
 * death is detachment, never turn death. */
#include "core/runner.h"
#include "core/perm.h"
#include "core/runtime.h"
#include "json/json.h"
#include "mcp/mcp.h"
#include "net/net.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

bool tny_isolation_policy(const tny_ctx *ctx, bool transport_fork_safe) {
#ifdef __EMSCRIPTEN__
    (void)ctx;
    (void)transport_fork_safe;
    return false; /* no fork in the browser (docs/adr/0017) */
#else
    if (ctx && ctx->no_save) return false; /* ephemeral: nothing durable to survive for */
    const char *v = getenv("TNY_ISOLATE");
    if (v && strcmp(v, "0") == 0) return false; /* debug escape hatch */
    if (!transport_fork_safe) return false;     /* macOS fork-pre-exec trust safety */
    return true;
#endif
}

bool tny_isolation_enabled(const tny_ctx *ctx) {
    return tny_isolation_policy(ctx, nstream_fork_safe());
}

/* Shared with cmd_ask's in-process path so the foreground --json blob and
 * the stored session `result` stay the same bytes (docs/adr/0031 dec. 3). */
char *tny_turn_result_json(tny_ctx *ctx, tny_engine *engine, tny_session_state *session,
                           const char *output, const char *host_tools_items,
                           const char *extension_items, const char *errline, int exit_code) {
    buf_t out;
    buf_init(&out);
    buf_appends(&out, "{\"output\":");
    jescape(&out, output ? output : "");
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
        if (host_tools_items) buf_appends(&out, host_tools_items);
        buf_appends(&out, "]");
    }
    if (errline && *errline) {
        buf_appends(&out, ",\"error\":");
        jescape(&out, errline);
    }
    buf_appends(&out, ",\"extension_messages\":[");
    if (extension_items) buf_appends(&out, extension_items);
    buf_appends(&out, "]");
    buf_appends(&out, "}\n");
    return buf_detach(&out);
}

#ifndef __EMSCRIPTEN__

/* ---- wire vocabulary (server and client; native only — every caller
 * lives behind the fork/socket guard below) ---- */

static const char *rn_kind_name(tny_event_kind k) {
    switch (k) {
    case TNY_EV_TEXT_DELTA: return "text_delta";
    case TNY_EV_THINKING: return "thinking";
    case TNY_EV_TOOL_START: return "tool_start";
    case TNY_EV_TOOL_END: return "tool_end";
    case TNY_EV_TOOL_PROGRESS: return "tool_progress";
    case TNY_EV_PERMISSION: return "permission";
    case TNY_EV_PLAN: return "plan";
    case TNY_EV_USAGE: return "usage";
    case TNY_EV_TURN_END: return "turn_end_raw";
    case TNY_EV_ERROR: return "error";
    case TNY_EV_STATUS: return "status";
    case TNY_EV_STEER_REJECTED: return "steer_rejected";
    case TNY_EV_CUSTOM_MESSAGE: return "custom_message";
    case TNY_EV_USER_MESSAGE: return "user_message";
    }
    return "status";
}

static const char *rn_stop_name(tny_stop_reason s) {
    switch (s) {
    case TNY_STOP_DONE: return "done";
    case TNY_STOP_INTERRUPTED: return "interrupted";
    case TNY_STOP_DENIED: return "denied";
    case TNY_STOP_STEP_LIMIT: return "step_limit";
    case TNY_STOP_ERROR: return "error";
    }
    return "error";
}

static tny_stop_reason rn_stop_from(const char *s) {
    if (s && strcmp(s, "done") == 0) return TNY_STOP_DONE;
    if (s && strcmp(s, "interrupted") == 0) return TNY_STOP_INTERRUPTED;
    if (s && strcmp(s, "denied") == 0) return TNY_STOP_DENIED;
    if (s && strcmp(s, "step_limit") == 0) return TNY_STOP_STEP_LIMIT;
    return TNY_STOP_ERROR;
}

static const char *rn_error_name(tny_event_error_kind code) {
    switch (code) {
    case TNY_EVENT_ERROR_IO: return "io";
    case TNY_EVENT_ERROR_PROTOCOL: return "protocol";
    case TNY_EVENT_ERROR_BACKPRESSURE: return "backpressure";
    case TNY_EVENT_ERROR_AUTH: return "auth";
    case TNY_EVENT_ERROR_OOM: return "oom";
    default: return "internal";
    }
}

static tny_event_error_kind rn_error_from(const char *s) {
    if (!s) return TNY_EVENT_ERROR_NONE;
    if (strcmp(s, "io") == 0) return TNY_EVENT_ERROR_IO;
    if (strcmp(s, "protocol") == 0) return TNY_EVENT_ERROR_PROTOCOL;
    if (strcmp(s, "backpressure") == 0) return TNY_EVENT_ERROR_BACKPRESSURE;
    if (strcmp(s, "auth") == 0) return TNY_EVENT_ERROR_AUTH;
    if (strcmp(s, "oom") == 0) return TNY_EVENT_ERROR_OOM;
    return TNY_EVENT_ERROR_INTERNAL;
}

/* One normalized event as one wire line (no trailing newline; caller adds). */
static void rn_event_line(buf_t *b, const tny_backend_event *ev) {
    buf_appends(b, "{\"ev\":");
    jescape(b, rn_kind_name(ev->kind));
    switch (ev->kind) {
    case TNY_EV_TEXT_DELTA:
    case TNY_EV_THINKING:
    case TNY_EV_PLAN:
    case TNY_EV_STATUS:
    case TNY_EV_STEER_REJECTED:
    case TNY_EV_CUSTOM_MESSAGE:
    case TNY_EV_USER_MESSAGE:
    case TNY_EV_ERROR: {
        buf_appends(b, ",\"text\":");
        char *owned = ev->text ? xstrndup(ev->text, ev->text_len) : NULL;
        jescape(b, owned ? owned : "");
        free(owned);
        if (ev->message_type) {
            buf_appends(b, ",\"custom_type\":");
            jescape(b, ev->message_type);
        }
        if (ev->kind == TNY_EV_ERROR) {
            buf_appends(b, ",\"code\":");
            jescape(b, rn_error_name(ev->error_code));
        }
        break;
    }
    case TNY_EV_TOOL_START:
    case TNY_EV_TOOL_END:
    case TNY_EV_TOOL_PROGRESS:
        buf_appends(b, ",\"tool_name\":");
        jescape(b, ev->tool_name ? ev->tool_name : "tool");
        buf_appends(b, ",\"tool_id\":");
        jescape(b, ev->tool_id ? ev->tool_id : "");
        buf_appends(b, ",\"detail\":");
        jescape(b, ev->tool_detail ? ev->tool_detail : "");
        if (ev->kind == TNY_EV_TOOL_END)
            buf_appendf(b, ",\"ok\":%s", ev->tool_ok ? "true" : "false");
        break;
    case TNY_EV_PERMISSION:
        buf_appends(b, ",\"id\":");
        jescape(b, ev->perm_id ? ev->perm_id : "");
        buf_appends(b, ",\"summary\":");
        jescape(b, ev->perm_summary ? ev->perm_summary : "");
        buf_appendf(b, ",\"options\":%d", ev->perm_options);
        break;
    case TNY_EV_USAGE:
        buf_appendf(b,
                    ",\"in\":%lld,\"out\":%lld,\"context_used\":%lld,"
                    "\"context_size\":%lld",
                    (long long)ev->in_tokens, (long long)ev->out_tokens,
                    (long long)ev->context_used, (long long)ev->context_size);
        if (ev->has_cost) buf_appendf(b, ",\"cost\":%.12g", ev->cost);
        break;
    case TNY_EV_TURN_END:
        buf_appends(b, ",\"stop\":");
        jescape(b, rn_stop_name(ev->stop));
        break;
    }
    buf_appends(b, "}");
}

/* Prompts, snapshots, and result blobs ride single NDJSON lines, and the
 * engine accepts multi-megabyte prompts today — the caps only bound a
 * runaway/hostile peer, so they are generous. */
#define RN_MAX_CLIENTS    4
#define RN_MAX_LINE       (64u << 20)
#define RN_OUTBUF_MAX     (64u << 20)
#define RN_CKPT_MS        2000
#define RN_ORPHAN_IDLE_MS 60000

/* A per-uid runtime dir (the tmux pattern) for homes too deep for
 * sun_path. Refuses a dir we do not own outright — never chmod-fixes. */
static char *rn_sock_fallback(const char *tmp, const char *session_id) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "%s/tny-%ld", tmp, (long)getuid());
    if (mkdir(b.data, 0700) != 0 && errno != EEXIST) {
        buf_free(&b);
        return NULL;
    }
    struct stat st;
    if (lstat(b.data, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        buf_free(&b);
        return NULL;
    }
#if !defined(__CYGWIN__) && !defined(__MSYS__)
    /* POSIX mode bits are not faithfully representable on NTFS (MSYS chmod
     * caveat, see test_session_bg); the ownership check above still holds. */
    if ((st.st_mode & 077) != 0) {
        buf_free(&b);
        return NULL;
    }
#endif
    buf_appendf(&b, "/%s.sock", session_id);
    if (b.len >= 100) {
        buf_free(&b);
        return NULL;
    }
    return buf_detach(&b);
}

char *tny_runner_sock_path(const char *session_dir) {
    char *p = path_join(session_dir, "sock");
    if (p && strlen(p) < 100) return p; /* portable floor for sun_path */
    free(p);
    const char *base = strrchr(session_dir, '/');
    base = base ? base + 1 : session_dir;
    const char *tmp = getenv("TMPDIR");
    char *fb = tmp && *tmp ? rn_sock_fallback(tmp, base) : NULL;
    return fb ? fb : rn_sock_fallback("/tmp", base);
}

/* ---- server ---- */

typedef struct {
    int fd;
    buf_t in;
    buf_t out;
} rn_client;

typedef struct {
    tny_ctx *ctx;
    tny_session_state *session;
    perm_engine *perm;
    tny_engine *engine;
    bool serve;
    int lfd;
    char *sock_path;
    rn_client cl[RN_MAX_CLIENTS];
    bool had_client;
    bool quit;
    int quit_code;
    bool end_after_turn;
    bool turn_active;
    bool turn_ended;
    bool turn_ran;
    tny_stop_reason stop;
    int64_t started_ms;
    int64_t last_ckpt;
    /* recorder — the -B accumulation, one place for every mode */
    buf_t output, host_tools, ext_msgs, errline;
    int errpipe;            /* read end of the fd-2 tee (host stderr, diagnostics) */
    buf_t erracc;           /* partial line from the tee */
    char pending_perm[128]; /* forwarded permission id awaiting a client */
} rn_state;

static volatile sig_atomic_t g_rn_stop = 0;
static void rn_on_term(int sig) {
    (void)sig;
    g_rn_stop = 1;
}

static bool rn_cancel_probe(void *ud) {
    (void)ud;
    if (!g_rn_stop) return false;
    g_rn_stop = 0;
    return true;
}

static int rn_client_count(rn_state *r) {
    int n = 0;
    for (int i = 0; i < RN_MAX_CLIENTS; i++)
        if (r->cl[i].fd >= 0) n++;
    return n;
}

static void rn_client_drop(rn_state *r, int i) {
    if (r->cl[i].fd < 0) return;
    close(r->cl[i].fd);
    r->cl[i].fd = -1;
    buf_free(&r->cl[i].in);
    buf_free(&r->cl[i].out);
}

static void rn_client_flush(rn_state *r, int i) {
    rn_client *c = &r->cl[i];
    while (c->fd >= 0 && c->out.len) {
        ssize_t n = write(c->fd, c->out.data, c->out.len);
        if (n > 0) {
            buf_consume(&c->out, (size_t)n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        rn_client_drop(r, i);
        return;
    }
}

static void rn_send_line(rn_state *r, int i, const char *line, size_t len) {
    rn_client *c = &r->cl[i];
    if (c->fd < 0) return;
    if (c->out.len + len > RN_OUTBUF_MAX) { /* stalled reader: cut it loose */
        rn_client_drop(r, i);
        return;
    }
    buf_append(&c->out, line, len);
    buf_append(&c->out, "\n", 1);
    rn_client_flush(r, i);
}

static void rn_broadcast(rn_state *r, const buf_t *line) {
    for (int i = 0; i < RN_MAX_CLIENTS; i++) rn_send_line(r, i, line->data, line->len);
}

static void rn_broadcast_event(rn_state *r, const tny_backend_event *ev) {
    buf_t b;
    buf_init(&b);
    rn_event_line(&b, ev);
    rn_broadcast(r, &b);
    buf_free(&b);
}

static void rn_broadcast_status(rn_state *r, const char *text) {
    fprintf(stdout, "%s\n", text); /* task.log keeps the -B era trail */
    tny_backend_event ev = {0};
    ev.kind = TNY_EV_STATUS;
    ev.text = text;
    ev.text_len = strlen(text);
    rn_broadcast_event(r, &ev);
}

/* Drain the fd-2 tee: every complete line goes to task.log (stdout) and to
 * the clients as a `log` message — the pre-0053 terminal trail, live. */
static void rn_drain_errpipe(rn_state *r) {
    if (r->errpipe < 0) return;
    char tmp[4096];
    for (;;) {
        ssize_t n = read(r->errpipe, tmp, sizeof tmp);
        if (n > 0) {
            buf_append(&r->erracc, tmp, (size_t)n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        close(r->errpipe); /* nothing writes fd 2 anymore */
        r->errpipe = -1;
        break;
    }
    char *nl;
    while (r->erracc.len && (nl = memchr(r->erracc.data, '\n', r->erracc.len))) {
        size_t linelen = (size_t)(nl - r->erracc.data);
        fwrite(r->erracc.data, 1, linelen + 1, stdout);
        buf_t b;
        buf_init(&b);
        buf_appends(&b, "{\"ev\":\"log\",\"text\":");
        char *owned = xstrndup(r->erracc.data, linelen);
        jescape(&b, owned ? owned : "");
        free(owned);
        buf_appends(&b, "}");
        rn_broadcast(r, &b);
        buf_free(&b);
        buf_consume(&r->erracc, linelen + 1);
    }
}

static void rn_send_hello(rn_state *r, int i) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "{\"ev\":\"hello\",\"pid\":%ld,\"provider\":", (long)getpid());
    jescape(&b, tny_provider_name(r->ctx));
    buf_appends(&b, ",\"model\":");
    jescape(&b, r->ctx->model ? r->ctx->model : "default");
    buf_appends(&b, ",\"session_id\":");
    jescape(&b, r->session->id);
    buf_appendf(&b, ",\"turn_active\":%s}", r->turn_active ? "true" : "false");
    rn_send_line(r, i, b.data, b.len);
    buf_clear(&b);
    if (r->turn_active && r->output.len) { /* late joiner catches up */
        buf_appends(&b, "{\"ev\":\"snapshot\",\"text\":");
        jescape(&b, r->output.data);
        buf_appends(&b, "}");
        rn_send_line(r, i, b.data, b.len);
    }
    buf_free(&b);
}

static void rn_accept(rn_state *r) {
    for (;;) {
        int slot = -1;
        for (int i = 0; i < RN_MAX_CLIENTS; i++)
            if (r->cl[i].fd < 0) {
                slot = i;
                break;
            }
        int fd = accept(r->lfd, NULL, NULL);
        if (fd < 0) return;
        if (slot < 0) {
            close(fd); /* full house; the next detach frees a seat */
            return;
        }
        set_nonblock(fd, true);
        r->cl[slot].fd = fd;
        buf_init(&r->cl[slot].in);
        buf_init(&r->cl[slot].out);
        r->had_client = true;
        rn_send_hello(r, slot);
    }
}

/* Finalize the turn that just ended (or failed): status + result + lock,
 * then tell everyone. Safe with engine == NULL (early failures). */
static void rn_finalize(rn_state *r, tny_stop_reason stop, int exit_code) {
    const char *stname = stop == TNY_STOP_DONE          ? "done"
                         : stop == TNY_STOP_INTERRUPTED ? "interrupted"
                                                        : "error";
    if (stop == TNY_STOP_DONE) session_recovery_clear(r->session);
    char *result = tny_turn_result_json(
        r->ctx, r->engine, r->session, r->output.data ? r->output.data : "",
        r->host_tools.len ? r->host_tools.data : NULL, r->ext_msgs.len ? r->ext_msgs.data : NULL,
        r->errline.len ? r->errline.data : NULL, exit_code);
    session_set_status_finished(r->session, stname, exit_code, result);
    session_save(r->session);
    /* serve mode keeps the writer lock across turns — the runner is the
     * session's sole writer for its whole lifetime (docs/adr/0053); it
     * self-releases on exit. once mode releases like the 0031 child. */
    if (!r->serve) session_lock_release(r->session);
    if (stop == TNY_STOP_DONE) tny_settings_remember_use(r->ctx);

    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"ev\":\"turn_end\",\"stop\":");
    jescape(&b, rn_stop_name(stop));
    buf_appendf(&b, ",\"exit_code\":%d,\"result_text\":", exit_code);
    jescape(&b, result ? result : "{}");
    buf_appends(&b, "}");
    rn_broadcast(r, &b);
    buf_free(&b);
    free(result);

    if (r->output.len && r->output.data[r->output.len - 1] != '\n') fputs("\n", stdout);
    fflush(stdout);
    r->turn_active = false;
    r->turn_ended = false;
    r->pending_perm[0] = 0;
    buf_clear(&r->output);
    buf_clear(&r->host_tools);
    buf_clear(&r->ext_msgs);
    buf_clear(&r->errline);
    if (!r->serve || r->end_after_turn) {
        r->quit = true;
        r->quit_code = exit_code;
    }
}

static void rn_turn_err(rn_state *r, const char *msg, int exit_code) {
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"ev\":\"turn_err\",\"text\":");
    jescape(&b, msg);
    buf_appends(&b, "}");
    rn_broadcast(r, &b);
    buf_free(&b);
    fprintf(stdout, "tny-runner: %s\n", msg);
    if (!r->serve) { /* once mode records the failure and gives up */
        buf_clear(&r->errline);
        buf_appends(&r->errline, msg);
        rn_finalize(r, TNY_STOP_ERROR, exit_code);
    } else {
        session_lock_release(r->session);
    }
}

/* Fold one engine event into the recorder, into task.log (our stdio), and
 * onto the wire. The log keeps the pre-0053 background-child observability:
 * `tny session <id>` points readers at it for tool progress. */
static void rn_on_event(rn_state *r, const tny_backend_event *ev) {
    switch (ev->kind) {
    case TNY_EV_TEXT_DELTA:
        buf_append(&r->output, ev->text, ev->text_len);
        fwrite(ev->text, 1, ev->text_len, stdout);
        fflush(stdout);
        rn_broadcast_event(r, ev);
        if (now_ms() - r->last_ckpt >= RN_CKPT_MS) {
            session_recovery_write(r->session, r->output.data ? r->output.data : "");
            r->last_ckpt = now_ms();
        }
        break;
    case TNY_EV_TOOL_START:
        fprintf(stdout, "⏺ %s %.120s\n", ev->tool_name, ev->tool_detail ? ev->tool_detail : "");
        rn_broadcast_event(r, ev);
        break;
    case TNY_EV_TOOL_PROGRESS:
        fprintf(stdout, "  … %s %.120s\n", ev->tool_name ? ev->tool_name : "tool",
                ev->tool_detail ? ev->tool_detail : "");
        rn_broadcast_event(r, ev);
        break;
    case TNY_EV_STATUS:
        fprintf(stdout, "%.*s\n", (int)ev->text_len, ev->text);
        rn_broadcast_event(r, ev);
        break;
    case TNY_EV_TOOL_END:
        fprintf(stdout, "  %s %s\n", ev->tool_ok ? "✓" : "✗", ev->tool_name);
        if (tny_engine_backend_id(r->engine) != TNY_BK_OPENAI && ev->tool_name) {
            if (r->host_tools.len) buf_appends(&r->host_tools, ",");
            buf_appends(&r->host_tools, "{\"name\":");
            jescape(&r->host_tools, ev->tool_name);
            buf_appendf(&r->host_tools, ",\"status\":\"%s\"}", ev->tool_ok ? "success" : "error");
        }
        rn_broadcast_event(r, ev);
        break;
    case TNY_EV_CUSTOM_MESSAGE:
    case TNY_EV_USER_MESSAGE: {
        if (r->ext_msgs.len) buf_appends(&r->ext_msgs, ",");
        if (ev->kind == TNY_EV_CUSTOM_MESSAGE) {
            buf_appends(&r->ext_msgs, "{\"kind\":\"custom\",\"custom_type\":");
            jescape(&r->ext_msgs, ev->message_type ? ev->message_type : "tny_extension");
            buf_appends(&r->ext_msgs, ",\"content\":");
        } else {
            buf_appends(&r->ext_msgs, "{\"kind\":\"user\",\"content\":");
        }
        char *owned = ev->text ? xstrndup(ev->text, ev->text_len) : NULL;
        jescape(&r->ext_msgs, owned ? owned : "");
        free(owned);
        buf_appends(&r->ext_msgs, "}");
        rn_broadcast_event(r, ev);
        break;
    }
    case TNY_EV_ERROR: {
        buf_clear(&r->errline);
        buf_append(&r->errline, ev->text, ev->text_len);
        fprintf(stdout, "tny-runner: %.*s\n", (int)ev->text_len, ev->text);
        rn_broadcast_event(r, ev);
        break;
    }
    case TNY_EV_PERMISSION:
        if (r->ctx->perm_mode == TNY_MODE_YOLO) {
            char line[400];
            snprintf(line, sizeof line, "auto-approving (yolo): %.300s",
                     ev->perm_summary ? ev->perm_summary : "");
            rn_broadcast_status(r, line);
            tny_engine_respond_permission(r->engine, ev->perm_id, TNY_PERM_DECISION_ALLOW);
        } else if (rn_client_count(r) > 0) {
            snprintf(r->pending_perm, sizeof r->pending_perm, "%s", ev->perm_id ? ev->perm_id : "");
            rn_broadcast_event(r, ev); /* the client decides; `perm` op answers */
        } else {
            char line[400];
            snprintf(line, sizeof line, "denying (no client attached to approve): %.280s",
                     ev->perm_summary ? ev->perm_summary : "");
            rn_broadcast_status(r, line);
            tny_engine_respond_permission(r->engine, ev->perm_id, TNY_PERM_DECISION_DENY);
        }
        break;
    case TNY_EV_TURN_END:
        r->turn_ended = true;
        r->stop = ev->stop;
        break;
    default: rn_broadcast_event(r, ev); break;
    }
}

static void rn_drain_engine(rn_state *r) {
    if (!r->engine) return;
    tny_owned_event *owned;
    while ((owned = tny_engine_pop_event(r->engine))) {
        rn_on_event(r, &owned->ev);
        tny_owned_event_free(owned);
    }
}

/* Create + connect + prepare the engine lazily (serve retries per turn). */
static int rn_ensure_engine(rn_state *r, char *err, size_t errlen) {
    if (r->engine) return 0;
    tny_backend *bk = tny_backend_create((tny_backend_id)r->ctx->backend, r->ctx);
    if (!bk) {
        snprintf(err, errlen, "backend create failed");
        return -1;
    }
    if (bk->connect(bk, err, errlen) != 0) {
        bk->destroy(bk);
        return -1;
    }
    tny_engine *engine = tny_engine_new(r->ctx, r->session, r->perm, NULL, NULL);
    if (!engine) {
        bk->disconnect(bk);
        bk->destroy(bk);
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    if (tny_engine_prepare(engine, bk, TNY_ENGINE_PREPARE_CONNECTED, err, errlen) != 0) {
        tny_engine_free(engine);
        return -1;
    }
    tny_engine_set_cancel_probe(engine, rn_cancel_probe, NULL);
    r->engine = engine;
    return 0;
}

static void rn_turn_begin(rn_state *r, const char *prompt, const char **images,
                          bool continue_recovery) {
    char err[512];
    if (r->turn_active) {
        rn_broadcast_status(r, "a turn is already running");
        return;
    }
    if (session_lock_acquire(r->session) != 0) {
        rn_turn_err(r, "session is locked by another process", 1);
        return;
    }
    if (rn_ensure_engine(r, err, sizeof err) != 0) {
        rn_turn_err(r, err, 1);
        return;
    }
    session_set_status_running(r->session);
    if (session_save(r->session) != 0) {
        rn_turn_err(r, "cannot write session", 1);
        return;
    }
    if (continue_recovery) {
        char *rec = session_recovery_read(r->session);
        if (rec) {
            session_set_extension_start(r->session, "recovery", NULL);
            buf_t b;
            buf_init(&b);
            buf_appends(&b, "{\"ev\":\"recovery\",\"text\":");
            jescape(&b, rec);
            buf_appends(&b, "}");
            rn_broadcast(r, &b);
            buf_free(&b);
            session_recovery_clear(r->session);
            free(rec);
        }
    }
    buf_clear(&r->output);
    buf_clear(&r->host_tools);
    buf_clear(&r->ext_msgs);
    buf_clear(&r->errline);
    if (tny_engine_start(r->engine, prompt, images, err, sizeof err) != 0) {
        buf_clear(&r->errline);
        buf_appends(&r->errline, err);
        rn_turn_err(r, err, 2);
        return;
    }
    r->turn_active = true;
    r->turn_ran = true;
    r->last_ckpt = now_ms();
    rn_drain_engine(r);
}

/* The TUI's unconfirmed-cancel fallback, runner-side: drop the engine (the
 * host process group dies with it) and finalize interrupted. */
static void rn_hard_cancel(rn_state *r) {
    if (!r->turn_active) return;
    if (r->engine) {
        tny_engine_preserve_session_on_free(r->engine);
        tny_engine_free(r->engine);
        r->engine = NULL;
    }
    rn_finalize(r, TNY_STOP_INTERRUPTED, 130);
}

static void rn_handle_op(rn_state *r, int ci, yyjson_val *root) {
    const char *op = jget_str(root, "op");
    if (!op) return;
    if (strcmp(op, "turn") == 0) {
        const char *prompt = jget_str(root, "prompt");
        if (!prompt || !*prompt) {
            rn_turn_err(r, "turn without a prompt", 1);
            return;
        }
        const char *images[17] = {0};
        int n = 0;
        yyjson_val *arr = jget(root, "images");
        if (arr && yyjson_is_arr(arr)) {
            size_t idx, max;
            yyjson_val *v;
            yyjson_arr_foreach(arr, idx, max, v) {
                if (n < 16 && yyjson_is_str(v)) images[n++] = yyjson_get_str(v);
            }
        }
        rn_turn_begin(r, prompt, n ? images : NULL, jget_bool(root, "continue_recovery", false));
    } else if (strcmp(op, "steer") == 0) {
        const char *text = jget_str(root, "text");
        if (!text || !*text) return;
        char err[256];
        if (!r->turn_active || !r->engine ||
            tny_engine_steer(r->engine, text, err, sizeof err) != 0) {
            /* hand the text back exactly like a host refusal (docs/adr/0013) */
            tny_backend_event ev = {0};
            ev.kind = TNY_EV_STEER_REJECTED;
            ev.text = text;
            ev.text_len = strlen(text);
            rn_broadcast_event(r, &ev);
        }
    } else if (strcmp(op, "cancel") == 0) {
        if (jget_bool(root, "hard", false)) rn_hard_cancel(r);
        else if (r->turn_active && r->engine) tny_engine_cancel(r->engine);
    } else if (strcmp(op, "perm") == 0) {
        const char *id = jget_str(root, "id");
        const char *d = jget_str(root, "decision");
        if (!id || !d || !r->engine || !r->pending_perm[0]) return;
        if (strcmp(id, r->pending_perm) != 0) return;
        tny_perm_decision dec = strcmp(d, "allow") == 0          ? TNY_PERM_DECISION_ALLOW
                                : strcmp(d, "allow_always") == 0 ? TNY_PERM_DECISION_ALLOW_ALWAYS
                                                                 : TNY_PERM_DECISION_DENY;
        r->pending_perm[0] = 0;
        tny_engine_respond_permission(r->engine, id, dec);
    } else if (strcmp(op, "end") == 0) {
        if (r->turn_active) {
            r->end_after_turn = true;
            if (r->engine) tny_engine_cancel(r->engine);
        } else {
            r->quit = true;
        }
    } else if (strcmp(op, "detach") == 0) {
        rn_client_flush(r, ci);
        rn_client_drop(r, ci);
    }
}

static void rn_client_read(rn_state *r, int i) {
    rn_client *c = &r->cl[i];
    char tmp[8192];
    for (;;) {
        ssize_t n = read(c->fd, tmp, sizeof tmp);
        if (n > 0) {
            buf_append(&c->in, tmp, (size_t)n);
            if (c->in.len > RN_MAX_LINE) {
                rn_client_drop(r, i);
                return;
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        rn_client_drop(r, i); /* EOF or error: the client detached */
        break;
    }
    if (r->cl[i].fd < 0) {
        if (r->pending_perm[0] && rn_client_count(r) == 0 && r->engine) {
            /* decision-pending client vanished: deny, keep the turn moving */
            char id[128];
            snprintf(id, sizeof id, "%s", r->pending_perm);
            r->pending_perm[0] = 0;
            tny_engine_respond_permission(r->engine, id, TNY_PERM_DECISION_DENY);
        }
        return;
    }
    char *nl;
    while (c->fd >= 0 && (nl = memchr(c->in.data, '\n', c->in.len))) {
        size_t linelen = (size_t)(nl - c->in.data);
        yyjson_doc *doc = jparse(c->in.data, linelen);
        buf_consume(&c->in, linelen + 1);
        if (!doc) continue; /* garbage line: skip, keep the connection */
        rn_handle_op(r, i, yyjson_doc_get_root(doc));
        yyjson_doc_free(doc);
    }
}

static _Noreturn void rn_child_main(tny_ctx *ctx, tny_session_state *session,
                                    const tny_runner_opts *opts, int lfd, char *sock_path) {
    setsid(); /* own group: survives the caller; `session stop` signals us */
    session_write_pid(session, getpid());
    if (opts->no_host_registry) ctx->no_host_registry = true;
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
    setvbuf(stdout, NULL, _IOLBF, 0); /* task.log streams line by line */
    setvbuf(stderr, NULL, _IONBF, 0);
    /* Tee fd 2: host stderr and diagnostics still land in task.log, but
     * through a pipe the loop drains — so an attached client sees them
     * live, exactly like the pre-0053 terminal (`log` messages). The write
     * end is nonblocking: a burst larger than the pipe inside one dispatch
     * drops lines instead of deadlocking the single-threaded loop. */
    int errpipe = -1;
    {
        int ep[2];
        if (pipe(ep) == 0) {
            set_nonblock(ep[0], true);
            set_nonblock(ep[1], true);
/* fd 2 is deliberately held for the process lifetime (it IS stderr) */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif
            if (dup2(ep[1], 2) == 2) errpipe = ep[0];
            else close(ep[0]);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
            close(ep[1]);
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = rn_on_term;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL); /* `session stop` cancels like ^C */
    signal(SIGINT, SIG_IGN);       /* not our terminal's business anymore */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    /* MCP servers must be our children so the stop group-signal reaches
     * them; threads do not survive fork (docs/adr/0031, 0049). */
    if (ctx->backend == TNY_BK_OPENAI) mcp_warm_start(ctx);

    rn_state r;
    memset(&r, 0, sizeof r);
    r.ctx = ctx;
    r.session = session;
    r.serve = opts->serve;
    r.lfd = lfd;
    r.sock_path = sock_path;
    r.started_ms = now_ms();
    r.errpipe = errpipe;
    for (int i = 0; i < RN_MAX_CLIENTS; i++) r.cl[i].fd = -1;
    buf_init(&r.output);
    buf_init(&r.erracc);
    buf_init(&r.host_tools);
    buf_init(&r.ext_msgs);
    buf_init(&r.errline);
    r.perm = perm_new(ctx);
    r.quit_code = 0;

    if (r.serve) /* hold the writer lock for the runner's lifetime */
        session_lock_acquire(r.session);
    if (r.serve || !opts->initial_prompt) {
        /* the pre-warm, as a process: connect before any turn arrives (for
         * foreground once-mode this overlaps the caller reading stdin,
         * docs/adr/0004 decision 2) */
        char err[512];
        if (rn_ensure_engine(&r, err, sizeof err) != 0)
            fprintf(stderr, "tny-runner: warm-up: %s (will retry at the first turn)\n", err);
    }
    if (opts->initial_prompt)
        rn_turn_begin(&r, opts->initial_prompt, opts->initial_images, opts->continue_recovery);

    while (!r.quit) {
        struct pollfd fds[2 + RN_MAX_CLIENTS + TNY_BACKEND_POLLFD_MAX];
        int cmap[RN_MAX_CLIENTS];
        nfds_t n = 0;
        int li = -1;
        if (rn_client_count(&r) < RN_MAX_CLIENTS) {
            li = (int)n;
            fds[n].fd = r.lfd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }
        for (int i = 0; i < RN_MAX_CLIENTS; i++) {
            cmap[i] = -1;
            if (r.cl[i].fd < 0) continue;
            cmap[i] = (int)n;
            fds[n].fd = r.cl[i].fd;
            fds[n].events = POLLIN | (r.cl[i].out.len ? POLLOUT : 0);
            fds[n].revents = 0;
            n++;
        }
        int pi = -1;
        if (r.errpipe >= 0) {
            pi = (int)n;
            fds[n].fd = r.errpipe;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }
        int ei = (int)n, ne = 0;
        if (r.turn_active && r.engine) {
            ne = tny_engine_pollfds(r.engine, fds + n, TNY_BACKEND_POLLFD_MAX);
            if (ne > 0) n += (nfds_t)ne;
        }
        int pr = tny_poll(fds, n, r.turn_active ? 100 : 400);
        if (pr < 0 && errno != EINTR) break;

        if (g_rn_stop) {
            g_rn_stop = 0;
            if (r.turn_active && r.engine) tny_engine_cancel(r.engine);
            else r.quit = true;
        }
        if (li >= 0 && (fds[li].revents & POLLIN)) rn_accept(&r);
        for (int i = 0; i < RN_MAX_CLIENTS; i++) {
            if (cmap[i] < 0 || r.cl[i].fd < 0) continue;
            short re = fds[cmap[i]].revents;
            if (re & POLLOUT) rn_client_flush(&r, i);
            if (r.cl[i].fd >= 0 && (re & (POLLIN | POLLHUP | POLLERR))) rn_client_read(&r, i);
        }
        if (pi >= 0 && (fds[pi].revents & (POLLIN | POLLHUP))) rn_drain_errpipe(&r);
        if (r.turn_active && r.engine) {
            tny_engine_dispatch(r.engine, fds + ei, ne);
            rn_drain_engine(&r);
            rn_drain_errpipe(&r); /* forward what the dispatch just printed */
        }
        if (r.turn_ended) {
            int code = r.stop == TNY_STOP_DONE ? 0 : r.stop == TNY_STOP_INTERRUPTED ? 130 : 2;
            rn_finalize(&r, r.stop, code);
        }
        if (r.serve && r.had_client && !r.turn_active && rn_client_count(&r) == 0)
            r.quit = true; /* the shell is gone and nothing is running */
        if (!r.serve && !r.turn_ran && !r.turn_active && rn_client_count(&r) == 0 &&
            now_ms() - r.started_ms > RN_ORPHAN_IDLE_MS)
            r.quit = true; /* launcher died before sending the turn */
    }

    if (r.turn_active) { /* `end` raced a turn that never confirmed */
        rn_hard_cancel(&r);
    }
    if (r.engine) {
        tny_engine_end_session(r.engine, r.serve ? "exit" : "done");
        tny_engine_preserve_session_on_free(r.engine);
        tny_engine_free(r.engine);
        r.engine = NULL;
    }
    session_save(r.session);
    rn_drain_errpipe(&r);
    fflush(NULL); /* task.log is complete before anyone hears bye */
    close(r.lfd);
    unlink(r.sock_path); /* last session-dir mutation: bye promises quiescence */
    {
        buf_t b;
        buf_init(&b);
        buf_appends(&b, "{\"ev\":\"bye\",\"text\":\"runner exiting\"}");
        rn_broadcast(&r, &b);
        buf_free(&b);
    }
    for (int i = 0; i < RN_MAX_CLIENTS; i++) {
        rn_client_flush(&r, i);
        rn_client_drop(&r, i);
    }
    mcp_shutdown_all();
    perm_free(r.perm);
    buf_free(&r.output);
    buf_free(&r.host_tools);
    buf_free(&r.ext_msgs);
    buf_free(&r.errline);
    buf_free(&r.erracc);
    _exit(r.quit_code);
}

pid_t tny_runner_spawn(tny_ctx *ctx, tny_session_state *session, const tny_runner_opts *opts,
                       char *err, size_t errlen) {
    if (!session || ctx->no_save) {
        snprintf(err, errlen, "isolation needs a saved session");
        return -1;
    }
    if (mkdir_p(session->dir) != 0) {
        snprintf(err, errlen, "cannot create %s", session->dir);
        return -1;
    }
    char *sock = tny_runner_sock_path(session->dir);
    if (!sock) {
        snprintf(err, errlen, "session path too long for a unix socket");
        return -1;
    }
    int lfd = unix_listen(sock);
    if (lfd < 0) {
        snprintf(err, errlen, "cannot listen on %s", sock);
        free(sock);
        return -1;
    }
    fflush(NULL); /* buffered stdio must not replay into task.log */
    pid_t pid = fork();
    if (pid < 0) {
        close(lfd);
        unlink(sock);
        free(sock);
        snprintf(err, errlen, "fork failed");
        return -1;
    }
    if (pid > 0) {
        close(lfd);
        free(sock);
        return pid;
    }
    rn_child_main(ctx, session, opts, lfd, sock);
}

/* ---- client ---- */

struct tny_runner_client {
    int fd;
    buf_t in;
    tny_runner_msg *head, *tail;
    bool dead;
};

tny_runner_client *tny_runner_client_connect(const char *sock_path, int timeout_ms) {
    int64_t deadline = monotonic_ms() + (timeout_ms > 0 ? timeout_ms : 0);
    int fd = -1;
    for (;;) {
        fd = unix_connect(sock_path);
        if (fd >= 0) break;
        if (monotonic_ms() >= deadline) return NULL;
        struct pollfd none = {-1, 0, 0};
        tny_poll(&none, 1, 50); /* bounded retry sleep through the seam */
    }
    tny_runner_client *c = calloc(1, sizeof *c);
    if (!c) {
        close(fd);
        return NULL;
    }
    c->fd = fd;
    buf_init(&c->in);
    return c;
}

int tny_runner_client_fd(const tny_runner_client *c) { return c ? c->fd : -1; }

static void rc_queue(tny_runner_client *c, tny_runner_msg *m) {
    m->next = NULL;
    if (c->tail) c->tail->next = m;
    else c->head = m;
    c->tail = m;
}

static void rc_parse_line(tny_runner_client *c, const char *line, size_t len) {
    yyjson_doc *doc = jparse(line, len);
    if (!doc) return;
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *ev = jget_str(root, "ev");
    if (!ev) {
        yyjson_doc_free(doc);
        return;
    }
    tny_runner_msg *m = calloc(1, sizeof *m);
    if (!m) {
        yyjson_doc_free(doc);
        return;
    }
    m->doc = doc;
    if (strcmp(ev, "hello") == 0) {
        m->kind = TNY_RMSG_HELLO;
        m->pid = (pid_t)jget_int(root, "pid", -1);
        m->provider = (char *)jget_str(root, "provider");
        m->model = (char *)jget_str(root, "model");
        m->turn_active = jget_bool(root, "turn_active", false);
    } else if (strcmp(ev, "snapshot") == 0) {
        m->kind = TNY_RMSG_SNAPSHOT;
        m->text = (char *)jget_str(root, "text");
    } else if (strcmp(ev, "recovery") == 0) {
        m->kind = TNY_RMSG_RECOVERY;
        m->text = (char *)jget_str(root, "text");
    } else if (strcmp(ev, "log") == 0) {
        m->kind = TNY_RMSG_LOG;
        m->text = (char *)jget_str(root, "text");
    } else if (strcmp(ev, "turn_end") == 0) {
        m->kind = TNY_RMSG_TURN_END;
        m->ev.kind = TNY_EV_TURN_END;
        m->ev.stop = rn_stop_from(jget_str(root, "stop"));
        m->exit_code = (int)jget_int(root, "exit_code", 2);
        m->result_json = (char *)jget_str(root, "result_text");
    } else if (strcmp(ev, "turn_err") == 0) {
        m->kind = TNY_RMSG_TURN_ERR;
        m->text = (char *)jget_str(root, "text");
    } else if (strcmp(ev, "bye") == 0) {
        m->kind = TNY_RMSG_BYE;
        m->text = (char *)jget_str(root, "text");
    } else {
        m->kind = TNY_RMSG_EVENT;
        tny_backend_event *e = &m->ev;
        size_t tlen = 0;
        const char *text = jget_strn(root, "text", &tlen);
        e->text = text;
        e->text_len = text ? tlen : 0;
        e->message_type = jget_str(root, "custom_type");
        e->tool_name = jget_str(root, "tool_name");
        e->tool_id = jget_str(root, "tool_id");
        e->tool_detail = jget_str(root, "detail");
        e->tool_ok = jget_bool(root, "ok", false);
        e->perm_id = jget_str(root, "id");
        e->perm_summary = jget_str(root, "summary");
        e->perm_options = (int)jget_int(root, "options", 0);
        e->in_tokens = jget_int(root, "in", 0);
        e->out_tokens = jget_int(root, "out", 0);
        e->context_used = jget_int(root, "context_used", 0);
        e->context_size = jget_int(root, "context_size", 0);
        e->cost = jget_num(root, "cost", 0);
        e->has_cost = jget(root, "cost") != NULL;
        e->error_code = rn_error_from(jget_str(root, "code"));
        if (strcmp(ev, "text_delta") == 0) e->kind = TNY_EV_TEXT_DELTA;
        else if (strcmp(ev, "thinking") == 0) e->kind = TNY_EV_THINKING;
        else if (strcmp(ev, "tool_start") == 0) e->kind = TNY_EV_TOOL_START;
        else if (strcmp(ev, "tool_end") == 0) e->kind = TNY_EV_TOOL_END;
        else if (strcmp(ev, "tool_progress") == 0) e->kind = TNY_EV_TOOL_PROGRESS;
        else if (strcmp(ev, "permission") == 0) e->kind = TNY_EV_PERMISSION;
        else if (strcmp(ev, "plan") == 0) e->kind = TNY_EV_PLAN;
        else if (strcmp(ev, "usage") == 0) e->kind = TNY_EV_USAGE;
        else if (strcmp(ev, "error") == 0) e->kind = TNY_EV_ERROR;
        else if (strcmp(ev, "steer_rejected") == 0) e->kind = TNY_EV_STEER_REJECTED;
        else if (strcmp(ev, "custom_message") == 0) e->kind = TNY_EV_CUSTOM_MESSAGE;
        else if (strcmp(ev, "user_message") == 0) e->kind = TNY_EV_USER_MESSAGE;
        else e->kind = TNY_EV_STATUS;
    }
    rc_queue(c, m);
}

int tny_runner_client_pump(tny_runner_client *c) {
    if (!c || c->fd < 0) return -1;
    char tmp[8192];
    for (;;) {
        ssize_t n = read(c->fd, tmp, sizeof tmp);
        if (n > 0) {
            buf_append(&c->in, tmp, (size_t)n);
            if (c->in.len > RN_MAX_LINE) {
                c->dead = true;
                break;
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        c->dead = true;
        break;
    }
    char *nl;
    while (c->in.len && (nl = memchr(c->in.data, '\n', c->in.len))) {
        size_t linelen = (size_t)(nl - c->in.data);
        rc_parse_line(c, c->in.data, linelen);
        buf_consume(&c->in, linelen + 1);
    }
    return c->dead ? -1 : 0;
}

tny_runner_msg *tny_runner_client_pop(tny_runner_client *c) {
    if (!c || !c->head) return NULL;
    tny_runner_msg *m = c->head;
    c->head = m->next;
    if (!c->head) c->tail = NULL;
    m->next = NULL;
    return m;
}

void tny_runner_msg_free(tny_runner_msg *m) {
    if (!m) return;
    yyjson_doc_free((yyjson_doc *)m->doc);
    free(m);
}

static int rc_send(tny_runner_client *c, const buf_t *line) {
    if (!c || c->fd < 0 || c->dead) return -1;
    size_t off = 0;
    while (off < line->len) {
        ssize_t n = write(c->fd, line->data + off, line->len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = {c->fd, POLLOUT, 0};
            if (tny_poll(&pf, 1, 5000) <= 0) return -1;
            continue;
        }
        c->dead = true;
        return -1;
    }
    return 0;
}

int tny_runner_client_turn(tny_runner_client *c, const char *prompt, const char **images,
                           bool continue_recovery) {
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"op\":\"turn\",\"prompt\":");
    jescape(&b, prompt ? prompt : "");
    if (images && images[0]) {
        buf_appends(&b, ",\"images\":[");
        for (int i = 0; images[i]; i++) {
            if (i) buf_appends(&b, ",");
            jescape(&b, images[i]);
        }
        buf_appends(&b, "]");
    }
    if (continue_recovery) buf_appends(&b, ",\"continue_recovery\":true");
    buf_appends(&b, "}\n");
    int rc = rc_send(c, &b);
    buf_free(&b);
    return rc;
}

int tny_runner_client_steer(tny_runner_client *c, const char *text) {
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"op\":\"steer\",\"text\":");
    jescape(&b, text ? text : "");
    buf_appends(&b, "}\n");
    int rc = rc_send(c, &b);
    buf_free(&b);
    return rc;
}

int tny_runner_client_cancel(tny_runner_client *c, bool hard) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "{\"op\":\"cancel\"%s}\n", hard ? ",\"hard\":true" : "");
    int rc = rc_send(c, &b);
    buf_free(&b);
    return rc;
}

int tny_runner_client_perm(tny_runner_client *c, const char *perm_id, tny_perm_decision d) {
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"op\":\"perm\",\"id\":");
    jescape(&b, perm_id ? perm_id : "");
    buf_appends(&b, ",\"decision\":");
    jescape(&b, d == TNY_PERM_DECISION_ALLOW          ? "allow"
                : d == TNY_PERM_DECISION_ALLOW_ALWAYS ? "allow_always"
                                                      : "deny");
    buf_appends(&b, "}\n");
    int rc = rc_send(c, &b);
    buf_free(&b);
    return rc;
}

int tny_runner_client_end(tny_runner_client *c, const char *reason) {
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"op\":\"end\",\"reason\":");
    jescape(&b, reason ? reason : "exit");
    buf_appends(&b, "}\n");
    int rc = rc_send(c, &b);
    buf_free(&b);
    return rc;
}

void tny_runner_client_close(tny_runner_client *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    buf_free(&c->in);
    tny_runner_msg *m = c->head;
    while (m) {
        tny_runner_msg *next = m->next;
        tny_runner_msg_free(m);
        m = next;
    }
    free(c);
}

#else /* __EMSCRIPTEN__: clean-error stubs (docs/adr/0017, 0053) */

char *tny_runner_sock_path(const char *session_dir) {
    (void)session_dir;
    return NULL;
}
pid_t tny_runner_spawn(tny_ctx *ctx, tny_session_state *session, const tny_runner_opts *opts,
                       char *err, size_t errlen) {
    (void)ctx;
    (void)session;
    (void)opts;
    snprintf(err, errlen, "process isolation is not available in the browser build");
    return -1;
}
tny_runner_client *tny_runner_client_connect(const char *sock_path, int timeout_ms) {
    (void)sock_path;
    (void)timeout_ms;
    return NULL;
}
int tny_runner_client_fd(const tny_runner_client *c) {
    (void)c;
    return -1;
}
int tny_runner_client_pump(tny_runner_client *c) {
    (void)c;
    return -1;
}
tny_runner_msg *tny_runner_client_pop(tny_runner_client *c) {
    (void)c;
    return NULL;
}
void tny_runner_msg_free(tny_runner_msg *m) { (void)m; }
int tny_runner_client_turn(tny_runner_client *c, const char *prompt, const char **images,
                           bool continue_recovery) {
    (void)c;
    (void)prompt;
    (void)images;
    (void)continue_recovery;
    return -1;
}
int tny_runner_client_steer(tny_runner_client *c, const char *text) {
    (void)c;
    (void)text;
    return -1;
}
int tny_runner_client_cancel(tny_runner_client *c, bool hard) {
    (void)c;
    (void)hard;
    return -1;
}
int tny_runner_client_perm(tny_runner_client *c, const char *perm_id, tny_perm_decision d) {
    (void)c;
    (void)perm_id;
    (void)d;
    return -1;
}
int tny_runner_client_end(tny_runner_client *c, const char *reason) {
    (void)c;
    (void)reason;
    return -1;
}
void tny_runner_client_close(tny_runner_client *c) { (void)c; }

#endif
