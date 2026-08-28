/* codex_items.c — rendering helpers for app-server "items" (the typed unit
 * behind item/started, the per-item delta notifications and item/completed)
 * plus token usage. Every field is host-supplied: type-check and cap it. */
#include "backends/codex/codex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const CX_ITEM_TYPE_KEYS[] = {"type", "itemType", "item_type", NULL};
const char *const CX_ITEM_ID_KEYS[] = {"id", "itemId", "item_id", NULL};
const char *const CX_STATUS_KEYS[] = {"status", "state", "outcome", NULL};
const char *const CX_TURN_ID_KEYS[] = {"turnId", "turn_id", NULL};
const char *const CX_TEXT_KEYS[] = {"text", "content", NULL};

const char *cx_first_str(yyjson_val *obj, const char *const *keys) {
    for (int i = 0; keys[i]; i++) {
        const char *v = jget_str(obj, keys[i]);
        if (v) return v;
    }
    return NULL;
}

static int64_t cx_first_int(yyjson_val *obj, const char *const *keys) {
    for (int i = 0; keys[i]; i++) {
        yyjson_val *v = jget(obj, keys[i]);
        if (v && yyjson_is_num(v)) return jget_int(obj, keys[i], 0);
    }
    return 0;
}

void cx_emit_capped(cx_impl *o, tny_event_kind k, const char *t) {
    if (!t || !*t) return;
    size_t n = strlen(t);
    if (n > CX_MAX_TEXT) n = CX_MAX_TEXT;
    tny_backend_event ev = {0};
    ev.kind = k;
    ev.text = t;
    ev.text_len = n;
    cx_emit(o, &ev);
}

yyjson_val *cx_item_of(yyjson_val *params) {
    yyjson_val *it = jget(params, "item");
    return it && yyjson_is_obj(it) ? it : params;
}

void cx_append_words(buf_t *b, yyjson_val *v) {
    if (!v) return;
    if (yyjson_is_str(v)) {
        const char *s = yyjson_get_str(v);
        buf_appendf(b, "%.*s", CX_MAX_DETAIL, s ? s : "");
        return;
    }
    if (!yyjson_is_arr(v)) return;
    size_t start = b->len;
    size_t idx, max;
    yyjson_val *e;
    yyjson_arr_foreach(v, idx, max, e) {
        if (!yyjson_is_str(e)) continue;
        if (b->len > start) buf_appends(b, " ");
        buf_appendf(b, "%.*s", CX_MAX_DETAIL, yyjson_get_str(e));
        if (b->len > CX_MAX_DETAIL) break;
    }
}

/* Text of an agentMessage/reasoning item: a string, or content parts. */
char *cx_item_text(yyjson_val *item) {
    static const char *const keys[] = {"text", "message", "content", "summary", NULL};
    const char *s = cx_first_str(item, keys);
    if (s) return xstrdup(s);
    yyjson_val *content = jget(item, "content");
    if (!content || !yyjson_is_arr(content)) return NULL;
    buf_t b;
    buf_init(&b);
    size_t idx, max;
    yyjson_val *e;
    yyjson_arr_foreach(content, idx, max, e) {
        const char *t = jget_str(e, "text");
        if (t) buf_appends(&b, t);
        if (b.len > CX_MAX_TEXT) break;
    }
    if (b.len) return buf_detach(&b);
    buf_free(&b);
    return NULL;
}

/* One-line human summary for a tool-ish item. Caller frees, may be NULL. */
char *cx_item_detail(const char *type, yyjson_val *item) {
    buf_t b;
    buf_init(&b);
    if (strcmp(type, "commandExecution") == 0 || strcmp(type, "command") == 0) {
        yyjson_val *cmd = jget(item, "command");
        if (!cmd) cmd = jget(item, "parsedCmd");
        if (!cmd) cmd = jget(item, "argv");
        cx_append_words(&b, cmd);
        const char *cwd = jget_str(item, "cwd");
        if (cwd && b.len) buf_appendf(&b, " (in %.200s)", cwd);
    } else if (strcmp(type, "fileChange") == 0 || strcmp(type, "patchApply") == 0) {
        yyjson_val *ch = jget(item, "changes");
        if (ch && yyjson_is_arr(ch)) {
            size_t idx, max, n = 0;
            yyjson_val *e;
            yyjson_arr_foreach(ch, idx, max, e) {
                const char *p = jget_str(e, "path");
                if (!p) continue;
                if (n++) buf_appends(&b, ", ");
                if (n > 6) {
                    buf_appends(&b, "…");
                    break;
                }
                const char *kind = jget_str(e, "kind");
                buf_appendf(&b, "%.200s%s%.24s%s", p, kind ? " (" : "", kind ? kind : "",
                            kind ? ")" : "");
            }
        } else if (ch && yyjson_is_obj(ch)) {
            size_t idx, max;
            yyjson_val *k, *v;
            yyjson_obj_foreach(ch, idx, max, k, v) {
                (void)v;
                if (!yyjson_is_str(k)) continue;
                if (idx) buf_appends(&b, ", ");
                if (idx > 5) {
                    buf_appends(&b, "…");
                    break;
                }
                buf_appendf(&b, "%.200s", yyjson_get_str(k));
            }
        }
        if (!b.len) {
            const char *p = jget_str(item, "path");
            if (p) buf_appendf(&b, "%.200s", p);
        }
    } else if (strcmp(type, "mcpToolCall") == 0) {
        static const char *const srv[] = {"server", "serverName", NULL};
        static const char *const tl[] = {"tool", "toolName", "name", NULL};
        const char *s = cx_first_str(item, srv), *t = cx_first_str(item, tl);
        buf_appendf(&b, "%.80s%s%.80s", s ? s : "", s && t ? "/" : "", t ? t : "");
    } else if (strcmp(type, "webSearch") == 0) {
        const char *q = jget_str(item, "query");
        if (q) buf_appendf(&b, "%.200s", q);
    }
    if (!b.len) {
        buf_free(&b);
        return NULL;
    }
    if (b.len > CX_MAX_DETAIL) {
        b.len = CX_MAX_DETAIL;
        b.data[b.len] = 0;
    }
    return buf_detach(&b);
}

