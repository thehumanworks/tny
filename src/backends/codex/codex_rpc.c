/* codex_rpc.c — JSON-RPC-shaped framing (no "jsonrpc" member), the outgoing
 * frame queue, the non-blocking pump and the -32001 backoff.
 * Outgoing frames are queued rather than written inline so we never re-enter
 * wslay from inside its own recv callback. */
#include "backends/codex/codex.h"

#include <poll.h>
#include "util/tny_poll.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- events ---------- */

void cx_emit(cx_impl *o, const tny_event *ev) {
    if (o->cb) o->cb(ev, o->ud);
}

void cx_emit_text(cx_impl *o, tny_event_kind k, const char *t) {
    if (!t || !*t) return;
    tny_event ev = {0};
    ev.kind = k;
    ev.text = t;
    ev.text_len = strlen(t);
    cx_emit(o, &ev);
}

void cx_end_turn(cx_impl *o, tny_stop_reason stop) {
    if (!o->turn_active) return;
    o->turn_active = false;
    o->cancel_sent = false;
    /* A turn/steer the host never answered cannot have joined this turn.
     * Resolve each one as rejected — carrying its own text — before
     * TURN_END, so a response that arrives after the turn completed (legal:
     * response and notification ordering on the socket is independent)
     * cannot strand the text (docs/adr/0013). */
    for (int i = 0; i < CX_MAX_PENDING; i++) {
        cx_pending *p = &o->pending[i];
        if (!p->method || p->kind != CXR_STEER) continue;
        tny_event sev = {0};
        sev.kind = TNY_EV_STEER_REJECTED;
        sev.text = p->steer_text ? p->steer_text : "";
        sev.text_len = strlen(sev.text);
        cx_emit(o, &sev);
        cx_pending_clear(p);
    }
    tny_event ev = {0};
    ev.kind = TNY_EV_TURN_END;
    ev.stop = stop;
    cx_emit(o, &ev);
}

void cx_fail_turn(cx_impl *o, const char *msg) {
    if (!o->turn_active) return;
    cx_emit_text(o, TNY_EV_ERROR, msg);
    cx_end_turn(o, TNY_STOP_ERROR);
}

/* ---------- outgoing frames ---------- */

/* Queued, never sent from inside a wslay recv callback. */
void cx_queue(cx_impl *o, char *json) {
    if (!json) return;
    if (o->n_out == o->cap_out) {
        int cap = o->cap_out ? o->cap_out * 2 : 8;
        char **q = realloc(o->outq, sizeof(char *) * (size_t)cap);
        if (!q) { free(json); return; }
        o->outq = q;
        o->cap_out = cap;
    }
    o->outq[o->n_out++] = json;
}

int cx_flush(cx_impl *o) {
    int rc = 0;
    for (int i = 0; i < o->n_out; i++) {
        if (o->ws && rc == 0 && !o->dead &&
            ws_send_text(o->ws, o->outq[i], strlen(o->outq[i])) != 0)
            rc = -1;
        free(o->outq[i]);
    }
    o->n_out = 0;
    if (rc != 0) o->dead = true;
    return rc;
}

cx_pending *cx_pending_find(cx_impl *o, int id) {
    for (int i = 0; i < CX_MAX_PENDING; i++)
        if (o->pending[i].method && o->pending[i].id == id) return &o->pending[i];
    return NULL;
}

void cx_pending_clear(cx_pending *p) {
    free(p->method);
    free(p->params);
    free(p->steer_text);
    memset(p, 0, sizeof *p);
}

/* Codex app-server owns its thread store. When tny is ephemeral, add the
 * protocol's native flag at the framing boundary so both initial requests
 * and tracked retries carry the same no-store intent. Caller-generated
 * thread/start params are compact JSON objects. */
static char *cx_ephemeral_params(cx_impl *o, const char *method, const char *params) {
    if (!o->ctx || !o->ctx->no_save || strcmp(method, "thread/start") != 0)
        return NULL;
    const char *p = params && *params ? params : "{}";
    size_t len = strlen(p);
    if (len < 2 || p[0] != '{' || p[len - 1] != '}') return NULL;
    buf_t b;
    buf_init(&b);
    buf_append(&b, p, len - 1);
    if (len > 2) buf_appends(&b, ",");
    buf_appends(&b, "\"ephemeral\":true}");
    return buf_detach(&b);
}

/* Queue one request frame. A tracked kind that cannot get a pending slot is
 * NOT sent and -1 is returned: an untracked response would be unmatchable,
 * so e.g. a steer would never see its rejection (docs/adr/0013). */
