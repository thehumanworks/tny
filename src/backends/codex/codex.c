/* codex.c — `codex app-server` over WebSockets (docs/backends/codex-app-server.md).
 * tny is a protocol client here: the host owns tools, sandbox and planning;
 * we own the transport, the approvals surface and the normalized event set.
 * Nothing is spawned before connect(): --help/--version stay microseconds.
 * Framing, the request queue and the pump live in codex_rpc.c. */
#include "backends/codex/codex.h"

#include <poll.h>
#include "util/tny_poll.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CX_CONNECT_TIMEOUT_MS 15000 /* spawn -> listening */
#define CX_ATTACH_TIMEOUT_MS  5000
#define CX_DISCOVER_TIMEOUT_MS 2000 /* opportunistic attach; refusal is instant */
#define CX_RPC_TIMEOUT_MS     30000
#define CX_CANCEL_GRACE_MS    3000

/* ---------- connect ---------- */

static void cx_load_token(cx_impl *o, char *err, size_t errlen, bool *failed) {
    *failed = false;
    if (o->ctx->ws_token_file && *o->ctx->ws_token_file) {
        size_t len = 0;
        char *data = file_slurp(o->ctx->ws_token_file, &len);
        if (!data) {
            snprintf(err, errlen, "codex: cannot read --ws-token-file %s",
                     o->ctx->ws_token_file);
            *failed = true;
            return;
        }
        o->token = xstrdup(str_trim(data));
        secure_free(data);
        return;
    }
    const char *env = getenv("CODEX_REMOTE_TOKEN");
    if (env && *env) o->token = xstrdup(env);
}

static int cx_handshake(cx_impl *o, char *err, size_t errlen) {
    buf_t p;
    buf_init(&p);
    buf_appends(&p, "{\"clientInfo\":{\"name\":\"tny\",\"title\":\"tny\",\"version\":\""
                    TNY_VERSION "\"}}");
    yyjson_doc *doc = cx_request_sync(o, "initialize", p.data, CX_RPC_TIMEOUT_MS,
                                      err, errlen);
    buf_free(&p);
    if (!doc) return -1;
    yyjson_doc_free(doc);
    cx_notify(o, "initialized", NULL);
    if (cx_flush(o) != 0) {
        snprintf(err, errlen, "codex: connection lost after initialize");
        return -1;
    }
    return 0;
}

/* Attach to an already-running app-server if one is discoverable: the
 * TNY_CODEX_WS env var, else ~/.tny/codex-host.json published by a live tny
 * (a long-lived TUI keeps its host warm; one-shot asks reuse it and skip the
 * ~1 s spawn+initialize). Any failure — dead port, refused upgrade, failed
 * handshake — falls through silently to a fresh spawn, whose registry write
 * then replaces the stale entry. Attached hosts are never ours to kill:
 * o->child stays 0, so cx_stop_child and the pgroup sweep leave them alone. */
static bool cx_try_discovered(cx_impl *o) {
    char *url = NULL;
    const char *env = getenv("TNY_CODEX_WS");
    if (env && *env) url = xstrdup(env);
    else if (cx_registry_load(&url, NULL) != 0) return false;
    char err[256];
    o->ws = ws_connect(url, o->token, CX_DISCOVER_TIMEOUT_MS, err, sizeof err);
    if (o->ws && cx_handshake(o, err, sizeof err) != 0) {
        ws_close(o->ws);
        o->ws = NULL;
    }
    if (!o->ws) {
        if (!o->ctx->library_mode && tny_debug())
            fprintf(stderr, "tny: no reusable codex host at %s (%s); spawning\n",
                    url, err);
        free(url);
        return false;
    }
    o->ws_url = url;
    return true;
}

