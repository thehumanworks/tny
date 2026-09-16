/* Chat/Responses event decoding only; scheduling and event policy stay in C. */
#include "cpp/owners.hpp"
#include <climits>
#include <cstring>
extern "C" {
#include "backends/openai/parsers.h"
}
namespace {
struct sink {
    oa_callset *calls;
    oa_decoded_cb emit;
    void *ud;
    void node(oa_decoded_kind kind, yyjson_val *value = nullptr) const {
        emit(kind, value, nullptr, 0, ud);
    }
    void text(oa_decoded_kind kind, yyjson_val *obj, const char *key) const {
        size_t len = 0;
        const char *value = jget_strn(obj, key, &len);
        if (value && len) emit(kind, nullptr, value, len, ud);
    }
    void set(int slot, int index, yyjson_val *item) const {
        const char *args = jget_str(item, "arguments");
        /* Empty item.done must not erase deltas already assembled. Otherwise
         * retain the distinction between an omitted and an empty argument field. */
        if (args && !*args && slot < calls->n && calls->calls[slot].args.len) args = nullptr;
        if (oa_calls_set(calls, slot, index, jget_str(item, "call_id"), jget_str(item, "name"),
                         args, true) != 0)
            throw std::bad_alloc();
    }
};
bool is_error(yyjson_val *v) {
    return yyjson_is_obj(v) || (yyjson_is_str(v) && yyjson_get_len(v) > 0);
}
void chat_event(const sink &out, yyjson_val *root) {
    auto *error = jget(root, "error");
    if (is_error(error)) {
        out.node(OA_DECODE_ERROR, error);
        return;
    }
    out.node(OA_DECODE_USAGE_CHAT, jget(root, "usage"));
    auto *choice = yyjson_arr_get_first(jget(root, "choices"));
    if (!choice) return;
    out.text(OA_DECODE_FINISH, choice, "finish_reason");
    auto *delta = jget(choice, "delta");
    if (!delta) delta = jget(choice, "message");
    out.text(OA_DECODE_TEXT, delta, "content");
    size_t reasoning_len = 0;
    const char *reasoning = jget_strn(delta, "reasoning_content", &reasoning_len);
    if (reasoning && reasoning_len)
        out.text(OA_DECODE_REASONING_CONTENT, delta, "reasoning_content");
    else out.text(OA_DECODE_THINKING, delta, "reasoning");
    auto *details = jget(delta, "reasoning_details");
    if (yyjson_is_arr(details)) {
        out.node(OA_DECODE_DETAILS, details);
        if (!reasoning && !jget(delta, "reasoning")) {
            size_t i, n;
            yyjson_val *item;
            yyjson_arr_foreach(details, i, n, item) {
                out.text(OA_DECODE_THINKING, item, jget_str(item, "text") ? "text" : "summary");
            }
        }
    }
    if (oa_calls_feed(out.calls, jget(delta, "tool_calls")) != 0) throw std::bad_alloc();
}
int by_index(const oa_callset *calls, int64_t index) {
    for (int i = 0; i < calls->n; i++)
        if (calls->calls[i].wire_index == index) return i;
    return -1;
}
void whole_response(const sink &out, yyjson_val *response) {
    size_t i, n;
    yyjson_val *item;
    yyjson_arr_foreach(jget(response, "output"), i, n, item) {
        const char *type = jget_str(item, "type");
        if (!type) continue;
        if (std::strcmp(type, "message") == 0) {
            size_t pi, pn;
            yyjson_val *part;
            yyjson_arr_foreach(jget(item, "content"), pi, pn, part) {
                const char *pt = jget_str(part, "type");
                if (pt && std::strcmp(pt, "output_text") == 0)
                    out.text(OA_DECODE_TEXT, part, "text");
            }
        } else if (std::strcmp(type, "function_call") == 0) {
            out.set(out.calls->n, out.calls->n, item);
        } else if (std::strcmp(type, "reasoning") == 0) out.node(OA_DECODE_REASONING_ITEM, item);
    }
    out.node(OA_DECODE_HOSTED_OUTPUT, response);
    out.node(OA_DECODE_USAGE_RSP, jget(response, "usage"));
    const char *status = jget_str(response, "status");
    if (status && std::strcmp(status, "failed") == 0) {
        auto *error = jget(response, "error");
        out.node(OA_DECODE_ERROR, error ? error : response);
    } else if (status && std::strcmp(status, "incomplete") == 0)
        out.node(OA_DECODE_INCOMPLETE, response);
    out.node(OA_DECODE_DONE);
}
void response_event(const sink &out, yyjson_val *root) {
    const char *raw_type = jget_str(root, "type");
    if (!raw_type) {
        auto *error = jget(root, "error");
        if (is_error(error)) out.node(OA_DECODE_ERROR, error);
        else if (yyjson_is_arr(jget(root, "output"))) whole_response(out, root);
        return;
    }
    std::string_view type(raw_type);
    if (type == "response.output_text.delta") out.text(OA_DECODE_TEXT, root, "delta");
    else if (type == "response.reasoning_summary_text.delta" ||
             type == "response.reasoning_text.delta")
        out.text(OA_DECODE_RSP_THINKING, root, "delta");
    else if (type == "response.output_item.added" || type == "response.output_item.done") {
        auto *item = jget(root, "item");
        const char *raw_item_type = jget_str(item, "type");
        std::string_view item_type(raw_item_type ? raw_item_type : "");
        out.node(OA_DECODE_HOSTED_ITEM, item);
        if (item_type == "reasoning") out.node(OA_DECODE_REASONING_ITEM, item);
        if (item_type == "function_call") {
            auto index = jget_int(root, "output_index", out.calls->n);
            int slot = by_index(out.calls, index);
            if (slot < 0) slot = out.calls->n;
            if (index >= 0 && index <= INT_MAX) out.set(slot, static_cast<int>(index), item);
        }
    } else if (type == "response.function_call_arguments.delta") {
        auto index = jget_int(root, "output_index", -1);
        int slot = by_index(out.calls, index);
        if (slot >= 0 && oa_calls_set(out.calls, slot, out.calls->calls[slot].wire_index, nullptr,
                                      nullptr, jget_str(root, "delta"), false) != 0)
            throw std::bad_alloc();
    } else if (type == "response.completed") {
        auto *response = jget(root, "response");
        out.node(OA_DECODE_HOSTED_OUTPUT, response);
        out.node(OA_DECODE_USAGE_RSP, jget(response, "usage"));
        out.node(OA_DECODE_DONE);
    } else if (type == "response.incomplete") {
        auto *response = jget(root, "response");
        out.node(OA_DECODE_USAGE_RSP, jget(response, "usage"));
        out.node(OA_DECODE_INCOMPLETE, response);
        out.node(OA_DECODE_DONE);
    } else if (type == "response.failed" || type == "error") {
        auto *response = jget(root, "response");
        out.node(OA_DECODE_USAGE_RSP, jget(response, "usage"));
        auto *error = jget(response, "error");
        if (!error) error = jget(root, "error");
        out.node(OA_DECODE_ERROR, error ? error : root);
    }
}
} // namespace
extern "C" int oa_decode_event(bool chat, const char *bytes, size_t len, oa_callset *calls,
                               oa_decoded_cb emit, void *ud) {
    if (calls->status) return calls->status;
    try {
        sink out{calls, emit, ud};
        std::string_view view(bytes ? bytes : "", len);
        if (chat && (view == "[DONE]" || view == "DONE")) {
            out.node(OA_DECODE_DONE);
            return 0;
        }
        auto doc = tny::parse(view);
        if (!doc) return 1;
        auto *root = yyjson_doc_get_root(doc.get());
        if (chat) chat_event(out, root);
        else response_event(out, root);
        return tny_alloc_scope_failed() ? -2 : 0;
    } catch (const std::bad_alloc &) { return -2; }
}