int cx_request(cx_impl *o, const char *method, const char *params, cx_reqkind kind) {
    char *ephemeral = cx_ephemeral_params(o, method, params);
    const char *wire_params = ephemeral ? ephemeral :
                              (params && *params ? params : "{}");
    if (kind != CXR_FREE) {
        bool registered = false;
        for (int i = 0; i < CX_MAX_PENDING; i++) {
            if (o->pending[i].method) continue;
            o->pending[i].id = o->next_id;
            o->pending[i].kind = kind;
            o->pending[i].method = xstrdup(method);
            o->pending[i].params = xstrdup(wire_params);
            registered = true;
            break;
        }
        if (!registered) {
            free(ephemeral);
            return -1;
        }
    }
    int id = o->next_id++;
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"method\":");
    jescape(&b, method);
    buf_appendf(&b, ",\"id\":%d,\"params\":%s}", id, wire_params);
    cx_queue(o, buf_detach(&b));
    free(ephemeral);
    return id;
}

void cx_notify(cx_impl *o, const char *method, const char *params) {
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"method\":");
    jescape(&b, method);
    if (params && *params) buf_appendf(&b, ",\"params\":%s", params);
    buf_appends(&b, "}");
    cx_queue(o, buf_detach(&b));
}

void cx_respond_result(cx_impl *o, const char *id_json, const char *result) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "{\"id\":%s,\"result\":%s}", id_json, result);
    cx_queue(o, buf_detach(&b));
}

void cx_respond_error(cx_impl *o, const char *id_json, int code, const char *msg) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "{\"id\":%s,\"error\":{\"code\":%d,\"message\":", id_json, code);
    jescape(&b, msg);
    buf_appends(&b, "}}");
    cx_queue(o, buf_detach(&b));
}

/* -32001 "server overloaded": resend with a fresh id after the backoff. */
void cx_process_retries(cx_impl *o) {
    int64_t now = now_ms();
    for (int i = 0; i < CX_MAX_PENDING; i++) {
        cx_pending *p = &o->pending[i];
        if (!p->method || !p->due_ms || now < p->due_ms) continue;
        p->due_ms = 0;
        p->id = o->next_id++;
        buf_t b;
        buf_init(&b);
        buf_appends(&b, "{\"method\":");
        jescape(&b, p->method);
        buf_appendf(&b, ",\"id\":%d,\"params\":%s}", p->id, p->params ? p->params : "{}");
        cx_queue(o, buf_detach(&b));
    }
}

/* ---------- pump ---------- */

int cx_pump_once(cx_impl *o, int poll_ms) {
    if (!o->ws || o->dead) return -1;
    if (cx_flush(o) != 0) return -1;
    struct pollfd fds[2];
    int n = 0;
    fds[n].fd = ws_fd(o->ws);
    fds[n].events = (short)(POLLIN | (ws_want_write(o->ws) ? POLLOUT : 0));
    fds[n++].revents = 0;
    if (o->child_err >= 0) {
        fds[n].fd = o->child_err;
        fds[n].events = POLLIN;
        fds[n++].revents = 0;
    }
    if (poll_ms > 0) tny_poll(fds, (nfds_t)n, poll_ms);
    cx_drain_child_stderr(o);
    if (ws_pump(o->ws, cx_on_ws_msg, o) != 0) { o->dead = true; return -1; }
    return cx_flush(o);
}

static void cx_idle(cx_impl *o, int ms) {
    int64_t deadline = now_ms() + ms;
    while (now_ms() < deadline && !o->dead)
        if (cx_pump_once(o, 50) != 0) return;
}

yyjson_doc *cx_request_sync(cx_impl *o, const char *method, const char *params,
                                   int timeout_ms, char *err, size_t errlen) {
    for (int attempt = 0; attempt < CX_RETRY_MAX; attempt++) {
        int id = cx_request(o, method, params, CXR_FREE);
        o->wait_id = id;
        o->wait_done = false;
        o->wait_doc = NULL;
        int64_t deadline = now_ms() + timeout_ms;
        while (!o->wait_done) {
            if (cx_pump_once(o, 100) != 0) {
                snprintf(err, errlen, "codex: connection lost during %s", method);
                o->wait_id = -1;
                return NULL;
            }
            if (cx_child_gone(o)) {
                snprintf(err, errlen, "codex app-server exited during %s", method);
                o->wait_id = -1;
                return NULL;
            }
            if (now_ms() > deadline) {
                snprintf(err, errlen, "codex: timed out waiting for %s", method);
                o->wait_id = -1;
                return NULL;
            }
        }
        o->wait_id = -1;
        yyjson_doc *doc = o->wait_doc;
        o->wait_doc = NULL;
        yyjson_val *e = jget(yyjson_doc_get_root(doc), "error");
        if (!e) return doc;
        int64_t code = jget_int(e, "code", 0);
        char msg[240];
        const char *m = jget_str(e, "message");
        snprintf(msg, sizeof msg, "%.200s", m ? m : "no detail");
        yyjson_doc_free(doc);
        if (code == -32001 && attempt + 1 < CX_RETRY_MAX) {
            cx_idle(o, (250 << attempt) + (int)(now_ms() % 120));
            continue;
        }
        snprintf(err, errlen, "codex: %s failed (%lld): %s", method, (long long)code, msg);
        return NULL;
    }
    snprintf(err, errlen, "codex: %s still overloaded after %d attempts",
             method, CX_RETRY_MAX);
    return NULL;
}

