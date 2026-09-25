#include "json/ownership.hpp"
#include "backends/openai/stream_decode.h"
#include <array>
#include <cstring>

namespace {
using tny::required;
void check(bool ok) {
    if (!ok) throw std::bad_alloc();
}
void put(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, yyjson_mut_val *value) {
    check(yyjson_mut_obj_put(obj, required(yyjson_mut_strcpy(doc, key)), required(value)));
}
void merge_details(yyjson_mut_doc *doc, yyjson_mut_val *arr, yyjson_val *details) {
    size_t di, dn;
    yyjson_val *frag;
    yyjson_arr_foreach(details, di, dn, frag) {
        if (!yyjson_is_obj(frag)) continue;
        yyjson_val *iv = jget(frag, "index");
        int64_t index = yyjson_is_int(iv) ? yyjson_get_sint(iv) : -1;
        yyjson_mut_val *item = nullptr;
        if (index >= 0) {
            size_t ai, an;
            yyjson_mut_val *cand;
            yyjson_mut_arr_foreach(arr, ai, an, cand) {
                yyjson_mut_val *ci = yyjson_mut_obj_get(cand, "index");
                if (yyjson_mut_is_int(ci) && yyjson_mut_get_sint(ci) == index) {
                    item = cand;
                    break;
                }
            }
        }
        if (!item) {
            check(yyjson_mut_arr_add_val(arr, required(yyjson_val_mut_copy(doc, frag))));
            continue;
        }
        yyjson_obj_iter it = yyjson_obj_iter_with(frag);
        yyjson_val *key;
        while ((key = yyjson_obj_iter_next(&it))) {
            yyjson_val *value = yyjson_obj_iter_get_val(key);
            const char *name = yyjson_get_str(key);
            if (!name || !value || yyjson_is_null(value)) continue;
            yyjson_mut_val *have = yyjson_mut_obj_get(item, name);
            bool textual = std::strcmp(name, "text") == 0 || std::strcmp(name, "summary") == 0 ||
                           std::strcmp(name, "data") == 0;
            if (textual && yyjson_is_str(value) && yyjson_mut_is_str(have)) {
                tny::string joined(yyjson_mut_get_str(have), yyjson_mut_get_len(have));
                joined.append(yyjson_get_str(value), yyjson_get_len(value));
                put(doc, item, name, yyjson_mut_strncpy(doc, joined.data(), joined.size()));
            } else if (!have || yyjson_mut_is_null(have))
                put(doc, item, name, yyjson_val_mut_copy(doc, value));
        }
    }
}
struct decoder_state {
    tny::mutable_document doc;
    yyjson_mut_val *details = nullptr, *items = nullptr, *hosted_items = nullptr;
    tny::string reasoning;
    decoder_state() = default;
    decoder_state(const decoder_state &) = delete;
    decoder_state &operator=(const decoder_state &) = delete;
    decoder_state(decoder_state &&) = default;
    decoder_state &operator=(decoder_state &&) = default;
    yyjson_mut_val *array(yyjson_mut_val *&slot) {
        if (!doc) doc = tny::make_document();
        if (!slot) slot = required(yyjson_mut_arr(doc.get()));
        return slot;
    }
    void reasoning_item(yyjson_val *item) {
        const char *enc = jget_str(item, "encrypted_content");
        if (!enc || !*enc) return;
        auto *arr = array(items);
        const char *id = jget_str(item, "id");
        yyjson_mut_val *copy = nullptr;
        if (id) {
            size_t i, n;
            yyjson_mut_val *old;
            yyjson_mut_arr_foreach(arr, i, n, old) {
                const char *oid = yyjson_mut_get_str(yyjson_mut_obj_get(old, "id"));
                if (oid && std::strcmp(oid, id) == 0) {
                    copy = old;
                    break;
                }
            }
        }
        if (!copy) {
            copy = required(yyjson_mut_obj(doc.get()));
            check(yyjson_mut_arr_add_val(arr, copy));
        }
        size_t i, n;
        yyjson_val *key, *value;
        yyjson_obj_foreach(item, i, n, key, value) {
            const char *name = yyjson_get_str(key);
            /* Output-only status is not an input reasoning member. Keep
             * unknown provider payload fields and replace only present fields. */
            if (std::strcmp(name, "status") == 0) continue;
            put(doc.get(), copy, name, yyjson_val_mut_copy(doc.get(), value));
        }
    }
};
/* Common events require no queue allocation. Overflow remains unbounded and
 * fault-injected. All borrowed views refer to the input doc until dispatch;
 * generated strings rebuild their views after moves. */
struct action {
    oa_decoded_event event{};
    tny::string generated;
    bool has_generated = false;
    action() = default;
    action(const action &) = delete;
    action &operator=(const action &) = delete;
    action(action &&) = default;
    action &operator=(action &&) = default;
};
struct batch {
    std::array<action, 8> local;
    tny::vector<action> overflow;
    size_t count = 0;
    action &add(oa_decoded_kind kind) {
        action *a;
        if (count < local.size()) a = &local[count];
        else {
            overflow.emplace_back();
            a = &overflow.back();
        }
        ++count;
        a->event.kind = kind;
        return *a;
    }
    void text(oa_decoded_kind kind, const char *text, size_t len) {
        if (!text || !len) return;
        auto &a = add(kind);
        a.event.text = text;
        a.event.len = len;
    }
    void value(oa_decoded_kind kind, yyjson_val *v) {
        auto &a = add(kind);
        a.event.value = v;
    }
    void finish(const char *reason, tny_stop_reason stop) {
        auto &a = add(OA_DECODE_FINISH);
        a.event.text = reason;
        a.event.stop = stop;
    }
    int dispatch(oa_decoded_cb cb, void *ud) {
        for (size_t i = 0; i < count; ++i) {
            auto &a = i < local.size() ? local[i] : overflow[i - local.size()];
            if (a.has_generated) {
                a.event.text = a.generated.data();
                a.event.len = a.generated.size();
            }
            int rc = cb(&a.event, ud);
            if (rc) return rc;
        }
        return TNY_PARSE_OK;
    }
};
bool is(yyjson_val *value, const char *key, const char *expected) {
    const char *s = jget_str(value, key);
    return s && std::strcmp(s, expected) == 0;
}
bool error_value(yyjson_val *v) {
    return yyjson_is_obj(v) || (yyjson_is_str(v) && yyjson_get_len(v));
}
void failure(batch &b, yyjson_val *err) {
    b.value(OA_DECODE_ERROR, err);
    b.add(OA_DECODE_DONE);
}
bool number(yyjson_val *v, int64_t &out) {
    if (yyjson_is_int(v)) {
        out = yyjson_get_sint(v);
        return true;
    }
    if (yyjson_is_real(v)) {
        double value = yyjson_get_real(v);
        if (value >= -0x1p63 && value < 0x1p63) {
            out = static_cast<int64_t>(value);
            return true;
        }
    }
    return false;
}
int64_t output_index(yyjson_val *root, int64_t fallback) {
    yyjson_val *v = jget(root, "output_index");
    if (!v) return fallback;
    int64_t index = -1;
    (void)number(v, index);
    return index;
}
void usage(batch &b, yyjson_val *value, bool chat) {
    yyjson_val *input = jget(value, chat ? "prompt_tokens" : "input_tokens");
    yyjson_val *output = jget(value, chat ? "completion_tokens" : "output_tokens");
    if (!yyjson_is_int(input) && !yyjson_is_int(output)) return;
    auto &event = b.add(OA_DECODE_USAGE).event;
    if (number(input, event.input_tokens)) event.usage_fields |= 1;
    if (number(output, event.output_tokens)) event.usage_fields |= 2;
    yyjson_val *details = jget(value, chat ? "prompt_tokens_details" : "input_tokens_details");
    if (number(jget(details, "cached_tokens"), event.cached_tokens)) event.usage_fields |= 4;
    if (number(jget(details, "cache_write_tokens"), event.cache_write_tokens))
        event.usage_fields |= 8;
}
void hosted_item(decoder_state &s, batch &b, yyjson_val *item, bool hosted) {
    if (!hosted) {
        if (is(item, "type", "web_search_call")) failure(b, jget(item, "type"));
        return;
    }
    const char *type = jget_str(item, "type"), *id = jget_str(item, "id");
    if (!type || !id) return;
    bool search = std::strcmp(type, "web_search_call") == 0;
    if (!search && std::strcmp(type, "message") != 0) return;
    bool done = is(item, "status", "completed") || is(item, "status", "failed");
    if (!search && !done) return;
    auto *arr = s.array(s.hosted_items);
    bool found = false, was_done = false;
    size_t i, n;
    yyjson_mut_val *old;
    yyjson_mut_arr_foreach(arr, i, n, old) {
        const char *oid = yyjson_mut_get_str(yyjson_mut_obj_get(old, "id"));
        if (!oid || std::strcmp(id, oid) != 0) continue;
        found = true;
        const char *status = yyjson_mut_get_str(yyjson_mut_obj_get(old, "status"));
        was_done =
            status && (std::strcmp(status, "completed") == 0 || std::strcmp(status, "failed") == 0);
        if (!was_done || done)
            required(
                yyjson_mut_arr_replace(arr, i, required(yyjson_val_mut_copy(s.doc.get(), item))));
        break;
    }
    if (!found)
        check(yyjson_mut_arr_add_val(arr, required(yyjson_val_mut_copy(s.doc.get(), item))));
    if (search) {
        if (!found) b.add(OA_DECODE_HOSTED_START).event.text = id;
        if (done && !was_done) {
            auto &a = b.add(OA_DECODE_HOSTED_END);
            a.event.text = id;
            a.event.ok = is(item, "status", "completed");
        }
    } else if (!was_done) {
        size_t pi, pn, ai, an;
        yyjson_val *part, *annotation;
        yyjson_arr_foreach(jget(item, "content"), pi, pn, part) {
            yyjson_arr_foreach(jget(part, "annotations"), ai, an, annotation) {
                const char *url = jget_str(annotation, "url");
                if (!is(annotation, "type", "url_citation") || !url ||
                    (std::strncmp(url, "https://", 8) != 0 && std::strncmp(url, "http://", 7) != 0))
                    continue;
                const char *title = jget_str(annotation, "title");
                auto &a = b.add(OA_DECODE_TEXT);
                a.has_generated = true;
                a.generated = "\n[";
                a.generated += title ? title : "Source";
                a.generated += "](";
                a.generated += url;
                a.generated += ")\n";
            }
        }
    }
}
void hosted_output(decoder_state &s, batch &b, yyjson_val *response, bool hosted) {
    size_t i, n;
    yyjson_val *item;
    yyjson_arr_foreach(jget(response, "output"), i, n, item) {
        hosted_item(s, b, item, hosted);
        if (is(item, "type", "reasoning")) s.reasoning_item(item);
    }
}
void chat_decode(decoder_state &s, batch &b, oa_callset *calls, yyjson_val *root) {
    yyjson_val *err = jget(root, "error");
    if (error_value(err)) {
        failure(b, err);
        return;
    }
    usage(b, jget(root, "usage"), true);
    yyjson_val *choice = yyjson_arr_get_first(jget(root, "choices"));
    if (!choice) return;
    const char *fr = jget_str(choice, "finish_reason");
    if (fr) {
        tny_stop_reason stop = TNY_STOP_DONE;
        if (!std::strcmp(fr, "length")) stop = TNY_STOP_STEP_LIMIT;
        else if (!std::strcmp(fr, "content_filter")) stop = TNY_STOP_DENIED;
        else if (!std::strcmp(fr, "error")) failure(b, nullptr);
        b.finish(fr, stop);
    }
    yyjson_val *delta = jget(choice, "delta");
    if (!delta) delta = jget(choice, "message");
    size_t len = 0;
    const char *content = jget_strn(delta, "content", &len);
    b.text(OA_DECODE_TEXT, content, len);
    const char *reasoning = jget_strn(delta, "reasoning_content", &len);
    if (reasoning && len) {
        s.reasoning.append(reasoning, len);
        b.text(OA_DECODE_THINKING, reasoning, len);
    } else {
        const char *text = jget_strn(delta, "reasoning", &len);
        b.text(OA_DECODE_THINKING, text, len);
    }
    yyjson_val *details = jget(delta, "reasoning_details");
    if (yyjson_is_arr(details)) {
        auto *arr = s.array(s.details);
        merge_details(s.doc.get(), arr, details);
        if (!reasoning && !jget(delta, "reasoning")) {
            size_t i, n;
            yyjson_val *detail;
            yyjson_arr_foreach(details, i, n, detail) {
                const char *text = jget_strn(detail, "text", &len);
                if (!text) text = jget_strn(detail, "summary", &len);
                b.text(OA_DECODE_THINKING, text, len);
            }
        }
    }
    size_t i, n;
    yyjson_val *tc;
    yyjson_arr_foreach(jget(delta, "tool_calls"), i, n, tc) {
        yyjson_val *name = jget(jget(tc, "function"), "name");
        if (yyjson_is_str(name) && strlen(yyjson_get_str(name)) != yyjson_get_len(name)) {
            failure(b, name);
            return;
        }
    }
    if (oa_calls_feed(calls, jget(delta, "tool_calls")) != TNY_PARSE_OK) throw std::bad_alloc();
}
void incomplete(batch &b, yyjson_val *response) {
    const char *reason = jget_str(jget(response, "incomplete_details"), "reason");
    b.finish(nullptr, reason && std::strstr(reason, "content_filter") ? TNY_STOP_DENIED
                                                                      : TNY_STOP_STEP_LIMIT);
}
void call_item(oa_callset *calls, int64_t index, yyjson_val *item, bool whole = false) {
    const char *args = jget_str(item, "arguments");
    size_t name_len = 0;
    const char *name = jget_strn(item, "name", &name_len);
    if (name && strlen(name) != name_len) name = "invalid tool name";
    if (oa_calls_item(calls, index, jget_str(item, "call_id"), name,
                      args && (*args || whole) ? args : nullptr, true, false) != TNY_PARSE_OK)
        throw std::bad_alloc();
}
void whole_response(decoder_state &s, batch &b, oa_callset *calls, yyjson_val *root, bool hosted) {
    size_t i, n;
    yyjson_val *item;
    yyjson_arr_foreach(jget(root, "output"), i, n, item) {
        if (is(item, "type", "message")) {
            size_t pi, pn;
            yyjson_val *part;
            yyjson_arr_foreach(jget(item, "content"), pi, pn, part) {
                if (!is(part, "type", "output_text")) continue;
                size_t len = 0;
                const char *text = jget_strn(part, "text", &len);
                b.text(OA_DECODE_TEXT, text, len);
            }
        } else if (is(item, "type", "function_call")) call_item(calls, calls->n, item, true);
        else if (is(item, "type", "reasoning")) s.reasoning_item(item);
    }
    hosted_output(s, b, root, hosted);
    usage(b, jget(root, "usage"), false);
    if (is(root, "status", "failed")) {
        yyjson_val *err = jget(root, "error");
        b.value(OA_DECODE_ERROR, err ? err : root);
    } else if (is(root, "status", "incomplete")) incomplete(b, root);
    b.add(OA_DECODE_DONE);
}
void responses_decode(decoder_state &s, batch &b, oa_callset *calls, yyjson_val *root,
                      bool hosted) {
    const char *type = jget_str(root, "type");
    if (!type) {
        yyjson_val *err = jget(root, "error");
        if (error_value(err)) failure(b, err);
        else if (yyjson_is_arr(jget(root, "output"))) whole_response(s, b, calls, root, hosted);
        return;
    }
    if (!std::strcmp(type, "response.output_text.delta")) {
        size_t len = 0;
        const char *text = jget_strn(root, "delta", &len);
        b.text(OA_DECODE_TEXT, text, len);
    } else if (!std::strcmp(type, "response.reasoning_summary_text.delta") ||
               !std::strcmp(type, "response.reasoning_text.delta")) {
        size_t len = 0;
        const char *text = jget_strn(root, "delta", &len);
        if (text && len && hosted) s.reasoning.append(text, len);
        b.text(OA_DECODE_THINKING, text, len);
    } else if (!std::strcmp(type, "response.output_item.added") ||
               !std::strcmp(type, "response.output_item.done")) {
        yyjson_val *item = jget(root, "item");
        hosted_item(s, b, item, hosted);
        if (is(item, "type", "reasoning")) s.reasoning_item(item);
        if (is(item, "type", "function_call")) call_item(calls, output_index(root, calls->n), item);
    } else if (!std::strcmp(type, "response.function_call_arguments.delta")) {
        if (oa_calls_item(calls, output_index(root, -1), nullptr, nullptr, jget_str(root, "delta"),
                          false, true) != TNY_PARSE_OK)
            throw std::bad_alloc();
    } else if (!std::strcmp(type, "response.completed")) {
        yyjson_val *response = jget(root, "response");
        hosted_output(s, b, response, hosted);
        usage(b, jget(response, "usage"), false);
        b.add(OA_DECODE_DONE);
    } else if (!std::strcmp(type, "response.incomplete")) {
        yyjson_val *response = jget(root, "response");
        usage(b, jget(response, "usage"), false);
        incomplete(b, response);
        b.add(OA_DECODE_DONE);
    } else if (!std::strcmp(type, "response.failed") || !std::strcmp(type, "error")) {
        yyjson_val *response = jget(root, "response");
        usage(b, jget(response, "usage"), false);
        yyjson_val *err = jget(response, "error");
        if (!err) err = jget(root, "error");
        failure(b, err ? err : root);
    }
}
} // namespace

