/* acp_events.c — turning what an ACP agent sends into tny's normalized events
 * (docs/architecture.md event set). Agent output is untrusted: every field is
 * type-checked before use and every copied span is bounded. */
#include "backends/acp/acp_client.h"
#include "util/util.h"
#include "util/alloc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ACP_DETAIL_CAP 4000

/* ---------- event helpers ---------- */

void ac_emit(ac_impl *o, const tny_backend_event *ev) {
    if (o->cb && !tny_alloc_scope_failed()) o->cb(ev, o->ud);
}

void ac_emit_text(ac_impl *o, tny_event_kind k, const char *t, size_t n) {
    tny_backend_event ev = {0};
    ev.kind = k;
    ev.text = t;
    ev.text_len = n;
    ac_emit(o, &ev);
}

void ac_emit_end(ac_impl *o, tny_stop_reason stop) {
    if (!ac_stop_owned_agent(o)) {
        ac_emit_text(o, TNY_EV_ERROR, ACP_CLEANUP_UNKNOWN, sizeof(ACP_CLEANUP_UNKNOWN) - 1);
        stop = TNY_STOP_ERROR;
    }
    if (o->cancelled && stop == TNY_STOP_DONE) stop = TNY_STOP_INTERRUPTED;
    if ((stop == TNY_STOP_DONE || stop == TNY_STOP_INTERRUPTED) &&
        (o->permission_denied || (o->bridge && tny_acp_bridge_permission_blocked(o->bridge))))
        stop = TNY_STOP_DENIED;
    if (stop == TNY_STOP_DONE && o->bridge && tny_acp_bridge_ack_context(o->bridge) != 0) {
        const char *message = "acp: could not persist delivered runtime context acknowledgement";
        ac_emit_text(o, TNY_EV_ERROR, message, strlen(message));
        stop = TNY_STOP_ERROR;
    }
    if (o->bridge) {
        if (tny_alloc_settling() || tny_alloc_scope_failed()) tny_acp_bridge_abort(o->bridge);
        else tny_acp_bridge_end_turn(o->bridge);
    }
    o->turn_active = false;
    tny_backend_event ev = {0};
    ev.kind = TNY_EV_TURN_END;
    ev.stop = stop;
    ac_emit(o, &ev);
}

/* ---------- permission bookkeeping ---------- */

ac_perm *ac_perm_find(ac_impl *o, const char *id_raw) {
    for (int i = 0; i < o->nperms; i++)
        if (o->perms[i].id_raw && strcmp(o->perms[i].id_raw, id_raw) == 0) return &o->perms[i];
    return NULL;
}

void ac_perm_drop(ac_impl *o, ac_perm *p) {
    free(p->id_raw);
    free(p->allow_once);
    free(p->allow_always);
    free(p->reject);
    free(p->summary);
    int idx = (int)(p - o->perms);
    o->perms[idx] = o->perms[--o->nperms];
    memset(&o->perms[o->nperms], 0, sizeof(ac_perm));
}

void ac_perms_clear(ac_impl *o) {
    while (o->nperms) ac_perm_drop(o, &o->perms[o->nperms - 1]);
}

