/* codex_msg.c — one WebSocket TEXT frame in, normalized events out: routing
 * for notifications, server->client requests and responses.
 * Item rendering lives in codex_items.c (docs/backends/codex-app-server.md). */
#include "backends/codex/codex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- streamed-item bookkeeping (dedupe agentMessage text) ---------- */

static bool streamed_seen(cx_impl *o, const char *id) {
    if (!id) return false;
    for (int i = 0; i < o->n_streamed; i++)
        if (o->streamed[i] && strcmp(o->streamed[i], id) == 0) return true;
    return false;
}

static void streamed_add(cx_impl *o, const char *id) {
    if (!id || streamed_seen(o, id)) return;
    if (o->n_streamed == CX_MAX_STREAMED) {
        free(o->streamed[0]);
        memmove(o->streamed, o->streamed + 1,
                sizeof o->streamed[0] * (CX_MAX_STREAMED - 1));
        o->n_streamed--;
    }
    o->streamed[o->n_streamed++] = xstrdup(id);
}

/* ---------- notifications ---------- */

static void on_item_delta(cx_impl *o, const char *method, yyjson_val *params) {
    const char *kind = method + strlen("item/"); /* "<kind>/delta" */
    const char *id = cx_first_str(params, CX_ITEM_ID_KEYS);
    yyjson_val *d = jget(params, "delta");
    const char *text = NULL;
    if (d && yyjson_is_str(d)) text = yyjson_get_str(d);
    else if (d && yyjson_is_obj(d)) text = cx_first_str(d, CX_TEXT_KEYS);
    if (!text) text = jget_str(params, "text");
    if (!text || !*text) return;

    if (str_starts(kind, "agentMessage/") || str_starts(kind, "assistantMessage/")) {
        streamed_add(o, id);
        tny_backend_event ev = {0};
        ev.kind = TNY_EV_TEXT_DELTA;
        ev.text = text;
        ev.text_len = strlen(text);
        ev.message_id = id;
        cx_emit(o, &ev);
    } else if (strstr(kind, "easoning")) {
        streamed_add(o, id);
        tny_backend_event ev = {0};
        ev.kind = TNY_EV_THINKING;
        ev.text = text;
        ev.text_len = strlen(text);
        ev.message_id = id;
        cx_emit(o, &ev);
    } else if (str_starts(kind, "plan/")) {
        tny_backend_event ev = {0};
        ev.kind = TNY_EV_PLAN;
        ev.text = text;
        ev.text_len = strlen(text);
        ev.message_id = id;
        cx_emit(o, &ev);
    } else {
        tny_backend_event ev = {0};
        ev.kind = TNY_EV_TOOL_PROGRESS;
        ev.tool_name = kind;
        ev.tool_id = id;
        ev.tool_detail = text;
        cx_emit(o, &ev);
    }
}

static void on_turn_completed(cx_impl *o, yyjson_val *params) {
    yyjson_val *turn = jget(params, "turn");
    if (!turn || !yyjson_is_obj(turn)) turn = params;
    const char *st = cx_first_str(turn, CX_STATUS_KEYS);
    if (!st) st = cx_first_str(params, CX_STATUS_KEYS);
    if (!cx_emit_usage(o, turn) && turn != params) cx_emit_usage(o, params);
    tny_stop_reason stop = TNY_STOP_DONE;
    if (st) {
        if (strcmp(st, "interrupted") == 0 || strcmp(st, "cancelled") == 0 ||
            strcmp(st, "canceled") == 0 || strcmp(st, "aborted") == 0)
            stop = TNY_STOP_INTERRUPTED;
        else if (strcmp(st, "failed") == 0 || strcmp(st, "error") == 0)
            stop = TNY_STOP_ERROR;
    }
    if (stop == TNY_STOP_ERROR) {
        yyjson_val *e = jget(turn, "error");
        const char *msg = jget_str(e, "message");
        if (!msg) msg = jget_str(params, "message");
        buf_t b;
        buf_init(&b);
        buf_appendf(&b, "codex turn failed: %.400s", msg ? msg : "no detail from the host");
        cx_emit_capped(o, TNY_EV_ERROR, b.data);
        buf_free(&b);
    }
    cx_end_turn(o, stop);
}