int oa_reasoning_details_merge(yyjson_mut_doc *rdoc, yyjson_mut_val *arr, yyjson_val *details) {
    try {
        merge_details(rdoc, arr, details);
        return TNY_PARSE_OK;
    } catch (const std::bad_alloc &) { return TNY_PARSE_OOM; } catch (const std::length_error &) {
        return TNY_PARSE_OOM;
    }
}
void oa_decoder_reset(oa_decoder *decoder) {
    tny::owned<decoder_state> owner(static_cast<decoder_state *>(decoder->owner));
    *decoder = {};
}
int oa_decoder_feed(oa_decoder *decoder, oa_callset *calls, bool chat, bool hosted,
                    const char *data, size_t len, oa_decoded_cb cb, void *ud) {
    if (decoder->status) return decoder->status;
    tny::document doc;
    batch events;
    try {
        if (chat && ((len == 6 && !std::memcmp(data, "[DONE]", 6)) ||
                     (len == 4 && !std::memcmp(data, "DONE", 4))))
            events.add(OA_DECODE_DONE);
        else {
            doc = tny::parse(data, len);
            if (!doc) return TNY_PARSE_INVALID;
            if (!decoder->owner) decoder->owner = tny::make_owned<decoder_state>().release();
            auto &s = *static_cast<decoder_state *>(decoder->owner);
            if (chat) chat_decode(s, events, calls, yyjson_doc_get_root(doc.get()));
            else responses_decode(s, events, calls, yyjson_doc_get_root(doc.get()), hosted);
        }
    } catch (const std::bad_alloc &) {
        decoder->status = TNY_PARSE_OOM;
    } catch (const std::length_error &) { decoder->status = TNY_PARSE_OOM; }
    if (decoder->status) return decoder->status;
    /* C callbacks are outside all throwing decode operations. */
    int rc = events.dispatch(cb, ud);
    if (rc) decoder->status = rc;
    return rc;
}
int oa_decoder_extras(oa_decoder *decoder, char **out) {
    *out = nullptr;
    if (decoder->status || !decoder->owner) return decoder->status;
    try {
        auto &s = *static_cast<decoder_state *>(decoder->owner);
        if (!yyjson_mut_arr_size(s.details) && !yyjson_mut_arr_size(s.items) &&
            !yyjson_mut_arr_size(s.hosted_items) && s.reasoning.empty())
            return TNY_PARSE_OK;
        auto doc = tny::make_document();
        auto *root = required(yyjson_mut_obj(doc.get()));
        yyjson_mut_doc_set_root(doc.get(), root);
        if (yyjson_mut_arr_size(s.details))
            put(doc.get(), root, "reasoning_details",
                yyjson_mut_val_mut_copy(doc.get(), s.details));
        if (yyjson_mut_arr_size(s.items))
            put(doc.get(), root, "reasoning_items", yyjson_mut_val_mut_copy(doc.get(), s.items));
        if (yyjson_mut_arr_size(s.hosted_items))
            put(doc.get(), root, "responses_items",
                yyjson_mut_val_mut_copy(doc.get(), s.hosted_items));
        if (!s.reasoning.empty())
            put(doc.get(), root, "reasoning_content",
                yyjson_mut_strncpy(doc.get(), s.reasoning.data(), s.reasoning.size()));
        *out = required(jwrite(doc.get()));
        return TNY_PARSE_OK;
    } catch (const std::bad_alloc &) {
        decoder->status = TNY_PARSE_OOM;
    } catch (const std::length_error &) { decoder->status = TNY_PARSE_OOM; }
    return decoder->status;
}