static void handle_permission(ac_impl *o, yyjson_val *msg, yyjson_val *params) {
    char *id_raw = acp_id_text(msg);
    if (tny_alloc_scope_failed()) {
        free(id_raw);
        return;
    }
    if (o->nperms >= ACP_MAX_PERMS || ac_perm_find(o, id_raw)) {
        ac_tx_error(o, id_raw, ACP_E_INTERNAL, "too many pending permissions");
        free(id_raw);
        return;
    }
    ac_perm *p = &o->perms[o->nperms++];
    memset(p, 0, sizeof *p);
    p->id_raw = id_raw;

    int opts = 0;
    yyjson_val *arr = jget(params, "options");
    size_t idx, max;
    yyjson_val *op;
    if (arr && yyjson_is_arr(arr)) {
        yyjson_arr_foreach(arr, idx, max, op) {
            if (tny_alloc_scope_failed()) return;
            const char *oid = jget_str(op, "optionId");
            const char *kind = jget_str(op, "kind");
            if (!oid) continue;
            if (kind && strcmp(kind, "allow_always") == 0) {
                if (!p->allow_always) p->allow_always = xstrdup(oid);
            } else if (kind && strcmp(kind, "allow_once") == 0) {
                if (!p->allow_once) p->allow_once = xstrdup(oid);
            } else if (kind &&
                       (strcmp(kind, "reject_once") == 0 || strcmp(kind, "reject_always") == 0)) {
                if (!p->reject) p->reject = xstrdup(oid);
            }
        }
    }
    if (tny_alloc_scope_failed()) return;
    if (p->allow_once) opts |= TNY_PERM_ALLOW_ONCE;
    if (p->allow_always) opts |= TNY_PERM_ALLOW_ALWAYS;
    if (p->reject) opts |= TNY_PERM_DENY;

    yyjson_val *tc = jget(params, "toolCall");
    const char *title = jget_str(tc, "title");
    const char *kind = jget_str(tc, "kind");
    buf_t sum;
    buf_init(&sum);
    buf_appendf(&sum, "%.200s%s%.40s", title ? title : "tool call", kind ? " (" : "",
                kind ? kind : "");
    if (kind) buf_appends(&sum, ")");
    p->summary = buf_detach(&sum);

    tny_backend_event ev = {0};
    ev.kind = TNY_EV_PERMISSION;
    ev.perm_id = p->id_raw;
    ev.perm_summary = p->summary;
    ev.perm_options = opts;
    ev.tool_name = title;
    ac_emit(o, &ev);
}

/* ---------- session/update ---------- */

static void append_tool_content(yyjson_val *content, buf_t *out) {
    if (!content || !yyjson_is_arr(content)) return;
    size_t idx, max;
    yyjson_val *c;
    yyjson_arr_foreach(content, idx, max, c) {
        yyjson_val *inner = jget(c, "content");
        const char *t = jget_str(inner ? inner : c, "text");
        if (!t) t = jget_str(c, "newText"); /* diff blocks */
        if (!t) continue;
        if (out->len) buf_appends(out, "\n");
        size_t room = out->len < ACP_DETAIL_CAP ? ACP_DETAIL_CAP - out->len : 0;
        buf_append(out, t, strnlen(t, room));
        if (out->len > ACP_DETAIL_CAP) break;
    }
}

static void update_tool_call(ac_impl *o, yyjson_val *u, const char *kind) {
    const char *id = jget_str(u, "toolCallId");
    const char *title = jget_str(u, "title");
    const char *status = jget_str(u, "status");
    bool done = status && (strcmp(status, "completed") == 0 || strcmp(status, "failed") == 0);
    buf_t detail;
    buf_init(&detail);
    append_tool_content(jget(u, "content"), &detail);
    if (strcmp(kind, "tool_call") == 0) {
        const char *tk = jget_str(u, "kind");
        tny_backend_event ev = {0};
        ev.kind = TNY_EV_TOOL_START;
        ev.tool_name = title ? title : (tk ? tk : "tool");
        ev.tool_id = id;
        ev.tool_detail = detail.len ? detail.data : NULL;
        ac_emit(o, &ev);
    }
    if (done) {
        tny_backend_event ev = {0};
        ev.kind = TNY_EV_TOOL_END;
        ev.tool_name = title ? title : "tool";
        ev.tool_id = id;
        ev.tool_detail = detail.len ? detail.data : NULL;
        ev.tool_ok = strcmp(status, "completed") == 0;
        ac_emit(o, &ev);
    } else if (strcmp(kind, "tool_call_update") == 0) {
        tny_backend_event ev = {0};
        ev.kind = TNY_EV_TOOL_PROGRESS;
        ev.tool_name = title ? title : "tool";
        ev.tool_id = id;
        ev.tool_detail = detail.len ? detail.data : status;
        ac_emit(o, &ev);
    }
    buf_free(&detail);
}

