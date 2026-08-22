/* acp_events.c — turning what an ACP agent sends into tny's normalized events
 * (docs/architecture.md event set). Agent output is untrusted: every field is
 * type-checked before use and every copied span is bounded. */
#include "backends/acp/acp_client.h"
#include "util/util.h"

#include <stdlib.h>
#include <string.h>

#define ACP_DETAIL_CAP 4000

/* ---------- event helpers ---------- */

void ac_emit(ac_impl *o, const tny_event *ev) {
    if (o->cb) o->cb(ev, o->ud);
}

void ac_emit_text(ac_impl *o, tny_event_kind k, const char *t, size_t n) {
    tny_event ev = {0};
    ev.kind = k;
    ev.text = t;
    ev.text_len = n;
    ac_emit(o, &ev);
}

void ac_emit_end(ac_impl *o, tny_stop_reason stop) {
    o->turn_active = false;
    tny_event ev = {0};
    ev.kind = TNY_EV_TURN_END;
    ev.stop = stop;
    ac_emit(o, &ev);
}

/* ---------- permission bookkeeping ---------- */

ac_perm *ac_perm_find(ac_impl *o, const char *id_raw) {
    for (int i = 0; i < o->nperms; i++)
        if (o->perms[i].id_raw && strcmp(o->perms[i].id_raw, id_raw) == 0)
            return &o->perms[i];
    return NULL;
}

void ac_perm_drop(ac_impl *o, ac_perm *p) {
    free(p->id_raw); free(p->allow_once); free(p->allow_always);
    free(p->reject); free(p->summary);
    int idx = (int)(p - o->perms);
    o->perms[idx] = o->perms[--o->nperms];
    memset(&o->perms[o->nperms], 0, sizeof(ac_perm));
}

void ac_perms_clear(ac_impl *o) {
    while (o->nperms) ac_perm_drop(o, &o->perms[o->nperms - 1]);
}

static void handle_permission(ac_impl *o, yyjson_val *msg, yyjson_val *params) {
    char *id_raw = acp_id_text(msg);
    if (o->nperms >= ACP_MAX_PERMS) {
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
            const char *oid = jget_str(op, "optionId");
            const char *kind = jget_str(op, "kind");
            if (!oid) continue;
            if (kind && strcmp(kind, "allow_always") == 0) {
                if (!p->allow_always) p->allow_always = xstrdup(oid);
            } else if (kind && strcmp(kind, "allow_once") == 0) {
                if (!p->allow_once) p->allow_once = xstrdup(oid);
            } else if (kind && (strcmp(kind, "reject_once") == 0 ||
                                strcmp(kind, "reject_always") == 0)) {
                if (!p->reject) p->reject = xstrdup(oid);
            } else if (!p->allow_once) {
                p->allow_once = xstrdup(oid); /* unknown kind: treat as allow-once */
            }
        }
    }
    if (p->allow_once) opts |= TNY_PERM_ALLOW_ONCE;
    if (p->allow_always) opts |= TNY_PERM_ALLOW_ALWAYS;
    if (p->reject) opts |= TNY_PERM_DENY;

    yyjson_val *tc = jget(params, "toolCall");
    const char *title = jget_str(tc, "title");
    const char *kind = jget_str(tc, "kind");
    buf_t sum;
    buf_init(&sum);
    buf_appendf(&sum, "%.200s%s%.40s", title ? title : "tool call",
                kind ? " (" : "", kind ? kind : "");
    if (kind) buf_appends(&sum, ")");
    p->summary = buf_detach(&sum);

    tny_event ev = {0};
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
        if (!t) t = jget_str(c, "newText");   /* diff blocks */
        if (!t) continue;
        if (out->len) buf_appends(out, "\n");
        buf_appends(out, t);
        if (out->len > ACP_DETAIL_CAP) break;
    }
}

