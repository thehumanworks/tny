/* runner.c — the detached session-runner process and its wire client
 * (docs/adr/0053). The runner generalizes the ADR-0031 background child:
 * setsid() group leader, sole session writer, hosts and MCP servers as
 * children, finalize on every exit path — plus an AF_UNIX NDJSON socket so
 * callers can watch, steer, approve, and cancel the turn live. Client
 * death is detachment, never turn death. */
extern "C" {
#include "core/runner.h"
#include "core/checkpoint.h"
#include "core/extensions.h"
#include "util/jobs_host.h"
#include "util/process.h"
#include <sys/wait.h>
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
}
#include "util/ownership.hpp"
#include "util/resources.hpp"
extern "C" char **environ;

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
    char *usage = tny_engine_openai_usage_json(engine);
    buf_appends(&out, ",\"usage\":");
    buf_appends(&out, usage ? usage : "null");
    free(usage);
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
    tny::descriptor fd;
    buf_t in;
    buf_t out;
    tny_runner_role role;
    bool handshaken;
    bool can_answer_questions;
    int64_t accepted_ms;
} rn_client;

typedef struct {
    tny_ctx *ctx;
    tny_session_state *session;
    perm_engine *perm;
    tny_engine *engine;
    bool serve;
    tny::descriptor lfd;
    char *sock_path;
    rn_client cl[RN_MAX_CLIENTS];
    bool had_client;
    bool quit;
    int quit_code;
    bool end_after_turn;
    bool turn_active;
    bool turn_ended;
    bool turn_ran;
    bool control_pumping;
    bool hard_cancel_pending;
    bool background_armed, background, background_permissions, handoff_pending;
    int64_t permission_deadline;
    char *permission_summary;
    int permission_options;
    tny_stop_reason stop;
    int64_t started_ms;
    int64_t last_ckpt;
    /* recorder — the -B accumulation, one place for every mode */
    buf_t output, thinking, host_tools, ext_msgs, errline;
    tny::descriptor errpipe; /* read end of the fd-2 tee (host stderr, diagnostics) */
    buf_t erracc;            /* partial line from the tee */
    char pending_perm[128];  /* forwarded permission id awaiting a client */
    bool question_pending;
    bool question_done;
    bool question_failed;
    int question_tool_client; /* -1: engine callback; >=0: tool client */
    char question_id[128];
    char *question_answer;
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
        if (r->cl[i].fd.borrow() >= 0) n++;
    return n;
}

static int rn_frontend_count(rn_state *r) {
    int n = 0;
    for (int i = 0; i < RN_MAX_CLIENTS; i++)
        if (r->cl[i].fd.borrow() >= 0 && r->cl[i].handshaken && r->cl[i].role != TNY_RUNNER_TOOL)
            n++;
    return n;
}

static int rn_owner(rn_state *r) {
    for (int i = 0; i < RN_MAX_CLIENTS; i++)
        if (r->cl[i].fd.borrow() >= 0 && r->cl[i].handshaken && r->cl[i].role == TNY_RUNNER_OWNER)
            return i;
    return -1;
}

static void rn_question_fail(rn_state *r, const char *error);
static int rn_control_pump(void *ud, int timeout_ms);
static char *rn_ask_user(const char *question, void *ud);

static const char *rn_role_name(tny_runner_role role) {
    switch (role) {
    case TNY_RUNNER_OWNER: return "owner";
    case TNY_RUNNER_OBSERVER: return "observer";
    case TNY_RUNNER_TOOL: return "tool";
    default: return "unknown";
    }
}

static void rn_client_drop(rn_state *r, int i) {
    if (r->cl[i].fd.borrow() < 0) return;
    bool owner = r->cl[i].handshaken && r->cl[i].role == TNY_RUNNER_OWNER;
    r->cl[i].fd.reset();
    buf_free(&r->cl[i].in);
    buf_free(&r->cl[i].out);
    if (r->question_pending && r->question_tool_client == i) {
        r->question_pending = false;
        r->question_done = true;
        r->question_failed = true;
    }
    if (owner) {
        if (r->pending_perm[0] && r->engine && !r->background_permissions) {
            char id[sizeof r->pending_perm];
            snprintf(id, sizeof id, "%s", r->pending_perm);
            r->pending_perm[0] = 0;
            tny_engine_respond_permission(r->engine, id, TNY_PERM_DECISION_DENY);
        }
        if (r->question_pending) rn_question_fail(r, "owning frontend disconnected");
    }
}