static void cx_notification(cx_impl *o, const char *method, yyjson_val *params) {
    if (strcmp(method, "account/login/completed") == 0) {
        /* codex_login.c pumps until this lands; success:false carries the
         * host's reason. Never log tokens or the params blob. */
        o->login_ok = jget_bool(params, "success", false);
        const char *e = jget_str(params, "error");
        if (e) snprintf(o->login_err, sizeof o->login_err, "%.200s", e);
        o->login_done = true;
        return;
    }
    if (strcmp(method, "turn/started") == 0) {
        yyjson_val *turn = jget(params, "turn");
        const char *tid = jget_str(turn ? turn : params, "id");
        if (!tid) tid = cx_first_str(params, CX_TURN_ID_KEYS);
        if (tid) { free(o->turn_id); o->turn_id = xstrdup(tid); }
        return;
    }
    if (strcmp(method, "turn/completed") == 0) { on_turn_completed(o, params); return; }
    if (strcmp(method, "turn/failed") == 0 || strcmp(method, "turn/aborted") == 0) {
        const char *msg = jget_str(jget(params, "error"), "message");
        buf_t b;
        buf_init(&b);
        buf_appendf(&b, "codex turn failed: %.400s", msg ? msg : "no detail from the host");
        cx_emit_capped(o, TNY_EV_ERROR, b.data);
        buf_free(&b);
        cx_end_turn(o, TNY_STOP_ERROR);
        return;
    }
    if (str_starts(method, "item/") && str_ends(method, "/delta")) {
        on_item_delta(o, method, params);
        return;
    }
    if (strcmp(method, "item/started") == 0 || strcmp(method, "item/updated") == 0 ||
        strcmp(method, "item/completed") == 0) {
        bool done = strcmp(method, "item/completed") == 0;
        if (strcmp(method, "item/updated") == 0) return; /* nothing new to render */
        yyjson_val *item = cx_item_of(params);
        const char *type = cx_first_str(item, CX_ITEM_TYPE_KEYS);
        if (!type) return;
        const char *id = cx_first_str(item, CX_ITEM_ID_KEYS);
        if (!id) id = cx_first_str(params, CX_ITEM_ID_KEYS);
        if (cx_item_is_user_echo(type, item)) return;
        if (cx_type_is_message(type)) {
            if (done && !streamed_seen(o, id)) {
                char *t = cx_item_text(item);
                if (t) { cx_emit_capped(o, TNY_EV_TEXT_DELTA, t); free(t); }
            }
            return;
        }
        if (cx_type_is_reasoning(type)) {
            if (done && !streamed_seen(o, id)) {
                char *t = cx_item_text(item);
                if (t) { cx_emit_capped(o, TNY_EV_THINKING, t); free(t); }
            }
            return;
        }
        if (cx_type_is_plan(type)) {
            if (done) cx_emit_plan(o, item);
            return;
        }
        char *detail = cx_item_detail(type, item);
        tny_backend_event ev = {0};
        ev.kind = done ? TNY_EV_TOOL_END : TNY_EV_TOOL_START;
        ev.tool_name = type;
        ev.tool_id = id;
        ev.tool_detail = detail;
        ev.tool_ok = done ? cx_item_ok(item) : true;
        cx_emit(o, &ev);
        free(detail);
        return;
    }
    if (strcmp(method, "thread/archived") == 0) {
        cx_emit_capped(o, TNY_EV_STATUS, "codex archived this thread");
        return;
    }
    if (strstr(method, "okenCount") || strstr(method, "oken_count") ||
        strstr(method, "okenUsage") || str_ends(method, "/usage")) {
        cx_emit_usage(o, params);
        return;
    }
    if (strcmp(method, "error") == 0 || str_ends(method, "/error")) {
        const char *msg = jget_str(params, "message");
        if (!msg) msg = jget_str(jget(params, "error"), "message");
        buf_t b;
        buf_init(&b);
        buf_appendf(&b, "codex: %.400s", msg ? msg : "unspecified host error");
        cx_emit_capped(o, TNY_EV_ERROR, b.data);
        buf_free(&b);
        return;
    }
    /* everything else (session config echoes, progress pings) is ignorable */
}