static void update_tool_call(ac_impl *o, yyjson_val *u, const char *kind) {
    const char *id = jget_str(u, "toolCallId");
    const char *title = jget_str(u, "title");
    const char *status = jget_str(u, "status");
    bool done = status && (strcmp(status, "completed") == 0 ||
                           strcmp(status, "failed") == 0);
    buf_t detail;
    buf_init(&detail);
    append_tool_content(jget(u, "content"), &detail);
    if (strcmp(kind, "tool_call") == 0) {
        const char *tk = jget_str(u, "kind");
        tny_event ev = {0};
        ev.kind = TNY_EV_TOOL_START;
        ev.tool_name = title ? title : (tk ? tk : "tool");
        ev.tool_id = id;
        ev.tool_detail = detail.len ? detail.data : NULL;
        ac_emit(o, &ev);
    }
    if (done) {
        tny_event ev = {0};
        ev.kind = TNY_EV_TOOL_END;
        ev.tool_name = title ? title : "tool";
        ev.tool_id = id;
        ev.tool_detail = detail.len ? detail.data : NULL;
        ev.tool_ok = strcmp(status, "completed") == 0;
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

void ac_handle_update(ac_impl *o, yyjson_val *params) {
    yyjson_val *u = jget(params, "update");
    if (!u || !yyjson_is_obj(u)) return;
    const char *kind = jget_str(u, "sessionUpdate");
    if (!kind) return;

    if (strcmp(kind, "agent_message_chunk") == 0 ||
        strcmp(kind, "agent_thought_chunk") == 0) {
        yyjson_val *c = jget(u, "content");
        const char *type = jget_str(c, "type");
        const char *text = (!type || strcmp(type, "text") == 0)
                               ? jget_str(c, "text") : NULL;
        if (!text) return;
        bool thought = strcmp(kind, "agent_thought_chunk") == 0;
        ac_emit_text(o, thought ? TNY_EV_THINKING : TNY_EV_TEXT_DELTA,
                     text, strlen(text));
        return;
    }
    if (strcmp(kind, "user_message_chunk") == 0) return; /* our own echo */
    if (strcmp(kind, "tool_call") == 0 || strcmp(kind, "tool_call_update") == 0) {
        update_tool_call(o, u, kind);
        return;
    }
    if (strcmp(kind, "plan") == 0) {
        update_plan(o, u);
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

void ac_handle_agent_request(ac_impl *o, yyjson_val *msg, const char *method,
                             yyjson_val *params) {
    if (strcmp(method, "session/request_permission") == 0) {
        if (!o->turn_active) {
            char *id = acp_id_text(msg);
            ac_tx_result(o, id, "{\"outcome\":{\"outcome\":\"cancelled\"}}");
            free(id);
            return;
        }
        handle_permission(o, msg, params);
        return;
    }
    char *id = acp_id_text(msg);
    /* Cursor's ACP surface blocks on these; acknowledge so the turn moves on
     * (docs/backends/acp.md "Cursor-as-ACP"). tny has no answer to invent, so
     * the ack is empty and the request is surfaced as a status line. */
    if (strcmp(method, "cursor/ask_question") == 0 ||
        strcmp(method, "cursor/create_plan") == 0 ||
        strcmp(method, "cursor/update_todos") == 0 ||
        strcmp(method, "cursor/task") == 0) {
        const char *q = jget_str(params, "question");
        if (!q) q = jget_str(params, "title");
        buf_t s;
        buf_init(&s);
        buf_appendf(&s, "%.40s: %.200s", method, q ? q : "(no detail)");
        ac_emit_text(o, TNY_EV_STATUS, s.data, s.len);
        buf_free(&s);
        ac_tx_result(o, id, "{}");
        free(id);
        return;
    }
    buf_t m;
    buf_init(&m);
    buf_appendf(&m, "tny does not implement %.100s (no fs/terminal capabilities "
                    "advertised)", method);
    ac_tx_error(o, id, ACP_E_NO_METHOD, m.data);
    buf_free(&m);
    free(id);
}

/* ---------- the session/prompt response ends the turn ---------- */

void ac_handle_prompt_response(ac_impl *o, yyjson_val *msg) {
    yyjson_val *err = jget(msg, "error");
    if (err) {
        const char *m = jget_str(err, "message");
        buf_t b;
        buf_init(&b);
        buf_appendf(&b, "agent rejected the prompt: %.300s", m ? m : "unknown error");
        ac_emit_text(o, TNY_EV_ERROR, b.data, b.len);
        buf_free(&b);
        ac_emit_end(o, TNY_STOP_ERROR);
        return;
    }
    const char *reason = jget_str(jget(msg, "result"), "stopReason");
    if (!reason) reason = "end_turn";
    if (strcmp(reason, "end_turn") == 0) {
        ac_emit_end(o, TNY_STOP_DONE);
    } else if (strcmp(reason, "cancelled") == 0) {
        ac_emit_end(o, TNY_STOP_INTERRUPTED);
    } else if (strcmp(reason, "refusal") == 0) {
        ac_emit_text(o, TNY_EV_ERROR, "agent refused the request", 25);
        ac_emit_end(o, TNY_STOP_DENIED);
    } else if (strcmp(reason, "max_tokens") == 0 ||
               strcmp(reason, "max_turn_requests") == 0) {
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