static int cx_connect(tny_backend *b, char *errbuf, size_t errlen) {
    cx_impl *o = b->impl;
    if (o->ws) return 0;

    bool failed = false;
    cx_load_token(o, errbuf, errlen, &failed);
    if (failed) return -1;

    char err[256];
    if (o->ctx->codex_ws && *o->ctx->codex_ws) {
        o->ws_url = xstrdup(o->ctx->codex_ws);
        o->ws = ws_connect(o->ws_url, o->token, CX_ATTACH_TIMEOUT_MS, err, sizeof err);
        if (!o->ws) {
            snprintf(errbuf, errlen, "codex: cannot attach to %s (%s). Is "
                     "`codex app-server --listen` running?", o->ws_url, err);
            return -1;
        }
    } else {
        if (cx_try_discovered(o)) return 0; /* shared host; nothing spawned */
        int port = cx_pick_port();
        if (port <= 0) {
            snprintf(errbuf, errlen, "codex: no free loopback port for the app-server");
            return -1;
        }
        buf_t u;
        buf_init(&u);
        buf_appendf(&u, "ws://127.0.0.1:%d", port);
        o->ws_url = buf_detach(&u);
        if (cx_spawn(o, port, errbuf, errlen) != 0) return -1;
        int64_t deadline = now_ms() + CX_CONNECT_TIMEOUT_MS;
        while (now_ms() < deadline) {
            cx_drain_child_stderr(o);
            if (cx_child_gone(o)) {
                /* the host's last words help debugging but would scribble over
                 * the TUI (and the pre-warm thread must stay silent) */
                if (o->stderr_tail.len && tny_debug())
                    fwrite(o->stderr_tail.data, 1, o->stderr_tail.len, stderr);
                snprintf(errbuf, errlen,
                         "codex: `%s app-server` exited during startup "
                         "(try `codex --version`)",
                         o->ctx->codex_bin ? o->ctx->codex_bin : "codex");
                cx_stop_child(o);
                return -1;
            }
            o->ws = ws_connect(o->ws_url, NULL, 1000, err, sizeof err);
            if (o->ws) break;
            tny_poll(NULL, 0, 150);
        }
        if (!o->ws) {
            snprintf(errbuf, errlen, "codex: app-server did not accept a WebSocket on "
                     "%s within %d s", o->ws_url, CX_CONNECT_TIMEOUT_MS / 1000);
            cx_stop_child(o);
            return -1;
        }
    }
    if (cx_handshake(o, errbuf, errlen) != 0) {
        ws_close(o->ws);
        o->ws = NULL;
        cx_stop_child(o);
        return -1;
    }
    /* the host is fully up: publish it so one-shot runs attach instead of
     * spawning their own (best effort; a failed write just costs them that) */
    if (o->child > 0 && cx_registry_write(o->ws_url, o->child) == 0)
        o->wrote_registry = true;
    return 0;
}

static void cx_disconnect(tny_backend *b) {
    cx_impl *o = b->impl;
    if (o->ws) {
        if (o->turn_active && o->thread_id && !o->dead) {
            /* be polite: ask the host to stop before the socket goes away */
            buf_t p;
            buf_init(&p);
            buf_appends(&p, "{\"threadId\":");
            jescape(&p, o->thread_id);
            if (o->turn_id) { buf_appends(&p, ",\"turnId\":"); jescape(&p, o->turn_id); }
            buf_appends(&p, "}");
            cx_request(o, "turn/interrupt", p.data, CXR_FREE);
            buf_free(&p);
            cx_flush(o);
        }
        ws_close(o->ws); /* sends the RFC6455 close frame */
        o->ws = NULL;
    }
    o->turn_active = false;
    cx_stop_child(o);
}

/* ---------- thread lifecycle ---------- */

static const char *thread_id_of(yyjson_val *res) {
    const char *tid = jget_str(jget(res, "thread"), "id");
    if (!tid) tid = jget_str(res, "threadId");
    if (!tid) tid = jget_str(res, "thread_id");
    if (!tid) tid = jget_str(res, "id");
    return tid;
}