/* ---------- server -> client requests ---------- */

static void approval_remember(cx_impl *o, char *id_json, const char *method) {
    for (int i = 0; i < CX_MAX_APPROVALS; i++) {
        if (o->approvals[i].id) continue;
        o->approvals[i].id = id_json;
        o->approvals[i].method = xstrdup(method);
        return;
    }
    /* ring full: the oldest slot is stale, reuse it */
    free(o->approvals[0].id);
    free(o->approvals[0].method);
    o->approvals[0].id = id_json;
    o->approvals[0].method = xstrdup(method);
}

static void approval_summary(buf_t *b, const char *method, yyjson_val *params) {
    yyjson_val *item = cx_item_of(params);
    if (str_starts(method, "item/commandExecution")) {
        buf_appends(b, "run: ");
        yyjson_val *cmd = jget(params, "command");
        if (!cmd) cmd = jget(item, "command");
        if (!cmd) cmd = jget(item, "parsedCmd");
        cx_append_words(b, cmd);
        const char *reason = jget_str(params, "reason");
        if (reason) buf_appendf(b, " — %.200s", reason);
    } else if (str_starts(method, "item/fileChange")) {
        buf_appends(b, "apply file changes: ");
        char *d = cx_item_detail("fileChange", jget(params, "changes") ? params : item);
        if (d) { buf_appendf(b, "%.400s", d); free(d); }
    } else {
        const char *what = jget_str(params, "reason");
        if (!what) what = jget_str(params, "message");
        buf_appendf(b, "codex requests approval: %.300s", what ? what : method);
    }
    if (b->len > CX_MAX_DETAIL) { b->len = CX_MAX_DETAIL; b->data[b->len] = 0; }
}

static void cx_server_request(cx_impl *o, const char *method, yyjson_val *idv,
                              yyjson_val *params) {
    char *id_json = jwrite_val(idv);
    if (!id_json) return;
    if (strlen(id_json) > CX_MAX_ID_TEXT) { free(id_json); return; }

    if (str_ends(method, "/requestApproval")) {
        buf_t sum;
        buf_init(&sum);
        approval_summary(&sum, method, params);
        approval_remember(o, id_json, method); /* takes id_json */
        tny_backend_event ev = {0};
        ev.kind = TNY_EV_PERMISSION;
        ev.perm_id = id_json;
        ev.perm_summary = sum.data ? sum.data : method;
        ev.perm_options = TNY_PERM_ALLOW_ONCE | TNY_PERM_ALLOW_ALWAYS | TNY_PERM_DENY;
        cx_emit(o, &ev);
        buf_free(&sum);
        return;
    }
    if (str_ends(method, "/requestUserInput")) {
        cx_respond_result(o, id_json, "{\"decision\":\"cancel\"}");
        cx_emit_capped(o, TNY_EV_STATUS,
                    "codex asked for extra input mid-turn; tny cancelled that "
                    "request (no interactive input channel for this backend)");
    } else if (strstr(method, "elicitation")) {
        cx_respond_result(o, id_json, "{\"action\":\"decline\"}");
        cx_emit_capped(o, TNY_EV_STATUS,
                    "declined an MCP elicitation request from codex");
    } else {
        cx_respond_error(o, id_json, -32601, "method not supported by tny");
    }
    free(id_json);
}

/* ---------- responses ---------- */

