/* SSE accumulation; callback views are valid only for the synchronous call. */
#include "cpp/owners.hpp"
extern "C" {
#include "net/net.h"
}

namespace {
struct sse_state {
    tny::string line;
    tny::string data;
    sse_state() = default;
    sse_state(const sse_state &) = delete;
    sse_state &operator=(const sse_state &) = delete;

    void dispatch(sse_event_cb cb, void *ud) {
        if (!data.empty()) cb(data.data(), data.size(), ud);
        data.clear();
    }
    void finish_line(sse_event_cb cb, void *ud) {
        std::string_view view(line.data(), line.size());
        if (!view.empty() && view.back() == '\r') view.remove_suffix(1);
        if (view.empty()) dispatch(cb, ud);
        else if (view.starts_with("data:")) {
            view.remove_prefix(5);
            if (view.starts_with(' ')) view.remove_prefix(1);
            if (!data.empty()) tny::append(data, "\n");
            tny::append(data, view);
        }
        line.clear();
    }
};
} // namespace

extern "C" void sse_parser_init(sse_parser *p) { *p = {}; }
extern "C" void sse_parser_free(sse_parser *p) {
    tny::owner<sse_state> cleanup(static_cast<sse_state *>(p->owner));
    *p = {};
}
extern "C" int sse_feed(sse_parser *p, const char *bytes, size_t n, sse_event_cb cb, void *ud) {
    if (p->status) return p->status;
    if (!n) return 0;
    try {
        if (!p->owner) p->owner = tny::make_owner<sse_state>().release();
        auto &state = *static_cast<sse_state *>(p->owner);
        std::string_view rest(bytes, n);
        while (!rest.empty()) {
            auto end = rest.find('\n');
            auto count = end == std::string_view::npos ? rest.size() : end;
            tny::append(state.line, rest.substr(0, count));
            rest.remove_prefix(count);
            if (end != std::string_view::npos) {
                state.finish_line(cb, ud);
                rest.remove_prefix(1);
            }
        }
        return 0;
    } catch (const std::bad_alloc &) {
        p->status = -2;
        return p->status;
    }
}
extern "C" int sse_flush(sse_parser *p, sse_event_cb cb, void *ud) {
    if (p->status) return p->status;
    if (!p->owner) return 0;
    try {
        auto &state = *static_cast<sse_state *>(p->owner);
        if (!state.line.empty()) state.finish_line(cb, ud);
        state.dispatch(cb, ud);
        return 0;
    } catch (const std::bad_alloc &) {
        p->status = -2;
        return p->status;
    }
}