static void rn_client_flush(rn_state *r, int i) {
    rn_client *c = &r->cl[i];
    while (c->fd.borrow() >= 0 && c->out.len) {
        ssize_t n = write(c->fd.borrow(), c->out.data, c->out.len);
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

/* A final result can exceed the socket's send buffer. Let readers drain it
 * before closing; a single nonblocking flush followed by close truncates
 * turn_end and makes a successfully interrupted turn look like a crash. */
static void rn_flush_before_exit(rn_state *r) {
    int64_t deadline = monotonic_ms() + 2000;
    while (monotonic_ms() < deadline) {
        struct pollfd fds[RN_MAX_CLIENTS];
        int clients[RN_MAX_CLIENTS];
        nfds_t n = 0;
        for (int i = 0; i < RN_MAX_CLIENTS; i++) {
            if (r->cl[i].fd.borrow() < 0 || !r->cl[i].out.len) continue;
            clients[n] = i;
            fds[n++] = pollfd{r->cl[i].fd.borrow(), POLLOUT, 0};
        }
        if (!n) break;
        int pr = tny_poll(fds, n, 50);
        if (pr < 0 && errno != EINTR) break;
        for (nfds_t i = 0; i < n; i++)
            if (fds[i].revents) rn_client_flush(r, clients[i]);
    }
}

static void rn_send_line(rn_state *r, int i, const char *line, size_t len) {
    rn_client *c = &r->cl[i];
    if (c->fd.borrow() < 0) return;
    if (c->out.len + len > RN_OUTBUF_MAX) { /* stalled reader: cut it loose */
        rn_client_drop(r, i);
        return;
    }
    buf_append(&c->out, line, len);
    buf_append(&c->out, "\n", 1);
    rn_client_flush(r, i);
}

/* `status` and `error_code` are OPTIONAL additions for the image_preview op
 * (docs/adr/0096); existing replies pass NULL for both and keep their exact
 * ok/answer/error shape. */
static void rn_send_control_result_ex(rn_state *r, int i, const char *id, const char *answer,
                                      const char *error, const char *status,
                                      const char *error_code) {
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"ev\":\"control_result\",\"id\":");
    jescape(&b, id ? id : "");
    buf_appendf(&b, ",\"ok\":%s", error ? "false" : "true");
    if (answer) {
        buf_appends(&b, ",\"answer\":");
        jescape(&b, answer);
    }
    if (error) {
        buf_appends(&b, ",\"error\":");
        jescape(&b, error);
    }
    if (status) {
        buf_appends(&b, ",\"status\":");
        jescape(&b, status);
    }
    if (error_code) {
        buf_appends(&b, ",\"error_code\":");
        jescape(&b, error_code);
    }
    buf_appends(&b, "}");
    rn_send_line(r, i, b.data, b.len);
    buf_free(&b);
}

static void rn_send_control_result(rn_state *r, int i, const char *id, const char *answer,
                                   const char *error) {
    rn_send_control_result_ex(r, i, id, answer, error, NULL, NULL);
}

static void rn_question_fail(rn_state *r, const char *error) {
    if (!r->question_pending) return;
    if (r->question_tool_client >= 0 && r->question_tool_client < RN_MAX_CLIENTS)
        rn_send_control_result(r, r->question_tool_client, r->question_id, NULL, error);
    r->question_pending = false;
    r->question_done = true;
    r->question_failed = true;
}

static void rn_broadcast(rn_state *r, const buf_t *line) {
    for (int i = 0; i < RN_MAX_CLIENTS; i++) {
        rn_client *c = &r->cl[i];
        if (c->fd.borrow() >= 0 && c->handshaken && c->role != TNY_RUNNER_TOOL)
            rn_send_line(r, i, line->data, line->len);
    }
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
    tny_backend_event ev = {};
    ev.kind = TNY_EV_STATUS;
    ev.text = text;
    ev.text_len = strlen(text);
    rn_broadcast_event(r, &ev);
}

/* Drain the fd-2 tee: every complete line goes to task.log (stdout) and to
 * the clients as a `log` message — the pre-0053 terminal trail, live. */
static void rn_drain_errpipe(rn_state *r) {
    if (r->errpipe.borrow() < 0) return;
    char tmp[4096];
    for (;;) {
        ssize_t n = read(r->errpipe.borrow(), tmp, sizeof tmp);
        if (n > 0) {
            buf_append(&r->erracc, tmp, (size_t)n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        r->errpipe.reset(); /* nothing writes fd 2 anymore */
        break;
    }
    char *nl;
    while (r->erracc.len &&
           (nl = static_cast<char *>(memchr(r->erracc.data, '\n', r->erracc.len)))) {
        size_t linelen = (size_t)(nl - r->erracc.data);
        fwrite(r->erracc.data, 1, linelen + 1, stdout);
        buf_t b;
        buf_init(&b);
        buf_appends(&b, "{\"ev\":\"log\",\"text\":");
        char *owned = xstrndup(r->erracc.data, linelen);
        jescape(&b, owned ? owned : "");
        free(owned);
        buf_appends(&b, "}");
        if (!r->handoff_pending) rn_broadcast(r, &b);
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
    buf_appends(&b, ",\"permission_mode\":");
    jescape(&b, tny_perm_mode_name(r->ctx->perm_mode));
    buf_appends(&b, ",\"session_id\":");
    jescape(&b, r->session->id);
    buf_appends(&b, ",\"role\":");
    jescape(&b, rn_role_name(r->cl[i].role));
    buf_appendf(&b, ",\"turn_active\":%s}", r->turn_active ? "true" : "false");
    rn_send_line(r, i, b.data, b.len);
    buf_clear(&b);
    if (r->cl[i].role != TNY_RUNNER_TOOL && r->turn_active) {
        const buf_t *parts[] = {&r->thinking, &r->output};
        for (int part = 0; part < 2; part++) {
            for (size_t at = 0; at < parts[part]->len;) {
                size_t len = parts[part]->len - at;
                if (len > 32768) len = 32768;
                while (len && at + len < parts[part]->len &&
                       ((unsigned char)parts[part]->data[at + len] & 0xc0) == 0x80)
                    len--;
                char *piece = xstrndup(parts[part]->data + at, len);
                buf_clear(&b);
                buf_appendf(&b, "{\"ev\":\"%s\",\"text\":", part ? "snapshot" : "thinking");
                jescape(&b, piece);
                free(piece);
                buf_appends(&b, "}");
                rn_send_line(r, i, b.data, b.len);
                at += len;
            }
        }
    }
    buf_free(&b);
}

static void rn_accept(rn_state *r) {
    for (;;) {
        int slot = -1;
        for (int i = 0; i < RN_MAX_CLIENTS; i++)
            if (r->cl[i].fd.borrow() < 0) {
                slot = i;
                break;
            }
        tny::descriptor accepted;
        accepted.adopt(accept(r->lfd.borrow(), NULL, NULL));
        int fd = accepted.borrow();
        if (fd < 0) return;
        if (slot < 0) {
            /* full house; accepted closes on return */
            return;
        }
/* GCC 14's analyzer confuses the listener with the descriptor accept()
 * returned and reports r->lfd.borrow() leaking at the first operation on the new fd
 * (the same false trace src/net/http_server.c scopes off); the listener
 * lives for the runner's lifetime and the accepted fd is stored in cl[]. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif
        set_nonblock(fd, true);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        rn_client *c = &r->cl[slot];
        /* A reused slot must not inherit the previous connection's handshake
         * or role: every client handshakes for itself, and a tool client that
         * lands where an owner sat is still only a tool client. Sequential
         * tool-role control requests within one turn reuse slots routinely. */
        c->handshaken = false;
        c->role = TNY_RUNNER_UNHANDSHAKEN;
        c->can_answer_questions = false;
        c->fd.adopt(accepted.release());
        buf_init(&c->in);
        buf_init(&c->out);
        c->accepted_ms = now_ms();
    }
}

/* Finalize the turn that just ended (or failed): status + result,
 * then tell everyone. Safe with engine == NULL (early failures). */
static void rn_finalize(rn_state *r, tny_stop_reason stop, int exit_code) {
    if (r->question_pending) rn_question_fail(r, "turn ended before the question was answered");
    const char *stname = stop == TNY_STOP_DONE          ? "done"
                         : stop == TNY_STOP_INTERRUPTED ? "interrupted"
                                                        : "error";
    if (stop == TNY_STOP_DONE) session_recovery_clear(r->session);
    char *result = tny_turn_result_json(
        r->ctx, r->engine, r->session, r->output.data ? r->output.data : "",
        r->host_tools.len ? r->host_tools.data : NULL, r->ext_msgs.len ? r->ext_msgs.data : NULL,
        r->errline.len ? r->errline.data : NULL, exit_code);
    yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(r->session->doc), "continuation");
    session_set_status_finished(r->session, stname, exit_code, result);
    session_save(r->session);
    /* The runner still owns teardown writes and its socket. Both modes keep
     * the writer until final quiescence, immediately before bye (ADR0104). */
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
    r->background_armed = false;
    r->pending_perm[0] = 0;
    buf_clear(&r->output);
    buf_clear(&r->thinking);
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
    }
}

/* Fold one engine event into the recorder, into task.log (our stdio), and
 * onto the wire. The log keeps the pre-0053 background-child observability:
 * `tny session <id>` points readers at it for tool progress. */
static void rn_on_event(rn_state *r, const tny_backend_event *ev) {
    switch (ev->kind) {
    case TNY_EV_THINKING:
        buf_append(&r->thinking, ev->text, ev->text_len);
        rn_broadcast_event(r, ev);
        break;
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
        } else if (rn_owner(r) >= 0 || r->background_permissions) {
            snprintf(r->pending_perm, sizeof r->pending_perm, "%s", ev->perm_id ? ev->perm_id : "");
            free(r->permission_summary);
            r->permission_summary =
                xstrdup(ev->perm_summary ? ev->perm_summary : "Permission required");
            r->permission_options = ev->perm_options;
            r->permission_deadline = monotonic_ms() + 300000;
            rn_broadcast_event(r, ev); /* reattach may answer a parked decision */
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
    tny_engine_set_frontend_control(engine, rn_ask_user, r, rn_control_pump, r, r->sock_path,
                                    r->session->id);
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
    buf_clear(&r->thinking);
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
        /* Native cancel records preview non-delivery synchronously. Drain it
         * before freeing the engine, which otherwise discards those events. */
        tny_engine_cancel(r->engine);
        rn_drain_engine(r);
        tny_engine_preserve_session_on_free(r->engine);
        tny_engine_free(r->engine);
        r->engine = NULL;
    }
    if (r->turn_active) rn_finalize(r, TNY_STOP_INTERRUPTED, 130);
}

bool tny_runner_role_allows(tny_runner_role role, const char *op) {
    if (strcmp(op, "detach") == 0) return true;
    if (role == TNY_RUNNER_OWNER)
        return strcmp(op, "turn") == 0 || strcmp(op, "steer") == 0 || strcmp(op, "cancel") == 0 ||
               strcmp(op, "perm") == 0 || strcmp(op, "end") == 0 ||
               strcmp(op, "ask_user_reply") == 0 || strcmp(op, "background") == 0;
    if (role == TNY_RUNNER_TOOL)
        return strcmp(op, "ask_user") == 0 || strcmp(op, "image_attach") == 0 ||
               /* exactly one narrow addition (docs/adr/0096); owner control
                * (turn/cancel/perm/end) stays out of reach */
               strcmp(op, "image_preview") == 0;
    return false;
}

/* JSON strings can contain decoded NUL. Never interpret just their C prefix. */
static const char *rn_string(yyjson_val *root, const char *key, size_t max) {
    yyjson_val *value = jget(root, key);
    if (!yyjson_is_str(value)) return NULL;
    size_t len = yyjson_get_len(value);
    const char *str = yyjson_get_str(value);
    return len && len <= max && !memchr(str, '\0', len) ? str : NULL;
}

static void rn_op_error(rn_state *r, int ci, yyjson_val *root, const char *error) {
    const char *id = rn_string(root, "id", sizeof r->question_id - 1);
    if (id) {
        rn_send_control_result(r, ci, id, NULL, error);
        return;
    }
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"ev\":\"error\",\"text\":");
    jescape(&b, error);
    buf_appends(&b, "}");
    rn_send_line(r, ci, b.data, b.len);
    buf_free(&b);
}

static int rn_start_question(rn_state *r, int tool_client, const char *id, const char *question) {
    int owner = rn_owner(r);
    if (owner < 0 || !r->cl[owner].can_answer_questions) return -1;
    if (r->question_pending) return -2;
    r->question_pending = true;
    r->question_done = false;
    r->question_failed = false;
    r->question_tool_client = tool_client;
    r->question_answer = NULL;
    snprintf(r->question_id, sizeof r->question_id, "%s", id);
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"ev\":\"ask_user\",\"id\":");
    jescape(&b, r->question_id);
    buf_appends(&b, ",\"question\":");
    jescape(&b, question);
    buf_appends(&b, "}");
    rn_send_line(r, owner, b.data, b.len);
    buf_free(&b);
    return 0;
}