bool cx_type_is_message(const char *type) {
    return strcmp(type, "agentMessage") == 0 || strcmp(type, "assistantMessage") == 0 ||
           strcmp(type, "message") == 0;
}

/* The host replays the prompt as a userMessage item; rendering it would echo
 * the user's own words back into the transcript. */
bool cx_item_is_user_echo(const char *type, yyjson_val *item) {
    if (strcmp(type, "userMessage") == 0) return true;
    if (strcmp(type, "message") == 0) {
        const char *r = jget_str(item, "role");
        return r && strcmp(r, "user") == 0;
    }
    return false;
}

bool cx_type_is_reasoning(const char *type) { return strstr(type, "easoning") != NULL; }

bool cx_type_is_plan(const char *type) {
    return strcmp(type, "todoList") == 0 || strcmp(type, "plan") == 0;
}

void cx_emit_plan(cx_impl *o, yyjson_val *item) {
    yyjson_val *arr = jget(item, "items");
    if (!arr) arr = jget(item, "todos");
    if (!arr || !yyjson_is_arr(arr)) return;
    buf_t b;
    buf_init(&b);
    size_t idx, max;
    yyjson_val *e;
    yyjson_arr_foreach(arr, idx, max, e) {
        static const char *const tk[] = {"text", "title", "step", NULL};
        const char *t = yyjson_is_str(e) ? yyjson_get_str(e) : cx_first_str(e, tk);
        if (!t) continue;
        const char *st = jget_str(e, "status");
        bool done = jget_bool(e, "completed", false) || (st && strcmp(st, "completed") == 0);
        if (b.len) buf_appends(&b, "\n");
        buf_appendf(&b, "%s %.200s", done ? "[x]" : "[ ]", t);
        if (b.len > CX_MAX_TEXT) break;
    }
    if (b.len) cx_emit_capped(o, TNY_EV_PLAN, b.data);
    buf_free(&b);
}

bool cx_item_ok(yyjson_val *item) {
    const char *st = cx_first_str(item, CX_STATUS_KEYS);
    if (st && (strcmp(st, "failed") == 0 || strcmp(st, "error") == 0 ||
               strcmp(st, "aborted") == 0 || strcmp(st, "declined") == 0 ||
               strcmp(st, "rejected") == 0 || strcmp(st, "cancelled") == 0))
        return false;
    yyjson_val *ec = jget(item, "exitCode");
    if (!ec) ec = jget(item, "exit_code");
    if (ec && yyjson_is_num(ec)) return yyjson_get_num(ec) == 0;
    return jget_bool(item, "success", true);
}

bool cx_emit_usage(cx_impl *o, yyjson_val *params) {
    static const char *const in_keys[] = {"input_tokens", "inputTokens", "prompt_tokens",
                                          "promptTokens", NULL};
    static const char *const out_keys[] = {"output_tokens", "outputTokens", "completion_tokens",
                                           "completionTokens", NULL};
    static const char *const holders[] = {
        "usage", "tokenUsage", "totalTokenUsage", "total_token_usage", "tokens", NULL};
    yyjson_val *u = NULL;
    for (int i = 0; holders[i] && !u; i++) {
        yyjson_val *h = jget(params, holders[i]);
        if (h && yyjson_is_obj(h)) u = h;
    }
    if (!u) {
        yyjson_val *info = jget(params, "info");
        for (int i = 0; holders[i] && !u; i++) {
            yyjson_val *h = jget(info, holders[i]);
            if (h && yyjson_is_obj(h)) u = h;
        }
    }
    if (!u) u = params;
    int64_t in = cx_first_int(u, in_keys), out = cx_first_int(u, out_keys);
    if (in <= 0 && out <= 0) return false;
    tny_backend_event ev = {0};
    ev.kind = TNY_EV_USAGE;
    ev.in_tokens = in;
    ev.out_tokens = out;
    cx_emit(o, &ev);
    return true;
}
