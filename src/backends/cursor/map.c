/* map.c — RunStreamMessage -> the normalized event set (docs/architecture.md).
 *
 * The bridge's oneof is `sdk_message | result | done` and `sdk_message` is a
 * `type` discriminator plus a Struct payload in `message` — the payload is the
 * @cursor/sdk stream event, so field lookups must unwrap that envelope first
 * (docs/backends/cursor-bridge.md, "Send stream"). `type` is one of system /
 * assistant / user / tool_call / thinking / status / task / usage. Field
 * spellings are accepted in both camelCase (Connect JSON) and proto
 * snake_case, and every lookup tolerates a missing or wrongly typed node:
 * host output is untrusted. */
#include "backends/cursor/impl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DETAIL_MAX 200

static uint64_t frame_hash(const char *payload, size_t len) {
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)payload[i];
        h *= UINT64_C(1099511628211);
    }
    return h ? h : 1;
}

bool cu_accept_frame(cu_impl *o, const char *payload, size_t len) {
    if (!o || o->stream_kind == CU_STREAM_NONE) return true;
    yyjson_doc *doc = jparse(payload, len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return true; /* keepalive or malformed frame: not part of durable replay */
    }
    const char *offset = jget_str(root, "offset");
    uint64_t hash = frame_hash(payload, len);
    /* Send may omit offsets while ObserveRun attaches one to the identical
     * durable envelope. Hash the semantic object without that transport
     * cursor so recovery cannot duplicate already rendered output. */
    if (yyjson_is_obj(root)) {
        yyjson_mut_doc *normalized = yyjson_doc_mut_copy(doc, jallocator());
        if (normalized) {
            yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(normalized), "offset");
            char *json = jwrite(normalized);
            if (json) {
                hash = frame_hash(json, strlen(json));
                free(json);
            }
            yyjson_mut_doc_free(normalized);
        }
    }

    if (o->stream_kind == CU_STREAM_SEND) {
        if (o->send_hash_count == o->send_hash_capacity) {
            size_t next_capacity = o->send_hash_capacity ? o->send_hash_capacity * 2 : 64;
            uint64_t *next = realloc(o->send_hashes, next_capacity * sizeof *next);
            if (next) {
                o->send_hashes = next;
                o->send_hash_capacity = next_capacity;
            }
        }
        if (o->send_hash_count < o->send_hash_capacity) o->send_hashes[o->send_hash_count++] = hash;
        yyjson_doc_free(doc);
        return true;
    }

    bool repeated_offset = offset && o->observe_offset && strcmp(offset, o->observe_offset) == 0;
    if (offset && *offset) {
        free(o->observe_offset);
        o->observe_offset = xstrdup(offset);
    }
    if (repeated_offset) {
        yyjson_doc_free(doc);
        return false;
    }
    if (o->observe_replay) {
        size_t match = o->replay_index;
        while (match < o->send_hash_count && o->send_hashes[match] != hash) match++;
        if (match < o->send_hash_count) {
            o->replay_index = match + 1;
            if (o->replay_index == o->send_hash_count) o->observe_replay = false;
            yyjson_doc_free(doc);
            return false;
        }
        o->observe_replay = false;
    }
    yyjson_doc_free(doc);
    return true;
}

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
    if (yyjson_is_str(v)) {
        buf_appends(out, yyjson_get_str(v));
        return;
    }
    if (yyjson_is_arr(v)) {
        size_t i, m;
        yyjson_val *e;
        yyjson_arr_foreach(v, i, m, e) collect_text(e, out, depth + 1);
        return;
    }
    if (!yyjson_is_obj(v)) return;
    yyjson_val *t = jget(v, "text");
    if (t && yyjson_is_str(t)) {
        buf_appends(out, yyjson_get_str(t));
        return;
    }
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

/* Token counts arrive as numbers on usage messages but as protojson
 * *strings* ("24530") inside RunResult (int64 fields) — accept both. */