static void update_plan(ac_impl *o, yyjson_val *u) {
    buf_t b;
    buf_init(&b);
    yyjson_val *entries = jget(u, "entries");
    size_t idx, max;
    yyjson_val *e;
    if (entries && yyjson_is_arr(entries)) {
        yyjson_arr_foreach(entries, idx, max, e) {
            const char *c = jget_str(e, "content");
            const char *st = jget_str(e, "status");
            if (!c) continue;
            if (b.len) buf_appends(&b, "\n");
            buf_appendf(&b, "[%s] %.300s", st ? st : "pending", c);
            if (b.len > ACP_DETAIL_CAP) break;
        }
    }
    if (b.len) ac_emit_text(o, TNY_EV_PLAN, b.data, b.len);
    buf_free(&b);
}

static int64_t usage_context_count(yyjson_val *update, const char *key) {
    yyjson_val *value = jget(update, key);
    if (!yyjson_is_int(value)) return -1;
    if (yyjson_is_uint(value) && yyjson_get_uint(value) > INT64_MAX) return -1;
    int64_t count = yyjson_get_sint(value);
    return count >= 0 ? count : -1;
}

void ac_handle_update(ac_impl *o, yyjson_val *params) {
    const char *session = jget_str(params, "sessionId");
    if (!o->turn_active || !session || !o->session_id || strcmp(session, o->session_id) != 0)
        return;
    yyjson_val *u = jget(params, "update");
    if (!u || !yyjson_is_obj(u)) return;
    const char *kind = jget_str(u, "sessionUpdate");
    if (!kind) return;

    if (strcmp(kind, "agent_message_chunk") == 0 || strcmp(kind, "agent_thought_chunk") == 0) {
        yyjson_val *c = jget(u, "content");
        const char *type = jget_str(c, "type");
        const char *text = (!type || strcmp(type, "text") == 0) ? jget_str(c, "text") : NULL;
        if (!text) return;
        bool thought = strcmp(kind, "agent_thought_chunk") == 0;
        tny_backend_event ev = {0};
        ev.kind = thought ? TNY_EV_THINKING : TNY_EV_TEXT_DELTA;
        ev.text = text;
        ev.text_len = strlen(text);
        ev.message_id = jget_str(u, "messageId");
        ac_emit(o, &ev);
        return;
    }
    if (strcmp(kind, "user_message_chunk") == 0) return; /* our own echo */
    if (strcmp(kind, "tool_call") == 0 || strcmp(kind, "tool_call_update") == 0) {
        /* The verified Claude tools-only adapter reports the same MCP calls
         * already emitted by the owning runtime. Its updates are presentation,
         * not additional executions. Generic agents can have independent tools. */
        if (!o->claude_verified) update_tool_call(o, u, kind);
        return;
    }
    if (strcmp(kind, "plan") == 0) {
        update_plan(o, u);
        return;
    }
    if (strcmp(kind, "usage_update") == 0) {
        tny_backend_event ev = {0};
        ev.kind = TNY_EV_USAGE;
        /* These are context occupancy and session cost, not turn token usage. */
        ev.tokens_unreported = true;
        ev.context_used = usage_context_count(u, "used");
        ev.context_size = usage_context_count(u, "size");
        o->usage_seen = true;
        if (ev.context_used >= 0) o->usage_context_used = ev.context_used;
        if (ev.context_size >= 0) o->usage_context_size = ev.context_size;
        yyjson_val *cost = jget(u, "cost");
        yyjson_val *amount = yyjson_is_obj(cost) ? jget(cost, "amount") : cost;
        const char *currency = jget_str(cost, "currency");
        bool valid_currency = currency && *currency && strlen(currency) <= 16 &&
                              yyjson_get_len(jget(cost, "currency")) == strlen(currency);
        if (yyjson_is_num(amount) && (!yyjson_is_obj(cost) || valid_currency)) {
            double value = yyjson_get_num(amount);
            if (isfinite(value) && value >= 0) {
                ev.cost = value;
                ev.has_cost = true;
                ev.cost_cumulative = true;
                ev.cost_currency = valid_currency ? currency : NULL;
                o->usage_cost = value;
                o->usage_has_cost = true;
                o->usage_currency[0] = '\0';
                if (valid_currency) memcpy(o->usage_currency, currency, strlen(currency) + 1);
            }
        }
        ac_emit(o, &ev);
        return;
    }
    /* available_commands_update, current_mode_update, anything newer:
     * protocol chatter, not conversation — surface only under TNY_DEBUG */
    if (tny_debug()) {
        buf_t s;
        buf_init(&s);
        buf_appendf(&s, "%.80s", kind);
        const char *mode = jget_str(u, "currentModeId");
        if (mode) buf_appendf(&s, ": %.80s", mode);
        ac_emit_text(o, TNY_EV_STATUS, s.data, s.len);
        buf_free(&s);
    }
}