static int cx_start_thread(cx_impl *o, const char *resume, char *err, size_t errlen) {
    buf_t p;
    buf_init(&p);
    const char *method;
    if (resume) {
        method = "thread/resume";
        buf_appends(&p, "{\"threadId\":");
        jescape(&p, resume);
        buf_appends(&p, "}");
    } else {
        method = "thread/start";
        buf_appends(&p, "{");
        bool first = true;
        if (o->ctx->model && *o->ctx->model) {
            buf_appends(&p, "\"model\":");
            jescape(&p, o->ctx->model);
            first = false;
        }
        /* TNY_CAP_FAST: "priority" is the paid fast tier ("fast" is the
         * renamed alias — send "priority", the value every app-server
         * release accepts). The host ignores unknown values. */
        if (o->ctx->service_tier && *o->ctx->service_tier) {
            if (!first) buf_appends(&p, ",");
            buf_appends(&p, "\"serviceTier\":");
            jescape(&p, tny_tier_is_fast(o->ctx->service_tier)
                            ? "priority" : o->ctx->service_tier);
        }
        buf_appends(&p, "}");
    }
    yyjson_doc *doc = cx_request_sync(o, method, p.data, CX_RPC_TIMEOUT_MS, err, errlen);
    buf_free(&p);
    if (!doc) return -1;
    const char *tid = thread_id_of(jget(yyjson_doc_get_root(doc), "result"));
    if (!tid || !*tid) {
        snprintf(err, errlen, "codex: %s returned no thread id", method);
        yyjson_doc_free(doc);
        return -1;
    }
    free(o->thread_id);
    o->thread_id = xstrdup(tid);
    yyjson_doc_free(doc);
    return 0;
}

static int cx_create_or_resume(tny_backend *b, const char *ptr, char *e, size_t el) {
    cx_impl *o = b->impl;
    if (!o->ws) { snprintf(e, el, "codex: not connected"); return -1; }
    if (ptr && *ptr) {
        if (cx_start_thread(o, ptr, e, el) == 0) return 0;
        if (!o->ctx->library_mode && tny_debug())
            fprintf(stderr, "tny: %s; starting a fresh codex thread\n", e);
    }
    return cx_start_thread(o, NULL, e, el);
}

static char *cx_session_pointer(tny_backend *b) {
    cx_impl *o = b->impl;
    return o->thread_id ? xstrdup(o->thread_id) : NULL;
}

/* ---------- turn ---------- */

static int cx_send(tny_backend *b, const char *prompt, const char **images,
                   tny_backend_event_cb cb, void *ud, char *errbuf, size_t errlen) {
    cx_impl *o = b->impl;
    o->cb = cb;
    o->ud = ud;
    if (!o->ws || o->dead) { snprintf(errbuf, errlen, "codex: not connected"); return -1; }
    if (!o->thread_id) { snprintf(errbuf, errlen, "codex: no thread started"); return -1; }
    if (images && images[0])
        cx_emit_text(o, TNY_EV_STATUS,
                     "codex backend: --image is not carried over app-server yet; "
                     "the images were ignored");

    buf_t p;
    buf_init(&p);
    buf_appends(&p, "{\"threadId\":");
    jescape(&p, o->thread_id);
    buf_appends(&p, ",\"input\":[{\"type\":\"text\",\"text\":");
    jescape(&p, prompt ? prompt : "");
    buf_appends(&p, ",\"text_elements\":[]}]");
    /* turn/start.effort overrides "for this turn and subsequent turns", so a
     * mid-conversation /effort needs no thread restart. Supported values come
     * from model/list supportedReasoningEfforts (see cx_list_models). */
    if (o->ctx->reasoning_effort && *o->ctx->reasoning_effort) {
        buf_appends(&p, ",\"effort\":");
        jescape(&p, tny_effort_wire(TNY_BK_CODEX, o->ctx->reasoning_effort));
    }
    buf_appends(&p, "}");

    free(o->turn_id);
    o->turn_id = NULL;
    o->turn_active = true;
    o->cancel_sent = false;
    int id = cx_request(o, "turn/start", p.data, CXR_TURN);
    buf_free(&p);
    if (id < 0) {
        o->turn_active = false;
        snprintf(errbuf, errlen, "codex: too many requests in flight");
        return -1;
    }
    if (cx_flush(o) != 0) {
        o->turn_active = false;
        snprintf(errbuf, errlen, "codex: failed to send turn/start");
        return -1;
    }
    return 0;
}

