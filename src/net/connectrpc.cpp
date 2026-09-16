#include "util/ownership.hpp"
#include "net/net.h"
#include <algorithm>
#include <cstring>

namespace {
struct frame_state {
    unsigned char header[5]{};
    size_t header_size = 0, length = 0;
    tny::string payload;
    frame_state() = default;
    frame_state(const frame_state &) = delete;
    frame_state &operator=(const frame_state &) = delete;
    frame_state(frame_state &&) = default;
    frame_state &operator=(frame_state &&) = default;
};
} // namespace

int connect_frame_encode(buf_t *out, uint8_t flags, const char *payload, size_t len) {
    if (len > UINT32_MAX || len > SIZE_MAX - 5) return TNY_PARSE_INVALID;
    /* Reserve the entire append before publishing a partial header. */
    buf_reserve(out, len + 5);
    if (buf_oom(out)) return TNY_PARSE_OOM;
    uint8_t head[5] = {flags, static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16),
                       static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)};
    buf_append(out, head, 5);
    if (len) buf_append(out, payload, len);
    return TNY_PARSE_OK;
}
void connect_decoder_init(connect_decoder *d) { *d = {}; }
void connect_decoder_free(connect_decoder *d) {
    tny::owned<frame_state> owner(static_cast<frame_state *>(d->owner));
    *d = {};
}
int connect_decoder_finish(const connect_decoder *d) {
    if (d->status) return d->status;
    auto *s = static_cast<const frame_state *>(d->owner);
    return s && s->header_size ? TNY_PARSE_INVALID : TNY_PARSE_OK;
}
int connect_decoder_feed(connect_decoder *d, const char *bytes, size_t n, connect_frame_cb cb,
                         void *ud) {
    if (d->status || !n) return d->status;
    try {
        if (!d->owner) d->owner = tny::make_owned<frame_state>().release();
        auto &s = *static_cast<frame_state *>(d->owner);
        while (n) {
            if (s.header_size < 5) {
                size_t take = std::min(n, 5 - s.header_size);
                std::memcpy(s.header + s.header_size, bytes, take);
                s.header_size += take;
                bytes += take;
                n -= take;
                if (s.header_size < 5) break;
                const auto *h = s.header;
                s.length = static_cast<uint32_t>(h[1]) << 24 | static_cast<uint32_t>(h[2]) << 16 |
                           static_cast<uint32_t>(h[3]) << 8 | h[4];
                if (s.length > CONNECT_MAX_FRAME) return d->status = TNY_PARSE_INVALID;
            }
            size_t take = std::min(n, s.length - s.payload.size());
            s.payload.append(bytes, take);
            bytes += take;
            n -= take;
            if (s.payload.size() < s.length) break;
            if (s.length || s.header[0]) cb(s.header[0], s.payload.data(), s.length, ud);
            s.payload.clear();
            s.header_size = 0;
        }
        return TNY_PARSE_OK;
    } catch (const std::bad_alloc &) {
        d->status = TNY_PARSE_OOM;
    } catch (const std::length_error &) { d->status = TNY_PARSE_OOM; }
    return d->status;
}