static void rn_handle_op(rn_state *r, int ci, yyjson_val *root) {
    const char *op = rn_string(root, "op", 32);
    if (!op) {
        rn_op_error(r, ci, root, "invalid control operation");
        return;
    }
    rn_client *client = &r->cl[ci];
    if (!client->handshaken) {
        if (strcmp(op, "hello") != 0) {
            rn_client_drop(r, ci);
            return;
        }
        const char *role = rn_string(root, "role", 16);
        tny_runner_role parsed = !role                           ? TNY_RUNNER_UNHANDSHAKEN
                                 : strcmp(role, "owner") == 0    ? TNY_RUNNER_OWNER
                                 : strcmp(role, "observer") == 0 ? TNY_RUNNER_OBSERVER
                                 : strcmp(role, "tool") == 0     ? TNY_RUNNER_TOOL
                                                                 : TNY_RUNNER_UNHANDSHAKEN;
        if (!parsed || (parsed == TNY_RUNNER_OWNER && rn_owner(r) >= 0)) {
            rn_client_drop(r, ci);
            return;
        }
        client->role = parsed;
        client->can_answer_questions =
            parsed == TNY_RUNNER_OWNER && jget_bool(root, "can_answer_questions", false);
        client->handshaken = true;
        r->had_client = true;
        rn_send_hello(r, ci);
        if (parsed == TNY_RUNNER_OWNER && r->pending_perm[0]) {
            tny_backend_event ev{};
            ev.kind = TNY_EV_PERMISSION;
            ev.perm_id = r->pending_perm;
            ev.perm_summary = r->permission_summary;
            ev.perm_options = r->permission_options;
            rn_broadcast_event(r, &ev);
        }
        return;
    }
    if (strcmp(op, "hello") == 0 || !tny_runner_role_allows(client->role, op)) {
        rn_op_error(r, ci, root, "operation is not allowed for this client role");
        return;
    }
    if ((jget(root, "id") && !rn_string(root, "id", sizeof r->question_id - 1)) ||
        (jget(root, "path") && !rn_string(root, "path", 4095))) {
        rn_op_error(r, ci, root, "control id/path must be bounded strings without NUL");
        return;
    }
    if (strcmp(op, "turn") == 0) {
        const char *prompt = jget_str(root, "prompt");
        if (!prompt || !*prompt) {
            rn_turn_err(r, "turn without a prompt", 1);
            return;
        }
        const char *images[17] = {};
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
            tny_backend_event ev = {};
            ev.kind = TNY_EV_STEER_REJECTED;
            ev.text = text;
            ev.text_len = strlen(text);
            rn_broadcast_event(r, &ev);
        }
    } else if (strcmp(op, "background") == 0) {
        if (!r->turn_active || !r->engine || r->ctx->backend != TNY_BK_OPENAI) {
            rn_op_error(r, ci, root, "background handoff requires an active native session runner");
        } else if (!r->background_armed) {
            /* Flag-only operation, also legal from the blocking-tool pump. */
            if (tny_engine_background(r->engine) == 0) {
                r->background_armed = true;
                rn_broadcast_status(r, "Background armed: after the next completed tool call");
            }
        }
    } else if (strcmp(op, "cancel") == 0) {
        r->background_armed = false;
        bool hard = jget_bool(root, "hard", false);
        if (r->control_pumping) {
            /* The blocking tool still owns the engine stack. Let its probe
             * unwind first; never cancel/free that stack from the nested pump. */
            g_rn_stop = 1;
            r->hard_cancel_pending |= hard;
        } else if (hard) rn_hard_cancel(r);
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
    } else if (strcmp(op, "ask_user") == 0) {
        const char *id = jget_str(root, "id");
        const char *question = jget_str(root, "question");
        if (!id || !*id || strlen(id) >= sizeof r->question_id || !question || !*question) {
            rn_op_error(r, ci, root, "ask_user needs a bounded string id and question");
            return;
        }
        int rc = rn_start_question(r, ci, id, question);
        if (rc == -1) rn_send_control_result(r, ci, id, NULL, "no interactive owner is attached");
        else if (rc == -2) rn_send_control_result(r, ci, id, NULL, "another question is pending");
    } else if (strcmp(op, "ask_user_reply") == 0) {
        const char *id = jget_str(root, "id");
        const char *answer = jget_str(root, "answer");
        if (!r->question_pending || !id || strcmp(id, r->question_id) != 0 || !answer) {
            rn_op_error(r, ci, root, "no matching question is pending");
            return;
        }
        if (r->question_tool_client >= 0)
            rn_send_control_result(r, r->question_tool_client, r->question_id, answer, NULL);
        else r->question_answer = xstrdup(answer);
        r->question_pending = false;
        r->question_done = true;
    } else if (strcmp(op, "image_attach") == 0) {
        const char *id = jget_str(root, "id");
        const char *path = jget_str(root, "path");
        if (!id || !*id || strlen(id) >= sizeof r->question_id || !path || !*path) {
            rn_op_error(r, ci, root, "image_attach needs a bounded string id and path");
            return;
        }
        char err[512];
        if (!r->turn_active || !r->engine ||
            tny_engine_queue_image(r->engine, path, err, sizeof err) != 0)
            rn_send_control_result(r, ci, id, NULL, !r->turn_active ? "no active turn" : err);
        else rn_send_control_result(r, ci, id, NULL, NULL);
    } else if (strcmp(op, "image_preview") == 0) {
        /* The explicitly requested generated-image preview (docs/adr/0096).
         * This is its own operation: it never falls back to image_attach, and
         * the receiver — not the caller — decides the status. */
        const char *id = rn_string(root, "id", sizeof r->question_id - 1);
        const char *path = rn_string(root, "path", 4095);
        const char *expected = rn_string(root, "expected_sha256", 64);
        yyjson_val *length = jget(root, "expected_bytes");
        bool valid_length = !length || (yyjson_is_uint(length) && yyjson_get_uint(length) > 0 &&
                                        yyjson_get_uint(length) <= TNY_IMAGE_OUTPUT_MAX);
        uint64_t expected_bytes = length && valid_length ? yyjson_get_uint(length) : 0;
        if (!valid_length || !id || !path || !expected ||
            yyjson_get_len(jget(root, "expected_sha256")) != 64 ||
            !tny_image_preview_hash_valid(expected)) {
            rn_op_error(r, ci, root,
                        "image_preview needs a bounded string id, a path and a 64-character "
                        "lowercase hex expected_sha256; supplied expected_bytes must be a positive "
                        "bounded integer");
            return;
        }
        char err[512] = "";
        const char *code = TNY_IMAGE_PREVIEW_CODE_NO_SESSION;
        tny_image_preview_status status = TNY_IMAGE_PREVIEW_UNAVAILABLE_SESSION;
        if (!r->turn_active || !r->engine)
            snprintf(err, sizeof err, "no active turn can take an image preview");
        else if (g_rn_stop || r->hard_cancel_pending) {
            status = TNY_IMAGE_PREVIEW_TURN_NOT_READY;
            code = TNY_IMAGE_PREVIEW_CODE_NOT_READY;
            snprintf(err, sizeof err, "the turn is cancelling");
        } else
            status = tny_engine_queue_image_preview(r->engine, path, expected, expected_bytes,
                                                    &code, err, sizeof err);
        bool queued = status == TNY_IMAGE_PREVIEW_QUEUED;
        rn_send_control_result_ex(r, ci, id, NULL, queued ? NULL : err,
                                  tny_image_preview_status_name(status), queued ? NULL : code);
    } else if (strcmp(op, "end") == 0) {
        if (r->turn_active) {
            r->end_after_turn = true;
            if (r->control_pumping) g_rn_stop = 1;
            else if (r->engine) tny_engine_cancel(r->engine);
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
    for (size_t bytes = 0; c->fd.borrow() >= 0 && bytes < 65536;) {
        ssize_t n = read(c->fd.borrow(), tmp, sizeof tmp);
        if (n > 0) {
            bytes += (size_t)n;
            buf_append(&c->in, tmp, (size_t)n);
            /* Parse before reading again: EOF after `cancel`/`end` must
             * not discard the owner's last commands. Limit each line,
             * not the combined size of a burst of valid messages. */
            char *nl;
            while (c->fd.borrow() >= 0 && c->in.len &&
                   (nl = static_cast<char *>(memchr(c->in.data, '\n', c->in.len)))) {
                size_t linelen = (size_t)(nl - c->in.data);
                if (linelen > RN_MAX_LINE) {
                    rn_client_drop(r, i);
                    return;
                }
                yyjson_doc *doc = jparse(c->in.data, linelen);
                buf_consume(&c->in, linelen + 1);
                if (!doc) continue;
                rn_handle_op(r, i, yyjson_doc_get_root(doc));
                yyjson_doc_free(doc);
            }
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
}

/* Bounded nested pump lent to a blocking terminal/ask-user tool. This fd set
 * intentionally contains only the listener and session clients: never the
 * backend, stderr tee, MCP, or extension fds. */
static int rn_control_pump(void *ud, int timeout_ms) {
    rn_state *r = static_cast<rn_state *>(ud);
    struct pollfd fds[1 + RN_MAX_CLIENTS];
    int cmap[RN_MAX_CLIENTS];
    nfds_t n = 0;
    int li = -1;
    if (rn_client_count(r) < RN_MAX_CLIENTS) {
        li = (int)n;
        fds[n++] = pollfd{r->lfd.borrow(), POLLIN, 0};
    }
    for (int i = 0; i < RN_MAX_CLIENTS; i++) {
        cmap[i] = -1;
        if (r->cl[i].fd.borrow() < 0) continue;
        cmap[i] = (int)n;
        fds[n++] =
            pollfd{r->cl[i].fd.borrow(), (short)(POLLIN | (r->cl[i].out.len ? POLLOUT : 0)), 0};
    }
    int pr = tny_poll(fds, n, timeout_ms < 0 ? 0 : timeout_ms);
    if (pr < 0 && errno != EINTR) return -1;
    if (li >= 0 && (fds[li].revents & POLLIN)) rn_accept(r);
    for (int i = 0; i < RN_MAX_CLIENTS; i++) {
        if (cmap[i] < 0 || r->cl[i].fd.borrow() < 0) continue;
        short re = fds[cmap[i]].revents;
        if (re & POLLOUT) rn_client_flush(r, i);
        if (r->cl[i].fd.borrow() >= 0 && (re & (POLLIN | POLLHUP | POLLERR))) {
            r->control_pumping = true;
            rn_client_read(r, i);
            r->control_pumping = false;
        }
        if (r->cl[i].fd.borrow() >= 0 && !r->cl[i].handshaken &&
            now_ms() - r->cl[i].accepted_ms > 5000)
            rn_client_drop(r, i);
    }
    return 0;
}

static char *rn_ask_user(const char *question, void *ud) {
    rn_state *r = static_cast<rn_state *>(ud);
    char *id = gen_id();
    if (!id) return NULL;
    int started = rn_start_question(r, -1, id, question);
    free(id);
    if (started != 0) return NULL;
    while (!r->question_done && !g_rn_stop) {
        if (rn_control_pump(r, 200) != 0) {
            rn_question_fail(r, "session control channel failed");
            break;
        }
    }
    if (g_rn_stop && r->question_pending) rn_question_fail(r, "turn interrupted");
    char *answer = r->question_failed ? NULL : r->question_answer;
    r->question_answer = NULL;
    r->question_pending = false;
    r->question_done = false;
    r->question_failed = false;
    r->question_id[0] = 0;
    return answer;
}

/* Restart protocol uses one anonymous socket for length-framed private JSON,
 * READY, GO (ownership), COMMITTED, RUN (side effects). Each wait is bounded.
 * The inherited flock/listener are never released or rebound during transfer. */
#define RN_RESTART_MAX (128u * 1024u * 1024u)

static int rn_transfer(int fd, void *bytes, size_t len, bool writing) {
    size_t off = 0;
    int64_t deadline = monotonic_ms() + 10000;
    while (off < len && monotonic_ms() < deadline && !g_rn_stop) {
        struct pollfd p = {fd, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
        int pr = tny_poll(&p, 1, 50);
        if (pr < 0 && errno != EINTR) return -1;
        if (!pr) continue;
        ssize_t n = writing ? write(fd, (char *)bytes + off, len - off)
                            : read(fd, (char *)bytes + off, len - off);
        if (n > 0) off += (size_t)n;
        else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) return -1;
    }
    return off == len ? 0 : -1;
}

static int rn_exchange_byte(int fd, char expected, bool writing) {
    char value = expected;
    return rn_transfer(fd, &value, 1, writing) == 0 && value == expected ? 0 : -1;
}

static void rn_background_marker(rn_state *r) {
    yyjson_mut_doc *d = r->session->doc;
    yyjson_mut_obj_put(yyjson_mut_doc_get_root(d), yyjson_mut_str(d, "background"),
                       yyjson_mut_bool(d, true));
    r->background = true;
}

static void rn_background_notice(rn_state *r) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "{\"ev\":\"backgrounded\",\"pid\":%ld}", (long)getpid());
    rn_broadcast(r, &b);
    buf_free(&b);
    r->background_armed = false;
}

static yyjson_mut_doc *rn_checkpoint(rn_state *r) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(jallocator());
    if (!d) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_val *engine = tny_engine_checkpoint(r->engine, d);
    if (!engine) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    yyjson_mut_obj_add_int(d, root, "version", 1);
    yyjson_mut_obj_add_val(d, root, "engine", engine);
    yyjson_mut_val *context = tny_checkpoint_context(d, r->ctx);
    if (!context || !yyjson_mut_obj_add_val(d, root, "context", context)) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    yyjson_mut_obj_add_strcpy(d, root, "session_id", r->session->id);
    yyjson_mut_obj_add_strcpy(d, root, "thinking", r->thinking.data ? r->thinking.data : "");
    yyjson_mut_obj_add_strcpy(d, root, "output", r->output.data ? r->output.data : "");
    yyjson_mut_obj_add_strcpy(d, root, "ext_msgs", r->ext_msgs.data ? r->ext_msgs.data : "");
    yyjson_mut_obj_add_uint(d, root, "event_sequence", r->session->extension_event_sequence);
    yyjson_mut_obj_add_uint(d, root, "agent_sequence", r->session->extension_agent_sequence);
    yyjson_mut_obj_add_bool(d, root, "session_started", r->session->extension_session_started);
    yyjson_mut_val *grants = yyjson_mut_arr(d);
    for (int i = 0; i < r->perm->n_grants; i++)
        yyjson_mut_arr_add_strcpy(d, grants, r->perm->grants[i]);
    yyjson_mut_obj_add_val(d, root, "grants", grants);
    int owner = rn_owner(r);
    yyjson_mut_obj_add_bool(d, root, "owner", owner >= 0);
    if (owner >= 0) {
        yyjson_mut_obj_add_strcpy(d, root, "client_in",
                                  r->cl[owner].in.data ? r->cl[owner].in.data : "");
        yyjson_mut_obj_add_strcpy(d, root, "client_out",
                                  r->cl[owner].out.data ? r->cl[owner].out.data : "");
        yyjson_mut_obj_add_bool(d, root, "questions", r->cl[owner].can_answer_questions);
    }
    /* Atomic checkpoint includes the full saved batch/results plus nonsecret
     * continuation state. Context and provider affinity NEVER go to disk. */
    yyjson_mut_doc *sd = r->session->doc;
    yyjson_mut_val *disk = yyjson_mut_val_mut_copy(sd, engine);
    yyjson_mut_obj_remove_key(yyjson_mut_obj_get(disk, "native"), "turn_state");
    yyjson_mut_val *resume = yyjson_mut_val_mut_copy(sd, root);
    yyjson_mut_obj_remove_key(resume, "engine");
    yyjson_mut_obj_remove_key(resume, "context");
    yyjson_mut_obj_remove_key(resume, "client_in");
    yyjson_mut_obj_remove_key(resume, "client_out");
    yyjson_mut_obj_put(resume, yyjson_mut_str(sd, "owner"), yyjson_mut_bool(sd, false));
    yyjson_mut_obj_add_bool(sd, resume, "resumable", true);
    yyjson_mut_val *public_context = tny_checkpoint_public(sd, r->ctx);
    if (!public_context) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    yyjson_mut_obj_add_val(sd, resume, "public_context", public_context);
    yyjson_mut_obj_add_val(sd, disk, "_resume", resume);
    yyjson_mut_obj_put(yyjson_mut_doc_get_root(sd), yyjson_mut_str(sd, "continuation"), disk);
    session_set_meta(r->session, tny_provider_name(r->ctx), r->ctx->model);
    if (!session_title(r->session)) {
        const char *prompt = yyjson_mut_get_str(yyjson_mut_obj_get(engine, "prompt_text"));
        session_set_title(r->session, prompt ? prompt : "Background agent");
    }
    if (session_save(r->session) != 0) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    /* Sync the checkpoint through the existing private-filesystem seam. */
    char *path = path_join(r->session->dir, "session.json");
    char *saved = jwrite(sd);
    int rc = !path || !saved ? -1 : tny_jobs_host_write_private(path, saved, strlen(saved));
    free(path);
    free(saved);
    if (rc != 0) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    return d;
}