/* turn/steer (docs/adr/0011): more user input for the active turn. Needs
 * the turn id from the turn/start response; before that the caller queues. */
static int cx_steer(tny_backend *b, const char *text, char *errbuf, size_t errlen) {
    cx_impl *o = b->impl;
    if (!o->turn_active || !o->ws || o->dead || o->cancel_sent) {
        snprintf(errbuf, errlen, "codex: no steerable turn");
        return -1;
    }
    if (!o->turn_id || !o->thread_id) {
        snprintf(errbuf, errlen, "codex: turn id not known yet");
        return -1;
    }
    buf_t p;
    buf_init(&p);
    buf_appends(&p, "{\"threadId\":");
    jescape(&p, o->thread_id);
    buf_appends(&p, ",\"expectedTurnId\":");
    jescape(&p, o->turn_id);
    buf_appends(&p, ",\"input\":[{\"type\":\"text\",\"text\":");
    jescape(&p, text ? text : "");
    buf_appends(&p, ",\"text_elements\":[]}]}");
    int id = cx_request(o, "turn/steer", p.data, CXR_STEER);
    buf_free(&p);
    if (id < 0) {
        /* no pending slot means no rejection could ever come back: refuse
         * now so the caller queues the text instead (docs/adr/0013) */
        snprintf(errbuf, errlen, "codex: too many requests in flight");
        return -1;
    }
    cx_pending *pd = cx_pending_find(o, id);
    if (pd) pd->steer_text = xstrdup(text ? text : "");
    if (cx_flush(o) != 0) {
        /* the frame never left: drop the pending too, or the turn-end sweep
         * would re-queue a text the caller is already queueing */
        if (pd) cx_pending_clear(pd);
        snprintf(errbuf, errlen, "codex: failed to send turn/steer");
        return -1;
    }
    return 0;
}

static void cx_cancel(tny_backend *b) {
    cx_impl *o = b->impl;
    if (!o->turn_active || o->cancel_sent || !o->ws) return;
    buf_t p;
    buf_init(&p);
    buf_appends(&p, "{\"threadId\":");
    jescape(&p, o->thread_id ? o->thread_id : "");
    if (o->turn_id) { buf_appends(&p, ",\"turnId\":"); jescape(&p, o->turn_id); }
    buf_appends(&p, "}");
    /* if every pending slot is busy, send it untracked: losing the response
     * mapping is better than not interrupting at all */
    if (cx_request(o, "turn/interrupt", p.data, CXR_INTERRUPT) < 0)
        cx_request(o, "turn/interrupt", p.data, CXR_FREE);
    buf_free(&p);
    cx_flush(o);
    o->cancel_sent = true;
    o->cancel_deadline = now_ms() + CX_CANCEL_GRACE_MS;
    cx_emit_text(o, TNY_EV_STATUS, "interrupting the codex turn…");
}

