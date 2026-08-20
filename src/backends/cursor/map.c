/* map.c — RunStreamMessage -> the normalized event set (docs/architecture.md).
 *
 * The bridge's oneof is `sdk_message | result | done` and `sdk_message.type`
 * is one of system / assistant / user / tool_call / thinking / status / task /
 * usage (docs/backends/cursor-bridge.md, "Send stream"). Field spellings are
 * accepted in both camelCase (Connect JSON) and proto snake_case, and every
 * lookup tolerates a missing or wrongly typed node: host output is untrusted. */
#include "backends/cursor/impl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DETAIL_MAX 200

static const char *str2(yyjson_val *v, const char *a, const char *b) {
    const char *s = jget_str(v, a);
    return s ? s : jget_str(v, b);
}

static bool is_one_of(const char *s, const char *const *set) {
    if (!s) return false;
    for (int i = 0; set[i]; i++)
        if (strcmp(s, set[i]) == 0) return true;
    return false;
}

/* Flatten any text-bearing node (string, content part array, message, delta). */
static void collect_text(yyjson_val *v, buf_t *out, int depth) {
    if (!v || depth > 4 || out->len > 64u * 1024u) return;
    if (yyjson_is_str(v)) { buf_appends(out, yyjson_get_str(v)); return; }
    if (yyjson_is_arr(v)) {
        size_t i, m;
        yyjson_val *e;
        yyjson_arr_foreach(v, i, m, e) collect_text(e, out, depth + 1);
        return;
    }
    if (!yyjson_is_obj(v)) return;
    yyjson_val *t = jget(v, "text");
    if (t && yyjson_is_str(t)) { buf_appends(out, yyjson_get_str(t)); return; }
    yyjson_val *next = jget(v, "delta");
    if (!next) next = jget(v, "content");
    if (!next) next = jget(v, "message");
    collect_text(next, out, depth + 1);
}

/* One short single-line summary of an argument or result node. */
static void append_short(yyjson_val *v, buf_t *out) {
    if (!v) return;
    char *owned = NULL;
    const char *s;
    if (yyjson_is_str(v)) {
        s = yyjson_get_str(v);
    } else {
        owned = jwrite_val(v);
        s = owned;
    }
    if (!s) return;
    size_t n = strlen(s), take = n > DETAIL_MAX ? DETAIL_MAX : n, start = out->len;
    buf_append(out, s, take);
    for (size_t i = start; i < out->len; i++)
        if (out->data[i] == '\n' || out->data[i] == '\r' || out->data[i] == '\t')
            out->data[i] = ' ';
    if (n > take) buf_appends(out, "...");
    free(owned);
}

static void take_usage(cu_impl *o, yyjson_val *u) {
    if (!u || !yyjson_is_obj(u)) return;
    int64_t in = jget_int(u, "inputTokens",
                 jget_int(u, "input_tokens",
                 jget_int(u, "promptTokens", jget_int(u, "prompt_tokens", -1))));
    int64_t out = jget_int(u, "outputTokens",
                  jget_int(u, "output_tokens",
                  jget_int(u, "completionTokens", jget_int(u, "completion_tokens", -1))));
    if (in >= 0) o->in_tok = in;
    if (out >= 0) o->out_tok = out;
}