static void rn_restart(rn_state *r) {
    rn_drain_errpipe(r); /* last wire drain before snapshotting client buffers */
    r->handoff_pending = true;
    yyjson_mut_doc *snapshot = rn_checkpoint(r);
    char *payload = snapshot ? jwrite(snapshot) : NULL;
    size_t len = payload ? strlen(payload) : 0;
    char *self = tny_process_self_path();
    int raw_pair[2] = {-1, -1};
    tny::descriptor pair[2];
    pid_t child = -1;
    bool released = false;
    yyjson_doc *recovery = payload ? jparse(payload, len) : NULL;
    int owner = rn_owner(r);
    char restart_arg[] = "--runner-restart";
    char *argv[] = {self, restart_arg, NULL};
    tny_fd_mapping maps[4];
    uint64_t size = len;
    if (!payload || !len || len > RN_RESTART_MAX || !self || !recovery ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, raw_pair) != 0)
        goto failed;
    for (int i = 0; i < 2; i++) {
        pair[i].adopt(raw_pair[i]);
        set_nonblock(pair[i].borrow(), true);
        fcntl(pair[i].borrow(), F_SETFD, FD_CLOEXEC);
    }
    maps[0] = {pair[1].borrow(), 3};
    maps[1] = {r->lfd.borrow(), 4};
    maps[2] = {r->session->lock_fd, 5};
    maps[3] = {owner >= 0 ? r->cl[owner].fd.borrow() : pair[1].borrow(), 6};
    if (tny_process_spawn_mapped(argv, environ, maps, owner >= 0 ? 4 : 3, &child) != 0) goto failed;
    pair[1].reset();
    if (rn_transfer(pair[0].borrow(), &size, sizeof size, true) != 0 ||
        rn_transfer(pair[0].borrow(), payload, len, true) != 0 ||
        rn_exchange_byte(pair[0].borrow(), 'R', false) != 0)
        goto failed;
    /* Child has validated/constructed the continuation but cannot mutate.
     * Parent quiesces every old writer before transferring mutation rights. */
    tny_engine_handoff_free(r->engine);
    r->engine = NULL;
    mcp_shutdown_all();
    tny_extensions_free(r->ctx->extensions);
    r->ctx->extensions = NULL;
    rn_drain_errpipe(r);
    fflush(NULL);
    released = true;
    if (rn_exchange_byte(pair[0].borrow(), 'G', true) != 0 ||
        rn_exchange_byte(pair[0].borrow(), 'C', false) != 0)
        goto failed;
    if (rn_exchange_byte(pair[0].borrow(), 'X', true) != 0) goto failed;
    /* RUN was released: no further session writes, unlock, unlink, or end
     * events from this process. The child owns all remaining tool effects. */
    _exit(0);
