/* SSE accumulation: no universal payload cap; each consumer owns its limits. */
#include "util/ownership.hpp"
#include "net/net.h"
#include <cstring>

namespace {
struct sse_state {
    tny::string line, data;
    sse_state() = default;
    sse_state(const sse_state &) = delete;
    sse_state &operator=(const sse_state &) = delete;
    sse_state(sse_state &&) = default;
    sse_state &operator=(sse_state &&) = default;

    void dispatch(sse_event_cb cb, void *ud) {
        if (!data.empty()) cb(data.data(), data.size(), ud);
        data.clear();
    }
    void handle(sse_event_cb cb, void *ud) {
        size_t len = line.size();
        if (len && line[len - 1] == '\r') --len;
        if (!len) dispatch(cb, ud);
        else if (len >= 5 && std::memcmp(line.data(), "data:", 5) == 0) {
            size_t start = len > 5 && line[5] == ' ' ? 6 : 5;
            if (!data.empty()) data.push_back('\n');
            data.append(line.data() + start, len - start);
        }
        line.clear();
    }
};
} // namespace

void sse_parser_init(sse_parser *p) { *p = {}; }
void sse_parser_free(sse_parser *p) {
    tny::owned<sse_state> owner(static_cast<sse_state *>(p->owner));
    *p = {};
}
int sse_feed(sse_parser *p, const char *bytes, size_t n, sse_event_cb cb, void *ud) {
    if (p->status || !n) return p->status;
    try {
        if (!p->owner) p->owner = tny::make_owned<sse_state>().release();
        auto &s = *static_cast<sse_state *>(p->owner);
        while (n) {
            const char *nl = static_cast<const char *>(std::memchr(bytes, '\n', n));
            size_t take = nl ? static_cast<size_t>(nl - bytes) : n;
            s.line.append(bytes, take);
            bytes += take;
            n -= take;
            if (nl) {
                s.handle(cb, ud);
                ++bytes;
                --n;
            }
        }
        return TNY_PARSE_OK;
    } catch (const std::bad_alloc &) {
        p->status = TNY_PARSE_OOM;
    } catch (const std::length_error &) { p->status = TNY_PARSE_OOM; }
    return p->status;
}
int sse_flush(sse_parser *p, sse_event_cb cb, void *ud) {
    if (p->status || !p->owner) return p->status;
    try {
        auto &s = *static_cast<sse_state *>(p->owner);
        if (!s.line.empty()) s.handle(cb, ud);
        s.dispatch(cb, ud);
        return TNY_PARSE_OK;
    } catch (const std::bad_alloc &) {
        p->status = TNY_PARSE_OOM;
    } catch (const std::length_error &) { p->status = TNY_PARSE_OOM; }
    return p->status;
}
