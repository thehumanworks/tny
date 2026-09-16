/* Connect envelope accumulator. Transports and HTTP chunking remain C. */
#include "cpp/owners.hpp"
#include <algorithm>
#include <array>
extern "C" {
#include "net/net.h"
}

namespace {
constexpr uint32_t max_frame = 64u * 1024u * 1024u;
struct frame_state {
    std::array<unsigned char, 5> header{};
    size_t used = 0;
    uint32_t length = 0;
    tny::string payload;
    frame_state() = default;
    frame_state(const frame_state &) = delete;
    frame_state &operator=(const frame_state &) = delete;
};
} // namespace
extern "C" int connect_frame_encode(buf_t *out, uint8_t flags, const char *payload, size_t len) {
    if (len > UINT32_MAX) return -1;
    if (out->oom) return -2;
    const uint8_t head[5] = {flags, static_cast<uint8_t>(len >> 24),
                             static_cast<uint8_t>(len >> 16), static_cast<uint8_t>(len >> 8),
                             static_cast<uint8_t>(len)};
    buf_append(out, head, sizeof head);
    if (len) buf_append(out, payload, len);
    return out->oom ? -2 : 0;
}
extern "C" void connect_decoder_init(connect_decoder *d) { *d = {}; }
extern "C" void connect_decoder_free(connect_decoder *d) {
    tny::owner<frame_state> cleanup(static_cast<frame_state *>(d->owner));
    *d = {};
}
extern "C" int connect_decoder_feed(connect_decoder *d, const char *bytes, size_t n,
                                    connect_frame_cb cb, void *ud) {
    if (d->status) return d->status;
    if (!n) return 0;
    try {
        if (!d->owner) d->owner = tny::make_owner<frame_state>().release();
        auto &state = *static_cast<frame_state *>(d->owner);
        std::string_view rest(bytes, n);
        while (!rest.empty()) {
            while (state.used < state.header.size() && !rest.empty()) {
                state.header[state.used++] = static_cast<unsigned char>(rest.front());
                rest.remove_prefix(1);
            }
            if (state.used < state.header.size()) break;
            const auto &h = state.header;
            state.length = uint32_t(h[1]) << 24 | uint32_t(h[2]) << 16 | uint32_t(h[3]) << 8 | h[4];
            if (state.length > max_frame) return d->status = -1;
            auto count = std::min(rest.size(), size_t(state.length) - state.payload.size());
            tny::append(state.payload, rest.substr(0, count));
            rest.remove_prefix(count);
            if (state.payload.size() < state.length) break;
            if (state.length || h[0]) cb(h[0], state.payload.data(), state.payload.size(), ud);
            if (tny_alloc_scope_failed()) {
                connect_decoder_free(d);
                return d->status = -2;
            }
            state.payload.clear();
            state.used = 0;
        }
        return 0;
    } catch (const std::bad_alloc &) {
        connect_decoder_free(d);
        return d->status = -2;
    }
}

extern "C" bool connect_decoder_pending(const connect_decoder *d) {
    const auto *state = static_cast<const frame_state *>(d->owner);
    return state && state->used != 0;
}