failed:
    r->handoff_pending = false;
    pair[0].reset();
    pair[1].reset();
    if (child > 0) {
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    }
    if (released) {
        char err[256];
        session_write_pid(r->session, getpid());
        mcp_warm_start(r->ctx);
        if (r->ctx->extensions_enabled)
            r->ctx->extensions =
                tny_extensions_new(r->ctx->tny_dir, r->ctx->cwd, r->ctx->extension_timeout_ms);
        if (rn_ensure_engine(r, err, sizeof err) != 0 ||
            tny_engine_restore(r->engine, jget(yyjson_doc_get_root(recovery), "engine")) != 0) {
            rn_broadcast_status(r,
                                "Background restart failed; saved continuation requires recovery");
            r->quit = true;
            r->turn_active = false; /* do not fabricate results for pending calls */
            r->quit_code = 2;
        }
    }
    r->background_armed = false;
    if (!r->quit) {
        yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(r->session->doc), "continuation");
        session_save(r->session);
        rn_broadcast_status(r, "Background restart failed; continuing in foreground");
        tny_engine_continue(r->engine);
    }
    yyjson_doc_free(recovery);
    yyjson_mut_doc_free(snapshot);
    if (payload) secure_free(payload);
    free(self);
}

static yyjson_mut_val *rn_continuation(tny_session_state *session) {
    return yyjson_mut_obj_get(yyjson_mut_doc_get_root(session->doc), "continuation");
}