static int64_t tok_count(yyjson_val *u, const char *key, int64_t dflt) {
    yyjson_val *v = jget(u, key);
    if (!v) return dflt;
    if (yyjson_is_int(v)) return yyjson_get_sint(v);
    if (yyjson_is_str(v)) {
        char *end = NULL;
        long long n = strtoll(yyjson_get_str(v), &end, 10);
        if (end && *end == '\0') return n;
    }
    return dflt;
}

static void take_usage(cu_impl *o, yyjson_val *u) {
    if (!u || !yyjson_is_obj(u)) return;
    yyjson_val *nested_usage = jget(u, "usage");
    if (yyjson_is_obj(nested_usage) && nested_usage != u) take_usage(o, nested_usage);
    int64_t in =
        tok_count(u, "inputTokens",
                  tok_count(u, "input_tokens",
                            tok_count(u, "promptTokens", tok_count(u, "prompt_tokens", -1))));
    int64_t out = tok_count(
        u, "outputTokens",
        tok_count(u, "output_tokens",
                  tok_count(u, "completionTokens", tok_count(u, "completion_tokens", -1))));
    if (in >= 0) o->in_tok = in;
    if (out >= 0) o->out_tok = out;
    yyjson_val *cost = jget(u, "cost");
    if (yyjson_is_obj(cost)) {
        yyjson_val *charged = jget(cost, "chargedCents");
        if (!charged) charged = jget(cost, "charged_cents");
        yyjson_val *raw = jget(cost, "rawCostCents");
        if (!raw) raw = jget(cost, "raw_cost_cents");
        yyjson_val *chosen = yyjson_is_num(charged) ? charged : raw;
        if (yyjson_is_num(chosen)) {
            o->cost = yyjson_get_num(chosen) / 100.0;
            o->has_cost = true;
        }
    }
}

/* "readToolCall" / "shell_tool_call" -> "read" / "shell". NULL when the key
 * carries no usable name. */
static const char *variant_tool_name(const char *key, char *buf, size_t cap) {
    if (!key) return NULL;
    size_t n = strlen(key);
    if (n > 8 && strcmp(key + n - 8, "ToolCall") == 0) n -= 8;
    else if (n > 10 && strcmp(key + n - 10, "_tool_call") == 0) n -= 10;
    if (!n || n >= cap) return NULL;
    memcpy(buf, key, n);
    buf[n] = '\0';
    return buf;
}

