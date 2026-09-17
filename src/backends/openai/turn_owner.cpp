#include "backends/openai/turn_owner.h"
#include "util/ownership.hpp"
#include <cassert>

extern "C" void oa_pending_reset(oa_pending *pending) {
    assert(!pending->call.custom_call);
    std::free(pending->id);
    std::free(pending->original_args);
    std::free(pending->effective_args);
    std::free(pending->control_extension);
    std::free(pending->control_reason);
    tools_call_release_storage(&pending->call);
    *pending = {};
}
namespace {
/* No heap wrapper for a field: the C layout is the inline owned value. */
struct pending_storage : oa_pending {
    pending_storage() noexcept : oa_pending{} {}
    pending_storage(const pending_storage &) = delete;
    pending_storage &operator=(const pending_storage &) = delete;
    ~pending_storage() noexcept { oa_pending_reset(this); }
    void copy(const char *cid, const char *original, const char *effective, const char *extension,
              const char *reason) {
        auto duplicate = [](const char *value) {
            char *p = tny_alloc_strdup(value);
            if (!p) throw std::bad_alloc();
            return p;
        };
        id = duplicate(cid);
        original_args = duplicate(original ? original : "{}");
        effective_args = duplicate(effective ? effective : "{}");
        if (extension) control_extension = duplicate(extension);
        if (reason) control_reason = duplicate(reason);
    }
};
struct turn_storage : oa_turn_storage {
    turn_storage() noexcept : oa_turn_storage{} {}
    turn_storage(const turn_storage &) = delete;
    turn_storage &operator=(const turn_storage &) = delete;
    ~turn_storage() noexcept {
        oa_pending_reset(&permission);
        oa_pending_reset(&custom);
        buf_free(&text);
        buf_free(&rawbody);
        buf_free(&toolcall_log);
        std::free(steer);
    }
};
} // namespace
extern "C" oa_turn_storage *oa_turn_new(void) {
    try {
        return tny::make_owned<turn_storage>().release();
    } catch (...) { return nullptr; }
}
extern "C" void oa_turn_free(oa_turn_storage **turn) {
    tny::owned<turn_storage> released(static_cast<turn_storage *>(*turn));
    *turn = nullptr;
}
extern "C" int oa_pending_admit(oa_pending *pending, const char *id, const char *original,
                                const char *effective, const char *extension, const char *reason,
                                tools_call *call) {
    if (pending->id || pending->call.name || pending->call.custom_call || !id || !call ||
        call == &pending->call || tny_alloc_scope_failed())
        return -2;
    try {
        pending_storage candidate;
        candidate.copy(id, original, effective, extension, reason);
        /* Commit is nonthrowing. All input metadata may borrow the source. */
        candidate.call = std::exchange(*call, tools_call{});
        *pending = std::exchange(static_cast<oa_pending &>(candidate), oa_pending{});
        return 0;
    } catch (...) { return -2; }
}
extern "C" void oa_turn_take_steer(oa_turn_storage *turn, char *owned) {
    std::free(turn->steer);
    turn->steer = owned;
}