static void emit_tool(cu_impl *o, yyjson_val *v) {
    static const char *const END_SUB[] = {"completed", "complete", "finished", "end",
                                          "ended",     "result",   "error",    "failed", NULL};
    static const char *const BAD_SUB[] = {"error", "failed", NULL};
    const char *sub = jget_str(v, "subtype");
    if (!sub) sub = jget_str(v, "phase");
    if (!sub) sub = jget_str(v, "status");
    yyjson_val *result = jget(v, "result");
    if (!result) result = jget(v, "output");
    bool end = sub ? is_one_of(sub, END_SUB) : result != NULL;

    const char *name = jget_str(v, "name");
    if (!name) name = str2(v, "toolName", "tool_name");
    if (!name) name = jget_str(v, "tool");
    if (!name) name = "tool";
    const char *id = str2(v, "toolCallId", "tool_call_id");
    if (!id) id = jget_str(v, "id");

    yyjson_val *src = result;
    if (!end) {
        src = jget(v, "args");
        if (!src) src = jget(v, "arguments");
        if (!src) src = jget(v, "input");
    }
    buf_t detail;
    buf_init(&detail);
    append_short(src, &detail);

    tny_event ev = {0};
    ev.kind = end ? TNY_EV_TOOL_END : TNY_EV_TOOL_START;
    ev.tool_name = name;
    ev.tool_id = id;
    ev.tool_detail = detail.len ? detail.data : NULL;
    ev.tool_ok = !(jget_bool(v, "isError", false) || jget_bool(v, "is_error", false) ||
                   jget(v, "error") != NULL || is_one_of(sub, BAD_SUB));
    cu_emit(o, &ev);
    buf_free(&detail);
}

static void handle_sdk(cu_impl *o, yyjson_val *v, int depth);

static void handle_sdk_json_str(cu_impl *o, const char *s, int depth) {
    size_t n = strlen(s);
    if (!n || n > CURSOR_MAX_MSG_BYTES) return;
    yyjson_doc *d = jparse(s, n);
    if (!d) return;
    handle_sdk(o, yyjson_doc_get_root(d), depth + 1);
    yyjson_doc_free(d);
}

static void handle_sdk(cu_impl *o, yyjson_val *v, int depth) {
    if (!v || depth > 2 || o->ended) return;
    if (yyjson_is_str(v)) { handle_sdk_json_str(o, yyjson_get_str(v), depth); return; }
    if (!yyjson_is_obj(v)) return;
    /* some builds carry the SDK payload as an opaque JSON string field */
    yyjson_val *nested = jget(v, "json");
    if (!nested) nested = jget(v, "payload");
    if (nested && yyjson_is_str(nested)) {
        handle_sdk_json_str(o, yyjson_get_str(nested), depth);
        return;
    }

    const char *rid = str2(v, "runId", "run_id");
    if (rid && *rid && !o->run_id) o->run_id = xstrdup(rid);
    take_usage(o, jget(v, "usage"));

    const char *type = jget_str(v, "type");
    if (!type) type = "";

    if (strcmp(type, "assistant") == 0 || strcmp(type, "text") == 0) {
        buf_t t;
        buf_init(&t);
        collect_text(v, &t, 0);
        if (t.len) {
            o->got_text = true;
            cu_emit_text(o, TNY_EV_TEXT_DELTA, t.data, t.len);
        }
        buf_free(&t);
    } else if (strcmp(type, "thinking") == 0 || strcmp(type, "reasoning") == 0) {
        buf_t t;
        buf_init(&t);
        collect_text(v, &t, 0);
        if (t.len) cu_emit_text(o, TNY_EV_THINKING, t.data, t.len);
        buf_free(&t);
    } else if (strcmp(type, "tool_call") == 0 || strcmp(type, "tool_use") == 0 ||
               strcmp(type, "tool_result") == 0) {
        emit_tool(o, v);
    } else if (strcmp(type, "status") == 0 || strcmp(type, "system") == 0) {
        buf_t t;
        buf_init(&t);
        const char *msg = jget_str(v, "message");
        if (msg) buf_appends(&t, msg);
        else collect_text(v, &t, 0);
        if (!t.len && strcmp(type, "system") == 0 && tny_debug()) {
            const char *sub = jget_str(v, "subtype"); /* lifecycle noise */
            buf_appendf(&t, "cursor session %s", sub ? sub : "started");
        }
        if (t.len) {
            if (strcmp(type, "status") == 0) {
                buf_clear(&o->last_status);
                buf_append(&o->last_status, t.data, t.len);
            }
            cu_emit_text(o, TNY_EV_STATUS, t.data, t.len);
        }
        buf_free(&t);
    } else if (strcmp(type, "task") == 0 || strcmp(type, "plan") == 0 ||
               strcmp(type, "todo") == 0) {
        buf_t t;
        buf_init(&t);
        collect_text(v, &t, 0);
        if (t.len) cu_emit_text(o, TNY_EV_PLAN, t.data, t.len);
        buf_free(&t);
    } else if (strcmp(type, "usage") == 0) {
        take_usage(o, v);
    } else if (strcmp(type, "error") == 0) {
        buf_t t;
        buf_init(&t);
        collect_text(v, &t, 0);
        if (!t.len) buf_appends(&t, "the cursor agent reported an error");
        o->saw_error = true;
        cu_emit_text(o, TNY_EV_ERROR, t.data, t.len);
        buf_free(&t);
    }
    /* "user" and unknown types: nothing to render */
}