/* ---------- agent → client requests ---------- */

void ac_handle_agent_request(ac_impl *o, yyjson_val *msg, const char *method, yyjson_val *params) {
    const char *session = jget_str(params, "sessionId");
    if (!session || !o->session_id || strcmp(session, o->session_id) != 0) {
        char *id = acp_id_text(msg);
        ac_tx_error(o, id, ACP_E_PARAMS, "unknown sessionId");
        free(id);
        return;
    }
    if (strcmp(method, "session/request_permission") == 0) {
        if (!o->turn_active || o->cancelled) {
            char *id = acp_id_text(msg);
            ac_tx_result(o, id, "{\"outcome\":{\"outcome\":\"cancelled\"}}");
            free(id);
            return;
        }
        handle_permission(o, msg, params);
        return;
    }
    char *id = acp_id_text(msg);
    if (tny_alloc_scope_failed()) {
        free(id);
        return;
    }
    buf_t m;
    buf_init(&m);
    buf_appendf(&m,
                "tny does not implement %.100s (no fs/terminal capabilities "
                "advertised)",
                method);
    ac_tx_error(o, id, ACP_E_NO_METHOD, m.data);
    buf_free(&m);
    free(id);
}

/* ---------- the session/prompt response ends the turn ---------- */

void ac_handle_prompt_response(ac_impl *o, yyjson_val *msg) {
    yyjson_val *err = jget(msg, "error");
    if (err) {
        buf_t b;
        buf_init(&b);
        buf_appendf(&b, "agent rejected the prompt (code %lld)",
                    (long long)jget_int(err, "code", ACP_E_INTERNAL));
        ac_emit_text(o, TNY_EV_ERROR, b.data, b.len);
        buf_free(&b);
        ac_emit_end(o, TNY_STOP_ERROR);
        return;
    }
    const char *reason = jget_str(jget(msg, "result"), "stopReason");
    if (!reason) {
        ac_emit_text(o, TNY_EV_ERROR, "acp: prompt response has no stopReason", 38);
        ac_emit_end(o, TNY_STOP_ERROR);
        return;
    }
    if (strcmp(reason, "end_turn") == 0) {
        ac_emit_end(o, TNY_STOP_DONE);
    } else if (strcmp(reason, "cancelled") == 0) {
        ac_emit_end(o, TNY_STOP_INTERRUPTED);
    } else if (strcmp(reason, "refusal") == 0) {
        ac_emit_text(o, TNY_EV_ERROR, "agent refused the request", 25);
        ac_emit_end(o, TNY_STOP_DENIED);
    } else if (strcmp(reason, "max_tokens") == 0 || strcmp(reason, "max_turn_requests") == 0) {
        ac_emit_text(o, TNY_EV_ERROR, reason, strlen(reason));
        ac_emit_end(o, TNY_STOP_STEP_LIMIT);
    } else {
        buf_t b;
        buf_init(&b);
        buf_appendf(&b, "unknown stopReason '%.60s'", reason);
        ac_emit_text(o, TNY_EV_ERROR, b.data, b.len);
        buf_free(&b);
        ac_emit_end(o, TNY_STOP_ERROR);
    }
}