static yyjson_doc *rn_disk_packet(tny_session_state *session) {
    yyjson_mut_val *engine = rn_continuation(session);
    yyjson_mut_val *resume = yyjson_mut_obj_get(engine, "_resume");
    if (!yyjson_mut_get_bool(yyjson_mut_obj_get(resume, "resumable"))) return NULL;
    yyjson_mut_doc *d = yyjson_mut_doc_new(jallocator());
    if (!d) return NULL;
    yyjson_mut_val *root = yyjson_mut_val_mut_copy(d, resume);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_val(d, root, "engine", yyjson_mut_val_mut_copy(d, engine));
    char *bytes = jwrite(d);
    yyjson_doc *packet = bytes ? jparse(bytes, strlen(bytes)) : NULL;
    free(bytes);
    yyjson_mut_doc_free(d);
    return packet;
}

static bool rn_consume_checkpoint(rn_state *r) {
    yyjson_mut_val *resume = yyjson_mut_obj_get(rn_continuation(r->session), "_resume");
    yyjson_mut_val *resumable = yyjson_mut_obj_get(resume, "resumable");
    if (!yyjson_mut_is_bool(resumable) || !yyjson_mut_get_bool(resumable) ||
        !yyjson_mut_set_bool(resumable, false))
        return false;
    if (session_save(r->session) == 0) return true;
    if (!yyjson_mut_set_bool(resumable, true)) return false;
    rn_broadcast_status(r, "Could not activate saved continuation; no pending tools were run");
    return false;
}