/* Takes ownership of doc unless it is handed to the synchronous waiter. */
static void cx_response(cx_impl *o, yyjson_doc *doc, yyjson_val *root, yyjson_val *idv) {
    int64_t id = yyjson_is_int(idv) ? yyjson_get_sint(idv) : -1;
    if (o->wait_id >= 0 && !o->wait_done && id == (int64_t)o->wait_id) {
        o->wait_doc = doc;
        o->wait_done = true;
        return;
    }
    cx_pending *p = cx_pending_find(o, (int)id);
    yyjson_val *err = jget(root, "error");
    if (err) {
        int64_t code = jget_int(err, "code", 0);
        const char *msg = jget_str(err, "message");
        if (code == -32001 && p && p->attempts < CX_RETRY_MAX - 1) {
            /* server overloaded: exponential backoff with jitter, per doc */
            int64_t base = 250 << p->attempts;
            p->attempts++;
            p->due_ms = now_ms() + base + (int64_t)(now_ms() % 120);
            cx_emit_capped(o, TNY_EV_STATUS, "codex is busy; retrying shortly");
        } else if (p && p->kind == CXR_STEER) {
            /* non-steerable turn (review/compact) or a stale turn id: hand
             * the text back to the frontend's queue; the turn itself is fine */
            buf_t b;
            buf_init(&b);
            buf_appendf(&b, "codex refused to steer this turn (%lld): %.200s",
                        (long long)code, msg ? msg : "no detail");
            cx_emit_capped(o, TNY_EV_STATUS, b.data);
            buf_free(&b);
            tny_backend_event ev = {0};
            ev.kind = TNY_EV_STEER_REJECTED;
            ev.text = p->steer_text ? p->steer_text : "";
            ev.text_len = strlen(ev.text);
            if (o->cb) o->cb(&ev, o->ud);
            cx_pending_clear(p);
        } else if (p) {
            buf_t b;
            buf_init(&b);
            buf_appendf(&b, "codex %s failed (%lld): %.300s",
                        p->method, (long long)code, msg ? msg : "no detail");
            cx_emit_capped(o, TNY_EV_ERROR, b.data);
            buf_free(&b);
            cx_pending_clear(p);
            cx_end_turn(o, TNY_STOP_ERROR);
        } else {
            /* a response to a request we no longer track — e.g. a steer the
             * turn-end sweep already resolved (docs/adr/0013). It must not
             * fail whatever turn is running now. */
            buf_t b;
            buf_init(&b);
            buf_appendf(&b, "codex answered a request tny gave up on (%lld): %.200s",
                        (long long)code, msg ? msg : "no detail");
            cx_emit_capped(o, TNY_EV_STATUS, b.data);
            buf_free(&b);
        }
        yyjson_doc_free(doc);
        return;
    }
    if (p && p->kind == CXR_TURN) {
        yyjson_val *res = jget(root, "result");
        yyjson_val *turn = jget(res, "turn");
        const char *tid = jget_str(turn ? turn : res, "id");
        if (!tid) tid = cx_first_str(res, CX_TURN_ID_KEYS);
        if (tid) { free(o->turn_id); o->turn_id = xstrdup(tid); }
    }
    if (p) cx_pending_clear(p);
    yyjson_doc_free(doc);
}

/* ---------- entry point ---------- */

void cx_on_ws_msg(const char *data, size_t len, void *ud) {
    cx_impl *o = ud;
    if (!len || len > CX_MAX_MSG_BYTES) return;
    yyjson_doc *doc = jparse(data, len);
    if (!doc) return;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) { yyjson_doc_free(doc); return; }
    yyjson_val *mv = jget(root, "method");
    yyjson_val *idv = jget(root, "id");
    if (mv && yyjson_is_str(mv)) {
        const char *method = yyjson_get_str(mv);
        yyjson_val *params = jget(root, "params");
        if (idv) cx_server_request(o, method, idv, params);
        else cx_notification(o, method, params);
        yyjson_doc_free(doc);
        return;
    }
    if (idv) { cx_response(o, doc, root, idv); return; }
    yyjson_doc_free(doc);
}