static void handle_result(cu_impl *o, yyjson_val *r) {
    if (o->ended) return;
    take_usage(o, jget(r, "usage"));
    const char *sub = jget_str(r, "subtype");
    const char *code = str2(r, "errorCode", "error_code");
    bool bad = (code && *code) || jget_bool(r, "isError", false) ||
               jget_bool(r, "is_error", false) || jget(r, "error") != NULL ||
               (sub && str_starts(sub, "error"));

    if (!o->got_text) {
        buf_t t;
        buf_init(&t);
        collect_text(r, &t, 0);
        if (t.len) {
            o->got_text = true;
            cu_emit_text(o, TNY_EV_TEXT_DELTA, t.data, t.len);
        }
        buf_free(&t);
    }
    if (bad || o->saw_error) {
        buf_t m;
        buf_init(&m);
        buf_appends(&m, "cursor run failed: ");
        if (o->last_status.len) buf_append(&m, o->last_status.data, o->last_status.len);
        else if (code && *code) buf_appends(&m, code);
        else buf_appends(&m, "the bridge reported an error with no message");
        cu_emit_text(o, TNY_EV_ERROR, m.data, m.len);
        buf_free(&m);
        cu_end_turn(o, TNY_STOP_ERROR);
        return;
    }
    cu_end_turn(o, TNY_STOP_DONE);
}

/* EndStreamResponse: JSON with an optional "error". */
static void handle_end_frame(cu_impl *o, const char *payload, size_t len) {
    yyjson_doc *doc = len ? jparse(payload, len) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool bad = jget(root, "error") != NULL;
    if (bad && !o->ended) {
        char line[320];
        cursor_error_line(payload, len, "the bridge ended the run with an error",
                          line, sizeof line);
        cu_emit_text(o, TNY_EV_ERROR, line, strlen(line));
        cu_end_turn(o, TNY_STOP_ERROR);
    } else if (!o->ended) {
        cu_end_turn(o, o->saw_error ? TNY_STOP_ERROR : TNY_STOP_DONE);
    }
    yyjson_doc_free(doc);
}

void cu_on_frame(uint8_t flags, const char *payload, size_t len, void *ud) {
    cu_impl *o = ud;
    if (len > CURSOR_MAX_MSG_BYTES) return;
    if (flags & CONNECT_FLAG_END) { handle_end_frame(o, payload, len); return; }
    if (o->ended) return;

    yyjson_doc *doc = jparse(payload, len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!root || !yyjson_is_obj(root)) { yyjson_doc_free(doc); return; }

    yyjson_val *sm = jget(root, "sdkMessage");
    if (!sm) sm = jget(root, "sdk_message");
    if (sm) handle_sdk(o, sm, 0);

    yyjson_val *res = jget(root, "result");
    if (res && yyjson_is_obj(res)) handle_result(o, res);
    else if (jget(root, "done") && !o->ended)
        cu_end_turn(o, o->saw_error ? TNY_STOP_ERROR : TNY_STOP_DONE);

    yyjson_doc_free(doc);
}