static void cx_respond_permission(tny_backend *b, const char *perm_id,
                                  tny_perm_decision d) {
    cx_impl *o = b->impl;
    if (!perm_id) return;
    for (int i = 0; i < CX_MAX_APPROVALS; i++) {
        if (!o->approvals[i].id || strcmp(o->approvals[i].id, perm_id) != 0) continue;
        const char *decision = d == TNY_PERM_DECISION_ALLOW          ? "accept"
                               : d == TNY_PERM_DECISION_ALLOW_ALWAYS ? "acceptForSession"
                                                                     : "decline";
        buf_t r;
        buf_init(&r);
        buf_appendf(&r, "{\"decision\":\"%s\"}", decision);
        cx_respond_result(o, o->approvals[i].id, r.data);
        buf_free(&r);
        free(o->approvals[i].id);
        free(o->approvals[i].method);
        o->approvals[i].id = NULL;
        o->approvals[i].method = NULL;
        cx_flush(o);
        return;
    }
}

/* ---------- loop integration ---------- */

static int cx_pollfds(tny_backend *b, struct pollfd *fds, int max) {
    cx_impl *o = b->impl;
    int n = 0;
    if (o->ws && n < max) {
        fds[n].fd = ws_fd(o->ws);
        fds[n].events = (short)(POLLIN | (ws_want_write(o->ws) ? POLLOUT : 0));
        fds[n++].revents = 0;
    }
    if (o->child_err >= 0 && n < max) {
        fds[n].fd = o->child_err;
        fds[n].events = POLLIN;
        fds[n++].revents = 0;
    }
    return n;
}

static int cx_dispatch(tny_backend *b, struct pollfd *fds, int n) {
    (void)fds;
    (void)n;
    cx_impl *o = b->impl;
    if (!o->ws) return 0;
    cx_process_retries(o);
    if (cx_pump_once(o, 0) != 0) {
        cx_fail_turn(o, "codex app-server closed the connection mid-turn");
        return -1;
    }
    if (cx_child_gone(o)) {
        cx_fail_turn(o, "codex app-server exited mid-turn");
        return -1;
    }
    if (o->turn_active && o->cancel_sent && now_ms() > o->cancel_deadline)
        cx_end_turn(o, TNY_STOP_INTERRUPTED);
    return 0;
}

/* supportedReasoningEfforts entries are objects ({"reasoningEffort":…}) in
 * current schemas; accept bare strings too. Appends `,"efforts":[…]`. */
static void cx_append_efforts(buf_t *j, yyjson_val *m) {
    yyjson_val *arr = jget(m, "supportedReasoningEfforts");
    if (!arr || !yyjson_is_arr(arr) || !yyjson_arr_size(arr)) return;
    buf_appends(j, ",\"efforts\":[");
    size_t idx, max;
    yyjson_val *e;
    bool first = true;
    yyjson_arr_foreach(arr, idx, max, e) {
        const char *v = yyjson_is_str(e) ? yyjson_get_str(e)
                                         : jget_str(e, "reasoningEffort");
        if (!v || !*v) continue;
        if (!first) buf_appends(j, ",");
        first = false;
        jescape(j, v);
    }
    buf_appends(j, "]");
    const char *dflt = jget_str(m, "defaultReasoningEffort");
    if (dflt && *dflt) {
        buf_appends(j, ",\"default_effort\":");
        jescape(j, dflt);
    }
}

/* Normalized model catalog: model/list result.data ->
 * [{"id","name","efforts":[…],"default_effort":…},…].
 * The host hides internal entries behind "hidden": skip them. */
static int cx_list_models(tny_backend *b, char **out, char *err, size_t errlen) {
    cx_impl *o = b->impl;
    if (!o->ws) { snprintf(err, errlen, "codex: not connected"); return -1; }
    yyjson_doc *doc = cx_request_sync(o, "model/list", "{}", CX_RPC_TIMEOUT_MS,
                                      err, errlen);
    if (!doc) return -1;
    yyjson_val *data = jget(jget(yyjson_doc_get_root(doc), "result"), "data");
    buf_t j;
    buf_init(&j);
    buf_appends(&j, "[");
    if (data && yyjson_is_arr(data)) {
        size_t idx, max;
        yyjson_val *m;
        bool first = true;
        yyjson_arr_foreach(data, idx, max, m) {
            const char *id = jget_str(m, "id");
            if (!id || !*id || jget_bool(m, "hidden", false)) continue;
            if (!first) buf_appends(&j, ",");
            first = false;
            buf_appends(&j, "{\"id\":");
            jescape(&j, id);
            const char *nm = jget_str(m, "displayName");
            if (nm) { buf_appends(&j, ",\"name\":"); jescape(&j, nm); }
            cx_append_efforts(&j, m);
            buf_appends(&j, "}");
        }
    }
    buf_appends(&j, "]");
    yyjson_doc_free(doc);
    *out = buf_detach(&j);
    return 0;
}