static void emit_tool(cu_impl *o, yyjson_val *v) {
    static const char *const END_SUB[] = {"completed", "complete", "finished", "end", "ended",
                                          "result",    "error",    "failed",   NULL};
    static const char *const BAD_SUB[] = {"error", "failed", NULL};
    const char *sub = jget_str(v, "subtype");
    if (!sub) sub = jget_str(v, "phase");
    if (!sub) sub = jget_str(v, "status");

    /* The SDK payload nests everything under a per-tool union:
     * tool_call.<variant>ToolCall = {args, result} — e.g. readToolCall,
     * shellToolCall, mcpToolCall (docs/backends/cursor-bridge.md,
     * "Tool call payloads"). Pick the union's first object member. */
    yyjson_val *un = jget(v, "tool_call");
    if (!un) un = jget(v, "toolCall");
    yyjson_val *call = NULL;
    const char *vkey = NULL;
    if (un && yyjson_is_obj(un)) {
        size_t i, m;
        yyjson_val *k, *val;
        yyjson_obj_foreach(un, i, m, k, val) {
            if (yyjson_is_obj(val)) {
                vkey = yyjson_get_str(k);
                call = val;
                break;
            }
        }
    }

    yyjson_val *result = call ? jget(call, "result") : NULL;
    if (!result) result = jget(v, "result");
    if (!result) result = jget(v, "output");
    bool end = sub ? is_one_of(sub, END_SUB) : result != NULL;

    char namebuf[64];
    const char *name = call ? jget_str(call, "name") : NULL; /* MCP tool name */
    if (!name) name = jget_str(v, "name");
    if (!name) name = str2(v, "toolName", "tool_name");
    if (!name) name = jget_str(v, "tool");
    if (!name) name = variant_tool_name(vkey, namebuf, sizeof namebuf);
    if (!name) name = "tool";
    const char *id = str2(v, "callId", "call_id");
    if (!id) id = str2(v, "toolCallId", "tool_call_id");
    if (!id) id = jget_str(v, "id");

    /* Results arrive wrapped. The bridge (observed live, sdk 1.0.28) sends
     * result = {"status":"success"|"error","value":{…}}; the CLI stream-json
     * spelling is result.success = {…} / result.error = "…". Unwrap either
     * so the clipped detail shows the interesting part, and a failure
     * wrapper implies tool_ok=false. */
    bool ok = !(jget_bool(v, "isError", false) || jget_bool(v, "is_error", false) ||
                jget(v, "error") != NULL || is_one_of(sub, BAD_SUB));
    if (result && yyjson_is_obj(result)) {
        const char *rstat = jget_str(result, "status");
        if (is_one_of(rstat, BAD_SUB)) ok = false;
        yyjson_val *err = jget(result, "error");
        if (!err) err = jget(result, "failure");
        if (!err) err = jget(result, "rejected");
        yyjson_val *succ = jget(result, "success");
        if (!succ) succ = jget(result, "value");
        if (err) {
            ok = false;
            if (!yyjson_is_null(err)) result = err;
        } else if (succ) {
            result = succ;
        }
    }

    yyjson_val *src = result;
    if (!end) {
        src = call ? jget(call, "args") : NULL;
        if (!src && call) src = jget(call, "rawArgs");
        if (!src) src = jget(v, "args");
        if (!src) src = jget(v, "arguments");
        if (!src) src = jget(v, "input");
    }
    buf_t detail;
    buf_init(&detail);
    append_short(src, &detail);
    /* completed frames repeat the args: fall back so TOOL_END is never bare */
    if (!detail.len && call) append_short(jget(call, "args"), &detail);

    /* The SDK re-emits `running` frames for one call while args stream in
     * (observed live): drop a start that repeats the previous one exactly,
     * but let a changed detail or a new call through. */
    if (!end) {
        buf_t sig;
        buf_init(&sig);
        buf_appendf(&sig, "%s\x1f%s\x1f", id ? id : "", name);
        buf_append(&sig, detail.data ? detail.data : "", detail.len);
        bool dup = o->last_tool_start.len == sig.len && sig.len &&
                   memcmp(o->last_tool_start.data, sig.data, sig.len) == 0;
        if (!dup) {
            buf_clear(&o->last_tool_start);
            buf_append(&o->last_tool_start, sig.data, sig.len);
        }
        buf_free(&sig);
        if (dup) {
            buf_free(&detail);
            return;
        }
    } else {
        buf_clear(&o->last_tool_start);
    }

    tny_backend_event ev = {0};
    ev.kind = end ? TNY_EV_TOOL_END : TNY_EV_TOOL_START;
    ev.tool_name = name;
    ev.tool_id = id;
    ev.tool_detail = detail.len ? detail.data : NULL;
    ev.tool_ok = ok;
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
    if (yyjson_is_str(v)) {
        handle_sdk_json_str(o, yyjson_get_str(v), depth);
        return;
    }
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
    take_usage(o, v);

    const char *type = jget_str(v, "type");
    if (!type) type = "";

    /* InteractionUpdate and ConversationStep carry their discriminator next
     * to a Struct rather than inside it. Preserve that type while mapping the
     * structured payload, including future additions we do not recognize. */
    yyjson_val *typed = jget(v, "update");
    if (!typed) typed = jget(v, "step");
    if (*type && yyjson_is_obj(typed)) {
        const char *inner_type = jget_str(typed, "type");
        v = typed;
        if (inner_type && *inner_type) type = inner_type;
    }

    /* SdkMessage is `type` + a Struct payload in `message`
     * (sdk_messages.proto): the payload is the real SDK event, which may
     * repeat `type`. Unwrap once so field lookups (tool_call union, status
     * text, run_id) see the event, not the envelope. Skipped when the inner
     * object carries a *different* type — that is a chat message, not an
     * envelope (e.g. the payload's own assistant `message`). */
    yyjson_val *inner = jget(v, "message");
    if (*type && inner && yyjson_is_obj(inner)) {
        const char *itype = jget_str(inner, "type");
        if (!itype || strcmp(itype, type) == 0) {
            v = inner;
            rid = str2(v, "runId", "run_id");
            if (rid && *rid && !o->run_id) o->run_id = xstrdup(rid);
            take_usage(o, v);
        }
    }

    const char *message_id = str2(v, "messageId", "message_id");
    if (!message_id) message_id = jget_str(v, "id");

    if (strcmp(type, "assistant") == 0 || strcmp(type, "text") == 0 ||
        strcmp(type, "text_delta") == 0 || strcmp(type, "textDelta") == 0) {
        buf_t t;
        buf_init(&t);
        collect_text(v, &t, 0);
        if (t.len) {
            if (o->text_source != CU_TEXT_NONE && o->text_source != o->mapping_source) {
                buf_free(&t);
                return;
            }
            if (o->text_source == CU_TEXT_NONE) o->text_source = o->mapping_source;
            o->got_text = true;
            tny_backend_event ev = {0};
            ev.kind = TNY_EV_TEXT_DELTA;
            ev.text = t.data;
            ev.text_len = t.len;
            ev.message_id = message_id;
            cu_emit(o, &ev);
        }
        buf_free(&t);
    } else if (strcmp(type, "thinking") == 0 || strcmp(type, "reasoning") == 0 ||
               strcmp(type, "thinking_delta") == 0 || strcmp(type, "thinkingDelta") == 0 ||
               strcmp(type, "summary") == 0) {
        buf_t t;
        buf_init(&t);
        collect_text(v, &t, 0);
        if (t.len) {
            tny_backend_event ev = {0};
            ev.kind = TNY_EV_THINKING;
            ev.text = t.data;
            ev.text_len = t.len;
            ev.message_id = message_id;
            cu_emit(o, &ev);
        }
        buf_free(&t);
    } else if (strcmp(type, "partial_tool_call") == 0) {
        const char *id = str2(v, "callId", "call_id");
        const char *name = jget_str(v, "name");
        yyjson_val *detail_value = jget(v, "args");
        if (!detail_value) detail_value = jget(v, "delta");
        buf_t detail;
        buf_init(&detail);
        append_short(detail_value, &detail);
        tny_backend_event ev = {0};
        ev.kind = TNY_EV_TOOL_PROGRESS;
        ev.tool_name = name ? name : "tool";
        ev.tool_id = id;
        ev.tool_detail = detail.data;
        cu_emit(o, &ev);
        buf_free(&detail);
    } else if (strcmp(type, "tool_call") == 0 || strcmp(type, "tool_use") == 0 ||
               strcmp(type, "tool_result") == 0 || strcmp(type, "tool_call_started") == 0 ||
               strcmp(type, "tool_call_completed") == 0) {
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
    /* RunStreamResult.result is a RunResult: final text in `result`,
     * usage in `usage` (sdk_messages.proto). */
    yyjson_val *rr = jget(r, "result");
    if (!yyjson_is_obj(rr)) rr = NULL;
    take_usage(o, r);
    if (rr) take_usage(o, rr);
    const char *sub = jget_str(r, "subtype");
    const char *st = jget_str(r, "status"); /* RunLifecycleStatus enum name */
    if ((!st || !*st) && rr) st = jget_str(rr, "status");
    const char *rid = str2(r, "runId", "run_id");
    if ((!rid || !*rid) && rr) rid = str2(rr, "runId", "run_id");
    if (rid && *rid && (!o->run_id || strcmp(o->run_id, rid) != 0)) {
        free(o->run_id);
        o->run_id = xstrdup(rid);
    }
    const char *code = str2(r, "errorCode", "error_code");
    bool cancelled = st && (strstr(st, "CANCELLED") || strcmp(st, "cancelled") == 0);
    bool pending = st && (strstr(st, "CREATING") || strstr(st, "RUNNING") ||
                          strcmp(st, "creating") == 0 || strcmp(st, "running") == 0);
    bool finished = !st || !*st || strstr(st, "FINISHED") || strcmp(st, "finished") == 0;
    bool bad = (code && *code) || jget_bool(r, "isError", false) ||
               jget_bool(r, "is_error", false) || jget(r, "error") != NULL ||
               (sub && str_starts(sub, "error")) ||
               (st && (strstr(st, "ERROR") || strstr(st, "EXPIRED") || strcmp(st, "error") == 0 ||
                       strcmp(st, "expired") == 0));

    if (!o->got_text) {
        buf_t t;
        buf_init(&t);
        collect_text(r, &t, 0);
        if (!t.len && rr) {
            const char *txt = jget_str(rr, "result");
            if (txt) buf_appends(&t, txt);
        }
        if (t.len) {
            o->got_text = true;
            cu_emit_text(o, TNY_EV_TEXT_DELTA, t.data, t.len);
        }
        buf_free(&t);
    }
    if (pending) return;
    if (!finished && !cancelled && !bad) {
        buf_t unknown;
        buf_init(&unknown);
        buf_appendf(&unknown, "cursor run ended with unknown status %s", st ? st : "?");
        cu_emit_text(o, TNY_EV_ERROR, unknown.data, unknown.len);
        buf_free(&unknown);
        cu_end_turn(o, TNY_STOP_ERROR);
        return;
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
    o->saw_terminal_result = true;
    cu_end_turn(o, cancelled ? TNY_STOP_INTERRUPTED : TNY_STOP_DONE);
}

/* EndStreamResponse: JSON with an optional "error". */
static void handle_end_frame(cu_impl *o, const char *payload, size_t len) {
    yyjson_doc *doc = len ? jparse(payload, len) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool bad = jget(root, "error") != NULL;
    if (bad && !o->ended) {
        char line[320];
        cursor_error_line(payload, len, "the bridge ended the run with an error", line,
                          sizeof line);
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
    if (flags & CONNECT_FLAG_END) {
        handle_end_frame(o, payload, len);
        return;
    }
    if (o->ended) return;
    if (!cu_accept_frame(o, payload, len)) return;

    yyjson_doc *doc = jparse(payload, len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return;
    }

    yyjson_val *sm = jget(root, "sdkMessage");
    if (!sm) sm = jget(root, "sdk_message");
    if (sm) {
        o->mapping_source = CU_TEXT_SDK;
        handle_sdk(o, sm, 0);
    }

    /* enableDeltas exposes the SDK's lower-level InteractionUpdate union.
     * It is another typed Struct: route recognized text/thinking/tool/status
     * variants through the same defensive normalizer and ignore unknowns. */
    yyjson_val *interaction = jget(root, "interactionUpdate");
    if (!interaction) interaction = jget(root, "interaction_update");
    if (interaction) {
        o->mapping_source = CU_TEXT_INTERACTION;
        handle_sdk(o, interaction, 0);
    }

    yyjson_val *step = jget(root, "step");
    if (step) {
        const char *type = jget_str(step, "type");
        /* Completed assistant steps restate text already delivered through
         * sdkMessage/interaction deltas. Keep them as a fallback only. */
        o->mapping_source = CU_TEXT_STEP;
        if (!o->got_text || !type || strcmp(type, "assistant") != 0) handle_sdk(o, step, 0);
    }

    yyjson_val *res = jget(root, "result");
    if (res && yyjson_is_obj(res)) handle_result(o, res);
    else if (jget(root, "done") && !o->ended) o->saw_done = true;

    yyjson_doc_free(doc);
}
