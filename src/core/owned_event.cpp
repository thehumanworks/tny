#include "core/owned_event.h"
#include "util/ownership.hpp"
#include <cstring>
#include <cstdio>
#include <vector>

namespace {
/* The record is never moved. Only its unique owning handle moves through the
 * C queue. Storage is sized once, before any borrowed pointer is published. */
struct owned_event : tny_owned_event {
    std::vector<char, tny::allocator<char>> bytes;
    size_t turn_capacity = 0;
    char *reserved_turn = nullptr;
    owned_event() : tny_owned_event{} {}
    owned_event(const owned_event &) = delete;
    owned_event &operator=(const owned_event &) = delete;
};
struct field {
    const char *source;
    size_t size;
    size_t capacity;
    const char **destination;
};
} // namespace

extern "C" tny_owned_event *tny_owned_event_copy(const tny_backend_event *event,
                                                 const char *provider, const char *session_id,
                                                 const char *turn_id, size_t turn_capacity) {
    try {
        auto owned = tny::make_owned<owned_event>();
        owned->ev = *event;
        const char *provider_view = nullptr, *session_view = nullptr, *turn_view = nullptr;
        auto text = [](const char *value, const char **destination) {
            return field{value, value ? std::strlen(value) : 0, 0, destination};
        };
        field fields[] = {
            text(provider, &provider_view),
            text(session_id, &session_view),
            {turn_id, turn_id ? std::strlen(turn_id) : 0, turn_capacity, &turn_view},
            {event->text, event->text ? event->text_len : 0, 0, &owned->ev.text},
            text(event->message_id, &owned->ev.message_id),
            text(event->tool_name, &owned->ev.tool_name),
            text(event->tool_id, &owned->ev.tool_id),
            text(event->tool_detail, &owned->ev.tool_detail),
            text(event->perm_id, &owned->ev.perm_id),
            text(event->perm_summary, &owned->ev.perm_summary),
            text(event->message_type, &owned->ev.message_type),
            text(event->cost_currency, &owned->ev.cost_currency),
        };
        size_t total = 0;
        for (auto &f : fields) {
            if (!f.source) continue;
            if (f.size == SIZE_MAX) throw std::bad_alloc();
            if (f.capacity < f.size + 1) f.capacity = f.size + 1;
            if (f.capacity > owned->bytes.max_size() - total) throw std::bad_alloc();
            total += f.capacity;
        }
        owned->bytes.resize(total);
        size_t offset = 0;
        for (const auto &f : fields) {
            *f.destination = nullptr;
            if (!f.source) continue;
            char *to = owned->bytes.data() + offset;
            std::memcpy(to, f.source, f.size);
            *f.destination = to;
            offset += f.capacity;
        }
        owned->provider = provider_view;
        owned->session_id = session_view;
        owned->turn_id = turn_view;
        owned->turn_capacity = turn_capacity;
        owned->reserved_turn = turn_capacity ? const_cast<char *>(turn_view) : nullptr;
        owned->owned_bytes = total;
        return owned.release();
    } catch (const std::bad_alloc &) { return nullptr; }
}

extern "C" void tny_owned_event_set_turn(tny_owned_event *event, const char *session_id,
                                         uint64_t sequence, int continuation) {
    auto *owned = static_cast<owned_event *>(event);
    if (owned->reserved_turn)
        std::snprintf(owned->reserved_turn, owned->turn_capacity, "%s:%llu:%d", session_id,
                      static_cast<unsigned long long>(sequence), continuation);
}

extern "C" void tny_owned_event_free(tny_owned_event *event) {
    tny::owned<owned_event> owned(static_cast<owned_event *>(event));
}