[[noreturn]] static void rn_child_main(tny_ctx *ctx, tny_session_state *session,
                                       const tny_runner_opts *opts, int lfd, char *sock_path,
                                       yyjson_val *restart) {
    /* Initial fork creates a session. A mapped restart already leads its
     * own group inside this detached session and has no controlling TTY. */
    if (!restart && setsid() < 0) _exit(2);
    bool from_disk = !restart && rn_continuation(session);
    yyjson_doc *disk_packet = from_disk ? rn_disk_packet(session) : NULL;
    if (from_disk && !disk_packet) _exit(2);
    if (disk_packet) restart = yyjson_doc_get_root(disk_packet);
    session->ctx = ctx;
    if (!restart) session_write_pid(session, getpid());
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
    tny::descriptor errpipe;
    {
        tny::pipe_pair tee;
        if (tee.open() == 0) {
            fcntl(tee.ends[0].borrow(), F_SETFD, FD_CLOEXEC);
            set_nonblock(tee.ends[0].borrow(), true);
            set_nonblock(tee.ends[1].borrow(), true);
            /* fd2 is deliberately process-lifetime stderr; the original pipe
             * ends remain scoped even when dup2 fails. */
            if (dup2(tee.ends[1].borrow(), 2) == 2) errpipe.adopt(tee.ends[0].release());
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
    if (!restart && ctx->backend == TNY_BK_OPENAI) mcp_warm_start(ctx);

    rn_state r{};

    r.ctx = ctx;
    r.session = session;
    r.serve = opts->serve;
    r.lfd.adopt(lfd);
    r.sock_path = sock_path;
    r.started_ms = now_ms();
    r.errpipe.adopt(errpipe.release());
    for (int i = 0; i < RN_MAX_CLIENTS; i++) r.cl[i].fd.adopt(-1);
    buf_init(&r.output);
    buf_init(&r.thinking);
    buf_init(&r.erracc);
    buf_init(&r.host_tools);
    buf_init(&r.ext_msgs);
    buf_init(&r.errline);
    r.perm = perm_new(ctx);
    r.quit_code = 0;
    r.background = yyjson_mut_get_bool(
        yyjson_mut_obj_get(yyjson_mut_doc_get_root(session->doc), "background"));
    if (restart) {
        r.background_permissions = true;
        char err[256];
        if (rn_ensure_engine(&r, err, sizeof err) != 0 ||
            tny_engine_restore(r.engine, jget(restart, "engine")) != 0)
            _exit(2);
        size_t gi, gn;
        yyjson_val *grant;
        yyjson_arr_foreach(jget(restart, "grants"), gi, gn, grant) {
            const char *g = yyjson_get_str(grant);
            if (!g) _exit(2);
            perm_grant(r.perm, g, NULL);
        }
        session->extension_event_sequence = (uint64_t)jget_int(restart, "event_sequence", 0);
        session->extension_agent_sequence = (uint64_t)jget_int(restart, "agent_sequence", 0);
        session->extension_session_started = jget_bool(restart, "session_started", false);
        buf_appends(&r.thinking,
                    jget_str(restart, "thinking") ? jget_str(restart, "thinking") : "");
        buf_appends(&r.output, jget_str(restart, "output") ? jget_str(restart, "output") : "");
        buf_appends(&r.ext_msgs,
                    jget_str(restart, "ext_msgs") ? jget_str(restart, "ext_msgs") : "");
        if (jget_bool(restart, "owner", false)) {
            r.cl[0].fd.adopt(6);
            r.cl[0].role = TNY_RUNNER_OWNER;
            r.cl[0].handshaken = true;
            r.cl[0].can_answer_questions = jget_bool(restart, "questions", false);
            buf_appends(&r.cl[0].in, jget_str(restart, "client_in"));
            buf_appends(&r.cl[0].out, jget_str(restart, "client_out"));
            fcntl(6, F_SETFD, FD_CLOEXEC);
        }
        r.turn_active = r.turn_ran = r.had_client = true;
        if (!from_disk &&
            (rn_exchange_byte(3, 'R', true) != 0 || rn_exchange_byte(3, 'G', false) != 0))
            _exit(2);
        rn_background_marker(&r);
        if (session_write_pid(session, getpid()) != 0 || session_save(session) != 0) _exit(2);
        if (!from_disk &&
            (rn_exchange_byte(3, 'C', true) != 0 || rn_exchange_byte(3, 'X', false) != 0))
            _exit(2);
        if (!from_disk) close(3);
        mcp_warm_start(ctx);
        rn_control_pump(&r, 0);
        if (g_rn_stop) {
            g_rn_stop = 0;
            tny_engine_cancel(r.engine);
        } else {
            if (!from_disk) rn_background_notice(&r);
            if (rn_consume_checkpoint(&r)) tny_engine_continue(r.engine);
            else {
                tny_engine_handoff_free(r.engine);
                r.engine = NULL;
                r.turn_active = false;
                r.quit = true;
                r.quit_code = 2;
            }
        }
        rn_drain_engine(&r);
    }

    /* spawn acquired the writer before binding; this child inherited it. */
    if (!restart && (r.serve || !opts->initial_prompt)) {
        /* the pre-warm, as a process: connect before any turn arrives (for
         * foreground once-mode this overlaps the caller reading stdin,
         * docs/adr/0004 decision 2) */
        char err[512];
        if (rn_ensure_engine(&r, err, sizeof err) != 0)
            fprintf(stderr, "tny-runner: warm-up: %s (will retry at the first turn)\n", err);
    }
    yyjson_doc_free(disk_packet);
    if (opts->initial_prompt) rn_background_marker(&r);
    if (opts->initial_prompt)
        rn_turn_begin(&r, opts->initial_prompt, opts->initial_images, opts->continue_recovery);

    while (!r.quit) {
        struct pollfd fds[2 + RN_MAX_CLIENTS + TNY_BACKEND_POLLFD_MAX];
        int cmap[RN_MAX_CLIENTS];
        nfds_t n = 0;
        int li = -1;
        if (rn_client_count(&r) < RN_MAX_CLIENTS) {
            li = (int)n;
            fds[n].fd = r.lfd.borrow();
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }
        for (int i = 0; i < RN_MAX_CLIENTS; i++) {
            cmap[i] = -1;
            if (r.cl[i].fd.borrow() < 0) continue;
            cmap[i] = (int)n;
            fds[n].fd = r.cl[i].fd.borrow();
            fds[n].events = POLLIN | (r.cl[i].out.len ? POLLOUT : 0);
            fds[n].revents = 0;
            n++;
        }
        int pi = -1;
        if (r.errpipe.borrow() >= 0) {
            pi = (int)n;
            fds[n].fd = r.errpipe.borrow();
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
            if (cmap[i] < 0 || r.cl[i].fd.borrow() < 0) continue;
            short re = fds[cmap[i]].revents;
            if (re & POLLOUT) rn_client_flush(&r, i);
            if (r.cl[i].fd.borrow() >= 0 && (re & (POLLIN | POLLHUP | POLLERR)))
                rn_client_read(&r, i);
            if (r.cl[i].fd.borrow() >= 0 && !r.cl[i].handshaken &&
                now_ms() - r.cl[i].accepted_ms > 5000)
                rn_client_drop(&r, i);
        }
        if (pi >= 0 && (fds[pi].revents & (POLLIN | POLLHUP))) rn_drain_errpipe(&r);
        if (r.turn_active && r.engine) {
            tny_engine_dispatch(r.engine, fds + ei, ne);
            rn_drain_engine(&r);
            rn_drain_errpipe(&r); /* forward what the dispatch just printed */
        }
        if (r.turn_active && r.engine && tny_engine_parked(r.engine)) rn_restart(&r);
        if (r.background_permissions && r.pending_perm[0] &&
            monotonic_ms() >= r.permission_deadline) {
            rn_broadcast_status(&r, "Background permission timed out waiting for reattachment");
            char id[sizeof r.pending_perm];
            snprintf(id, sizeof id, "%s", r.pending_perm);
            r.pending_perm[0] = 0;
            tny_engine_respond_permission(r.engine, id, TNY_PERM_DECISION_DENY);
            rn_drain_engine(&r);
        }
        if (r.hard_cancel_pending) {
            r.hard_cancel_pending = false;
            rn_hard_cancel(&r);
        }
        if (r.turn_ended) {
            int code = r.stop == TNY_STOP_DONE ? 0 : r.stop == TNY_STOP_INTERRUPTED ? 130 : 2;
            bool open_list = r.background_armed && r.stop == TNY_STOP_DONE;
            if (open_list) rn_background_marker(&r);
            rn_finalize(&r, r.stop, code);
            if (open_list) rn_background_notice(&r);
        }
        if (r.serve && r.had_client && !r.turn_active && rn_frontend_count(&r) == 0)
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
    mcp_shutdown_all();
    session_save(r.session);
    rn_drain_errpipe(&r);
    fflush(NULL); /* task.log is complete before anyone hears bye */
    r.lfd.reset();
    unlink(r.sock_path); /* last session-dir mutation: bye promises quiescence */
    session_lock_release(r.session);
    {
        buf_t b;
        buf_init(&b);
        buf_appends(&b, "{\"ev\":\"bye\",\"text\":\"runner exiting\"}");
        rn_broadcast(&r, &b);
        buf_free(&b);
    }
    rn_flush_before_exit(&r);
    for (int i = 0; i < RN_MAX_CLIENTS; i++) {
        rn_client_flush(&r, i);
        rn_client_drop(&r, i);
    }
    perm_free(r.perm);
    buf_free(&r.output);
    buf_free(&r.thinking);
    free(r.permission_summary);
    buf_free(&r.host_tools);
    buf_free(&r.ext_msgs);
    buf_free(&r.errline);
    buf_free(&r.erracc);
    free(r.question_answer);
    r.errpipe.reset();
    _exit(r.quit_code);
}

pid_t tny_runner_spawn(tny_ctx *ctx, tny_session_state *session, const tny_runner_opts *opts,
                       char *err, size_t errlen) {
    if (!session || ctx->no_save) {
        snprintf(err, errlen, "isolation needs a saved session");
        return -1;
    }
    tny_ctx *original_ctx = ctx;
    bool acquired_here = session->lock_fd < 0;
    if (session_lock_acquire(session) != 0) {
        snprintf(err, errlen, "session is locked by another process");
        return -1;
    }
    tny::spawn_writer writer(session, acquired_here);
    if (acquired_here && session->persisted && session_reload_locked(session, err, errlen) != 0) {
        return -1;
    }
    yyjson_mut_val *pending = rn_continuation(session);
    if (pending) {
        yyjson_doc *packet = opts->serve && !opts->initial_prompt ? rn_disk_packet(session) : NULL;
        tny_ctx *restored =
            packet
                ? tny_checkpoint_recover(ctx, jget(yyjson_doc_get_root(packet), "public_context"))
                : NULL;
        yyjson_doc_free(packet);
        if (!restored) {
            snprintf(
                err, errlen,
                "checkpoint cannot be replayed with this configuration or after activation; use "
                "tny resume with its original provider/configuration for an unconsumed checkpoint");
            return -1;
        }
        ctx = restored;
    }
    /* Publish a genuinely new snapshot before fork, so the parent also knows
     * that later runners must reload it, even if this child ends before a turn. */
    if (!session->persisted && session_save(session) != 0) {
        snprintf(err, errlen, "cannot write new session");
        if (ctx != original_ctx) tny_ctx_free(ctx);
        return -1;
    }
    char *sock = tny_runner_sock_path(session->dir);
    if (!sock) {
        snprintf(err, errlen, "session path too long for a unix socket");
        if (ctx != original_ctx) tny_ctx_free(ctx);
        return -1;
    }
    tny::descriptor listener;
    listener.adopt(unix_listen(sock));
    int lfd = listener.borrow();
    if (lfd < 0) {
        snprintf(err, errlen, "cannot listen on %s", sock);
        free(sock);
        if (ctx != original_ctx) tny_ctx_free(ctx);
        return -1;
    }
    fcntl(lfd, F_SETFD, FD_CLOEXEC);
    fflush(NULL); /* buffered stdio must not replay into task.log */
    pid_t pid = fork();
    if (pid < 0) {
        unlink(sock);
        free(sock);
        snprintf(err, errlen, "fork failed");
        if (ctx != original_ctx) tny_ctx_free(ctx);
        return -1;
    }
    if (pid > 0) {
        free(sock);
        if (ctx != original_ctx) tny_ctx_free(ctx);
        return pid;
    }
    writer.release();
    rn_child_main(ctx, session, opts, listener.release(), sock, NULL);
}

int tny_runner_restart_main(void) {
    tny::descriptor channel, listener, owner_client;
    tny::lock_descriptor writer;
    channel.adopt(3);
    listener.adopt(4);
    writer.adopt(5);
    if (fcntl(6, F_GETFD) >= 0) owner_client.adopt(6);
    signal(SIGPIPE, SIG_IGN);
    set_nonblock(3, true);
    uint64_t len = 0;
    if (rn_transfer(3, &len, sizeof len, false) != 0 || !len || len > RN_RESTART_MAX) return 2;
    char *bytes = static_cast<char *>(tny_alloc_malloc((size_t)len + 1));
    if (!bytes || rn_transfer(3, bytes, (size_t)len, false) != 0) {
        free(bytes);
        return 2;
    }
    bytes[len] = 0;
    yyjson_doc *d = jparse(bytes, (size_t)len);
    secure_free(bytes);
    yyjson_val *r = d ? yyjson_doc_get_root(d) : NULL;
    if (jget_int(r, "version", 0) != 1) {
        yyjson_doc_free(d);
        return 2;
    }
    tny_ctx *ctx = tny_checkpoint_context_restore(jget(r, "context"));
    const char *sid = jget_str(r, "session_id");
    tny_session_state *session = ctx && sid ? session_open(ctx, sid) : NULL;
    char *lock = session ? path_join(session->dir, "lock") : NULL;
    bool valid = lock && tny_jobs_host_fd_is_file(5, lock) &&
                 tny_jobs_host_lock_try(5) == TNY_JOBS_LOCK_ACQUIRED;
    free(lock);
    if (!valid) {
        session_close(session);
        tny_ctx_free(ctx);
        yyjson_doc_free(d);
        return 2;
    }
    session->lock_fd = writer.release();
    fcntl(5, F_SETFD, FD_CLOEXEC);
    fcntl(4, F_SETFD, FD_CLOEXEC);
    char *sock = tny_runner_sock_path(session->dir);
    if (!sock || chdir(ctx->cwd) != 0) {
        free(sock);
        session_close(session);
        tny_ctx_free(ctx);
        yyjson_doc_free(d);
        return 2;
    }
    tny_runner_opts opts{};
    opts.serve = true;
    (void)channel.release(); /* child protocol consumes fd3 */
    if (jget_bool(r, "owner", false)) (void)owner_client.release();
    rn_child_main(ctx, session, &opts, listener.release(), sock, r);
}

/* ---- client ---- */

struct tny_runner_client {
    tny::descriptor fd;
    buf_t in;
    tny_runner_msg *head, *tail;
    bool dead;
};

static int rc_send(tny_runner_client *c, const buf_t *line);

tny_runner_client *tny_runner_client_connect(const char *sock_path, int timeout_ms,
                                             tny_runner_role role, bool can_answer_questions) {
    int64_t deadline = monotonic_ms() + (timeout_ms > 0 ? timeout_ms : 0);
    int fd = -1;
    for (;;) {
        fd = unix_connect(sock_path);
        if (fd >= 0) break;
        if (monotonic_ms() >= deadline) return NULL;
        struct pollfd none = {-1, 0, 0};
        tny_poll(&none, 1, 50); /* bounded retry sleep through the seam */
    }
    tny::descriptor connection;
    connection.adopt(fd);
    tny::owned<tny_runner_client> owned;
    try {
        owned = tny::make_owned<tny_runner_client>();
    } catch (const std::bad_alloc &) { return NULL; }
    tny_runner_client *c = owned.get();
    c->fd.adopt(connection.release());
    buf_init(&c->in);
    buf_t hello;
    buf_init(&hello);
    buf_appends(&hello, "{\"op\":\"hello\",\"role\":");
    jescape(&hello, rn_role_name(role));
    if (role == TNY_RUNNER_OWNER && can_answer_questions)
        buf_appends(&hello, ",\"can_answer_questions\":true");
    buf_appends(&hello, "}\n");
    if (rc_send(c, &hello) != 0) {
        buf_free(&hello);
        tny_runner_client_close(owned.release());
        return NULL;
    }
    buf_free(&hello);
    return owned.release();
}

int tny_runner_client_fd(const tny_runner_client *c) { return c ? c->fd.borrow() : -1; }

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
    tny_runner_msg *m = static_cast<tny_runner_msg *>(tny_alloc_calloc(1, sizeof *m));
    if (!m) {
        yyjson_doc_free(doc);
        return;
    }
    m->doc = doc;
    if (strcmp(ev, "backgrounded") == 0) {
        m->kind = TNY_RMSG_BACKGROUNDED;
        m->pid = (pid_t)jget_int(root, "pid", 0);
    } else if (strcmp(ev, "hello") == 0) {
        m->kind = TNY_RMSG_HELLO;
        m->pid = (pid_t)jget_int(root, "pid", -1);
        m->provider = (char *)jget_str(root, "provider");
        m->model = (char *)jget_str(root, "model");
        m->turn_active = jget_bool(root, "turn_active", false);
        const char *mode = jget_str(root, "permission_mode");
        m->perm_mode = mode && strcmp(mode, "yolo") == 0   ? TNY_MODE_YOLO
                       : mode && strcmp(mode, "auto") == 0 ? TNY_MODE_AUTO
                                                           : TNY_MODE_ASK;
    } else if (strcmp(ev, "snapshot") == 0) {
        m->kind = TNY_RMSG_SNAPSHOT;
        m->text = (char *)jget_str(root, "text");
    } else if (strcmp(ev, "recovery") == 0) {
        m->kind = TNY_RMSG_RECOVERY;
        m->text = (char *)jget_str(root, "text");
    } else if (strcmp(ev, "log") == 0) {
        m->kind = TNY_RMSG_LOG;
        m->text = (char *)jget_str(root, "text");
    } else if (strcmp(ev, "ask_user") == 0) {
        m->kind = TNY_RMSG_ASK_USER;
        m->id = (char *)jget_str(root, "id");
        m->text = (char *)jget_str(root, "question");
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
    if (!c || c->fd.borrow() < 0) return -1;
    char tmp[8192];
    /* A hot runner must not monopolize the renderer's input loop. Parse
     * each read so a burst of small lines cannot trip the per-line cap. */
    for (size_t bytes = 0; !c->dead && bytes < 65536;) {
        ssize_t n = read(c->fd.borrow(), tmp, sizeof tmp);
        if (n > 0) {
            bytes += (size_t)n;
            buf_append(&c->in, tmp, (size_t)n);
            char *nl;
            while (c->in.len && (nl = static_cast<char *>(memchr(c->in.data, '\n', c->in.len)))) {
                size_t linelen = (size_t)(nl - c->in.data);
                if (linelen > RN_MAX_LINE) {
                    c->dead = true;
                    break;
                }
                rc_parse_line(c, c->in.data, linelen);
                buf_consume(&c->in, linelen + 1);
            }
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
    if (!c || c->fd.borrow() < 0 || c->dead) return -1;
    size_t off = 0;
    while (off < line->len) {
        ssize_t n = write(c->fd.borrow(), line->data + off, line->len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = {c->fd.borrow(), POLLOUT, 0};
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

int tny_runner_client_background(tny_runner_client *c) {
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"op\":\"background\"}\n");
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

int tny_runner_client_ask_user_reply(tny_runner_client *c, const char *id, const char *answer) {
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"op\":\"ask_user_reply\",\"id\":");
    jescape(&b, id ? id : "");
    buf_appends(&b, ",\"answer\":");
    jescape(&b, answer ? answer : "");
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
    if (c->fd.borrow() >= 0) c->fd.reset();
    buf_free(&c->in);
    tny_runner_msg *m = c->head;
    while (m) {
        tny_runner_msg *next = m->next;
        tny_runner_msg_free(m);
        m = next;
    }
    tny::owned<tny_runner_client> owned(c);
}

#else /* __EMSCRIPTEN__: clean-error stubs (docs/adr/0017, 0053) */

int tny_runner_restart_main(void) { return 1; }
int tny_runner_client_background(tny_runner_client *c) {
    (void)c;
    return -1;
}

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
tny_runner_client *tny_runner_client_connect(const char *sock_path, int timeout_ms,
                                             tny_runner_role role, bool can_answer_questions) {
    (void)sock_path;
    (void)timeout_ms;
    (void)role;
    (void)can_answer_questions;
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
int tny_runner_client_ask_user_reply(tny_runner_client *c, const char *id, const char *answer) {
    (void)c;
    (void)id;
    (void)answer;
    return -1;
}
int tny_runner_client_end(tny_runner_client *c, const char *reason) {
    (void)c;
    (void)reason;
    return -1;
}
void tny_runner_client_close(tny_runner_client *c) { (void)c; }

#endif