/* ---------- doctor ---------- */

static void first_line(char *s) {
    char *nl = strpbrk(s, "\r\n");
    if (nl) *nl = 0;
}

static int cx_doctor(struct tny_ctx *ctx, char *line, size_t linelen) {
    if (ctx->codex_ws && *ctx->codex_ws) {
        snprintf(line, linelen, "codex: attach to %.160s (no spawn)", ctx->codex_ws);
        return 0;
    }
    char bin[512];
    snprintf(bin, sizeof bin, "%s",
             ctx->codex_bin && *ctx->codex_bin ? ctx->codex_bin : "codex");
    char ver[256];
    char *vargv[] = {bin, (char *)"--version", NULL};
    if (cx_capture(vargv, ver, sizeof ver, 4000) != 0) {
        snprintf(line, linelen,
                 "codex: '%s' not runnable (install the Codex CLI, set TNY_CODEX_BIN "
                 "or --codex-bin, or attach with --codex-ws)", bin);
        return 1;
    }
    first_line(str_trim(ver));
    char login[256];
    char *largv[] = {bin, (char *)"login", (char *)"status", NULL};
    int lrc = cx_capture(largv, login, sizeof login, 6000);
    snprintf(line, linelen, "codex: %.120s, login %s", ver,
             lrc == 0 ? "ok" : "required (run `codex login`)");
    return 0;
}

/* ---------- lifecycle ---------- */

static void cx_destroy(tny_backend *b) {
    cx_impl *o = b->impl;
    cx_disconnect(b);
    for (int i = 0; i < o->n_out; i++) free(o->outq[i]);
    free(o->outq);
    for (int i = 0; i < CX_MAX_PENDING; i++) cx_pending_clear(&o->pending[i]);
    for (int i = 0; i < CX_MAX_APPROVALS; i++) {
        free(o->approvals[i].id);
        free(o->approvals[i].method);
    }
    for (int i = 0; i < o->n_streamed; i++) free(o->streamed[i]);
    if (o->wait_doc) yyjson_doc_free(o->wait_doc);
    buf_free(&o->child_line);
    buf_free(&o->stderr_tail);
    free(o->thread_id);
    free(o->turn_id);
    free(o->ws_url);
    secure_free(o->token);
    free(o);
    free(b);
}

tny_backend *tny_backend_codex_new(struct tny_ctx *ctx) {
    tny_backend *b = calloc(1, sizeof *b);
    cx_impl *o = calloc(1, sizeof *o);
    if (!b || !o) { free(b); free(o); return NULL; }
    o->ctx = ctx;
    o->child_err = -1;
    o->next_id = 1;
    o->wait_id = -1;
    buf_init(&o->child_line);
    buf_init(&o->stderr_tail);
    b->id = TNY_BK_CODEX;
    b->impl = o;
    b->connect = cx_connect;
    b->disconnect = cx_disconnect;
    b->create_or_resume = cx_create_or_resume;
    b->session_pointer = cx_session_pointer;
    b->send = cx_send;
    b->steer = cx_steer;
    b->cancel = cx_cancel;
    b->respond_permission = cx_respond_permission;
    b->pollfds = cx_pollfds;
    b->dispatch = cx_dispatch;
    b->list_models = cx_list_models;
    b->doctor = cx_doctor;
    b->destroy = cx_destroy;
    return b;
}
