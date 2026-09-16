/* Owned streamed calls, with id-first attribution even for broken gateways. */
#include "cpp/owners.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <climits>
extern "C" {
#include "backends/openai/parsers.h"
}

namespace {
struct call {
    tny::string id, name, args;
    bool has_id = false, has_name = false, has_args = false;
    call() = default;
    call(const call &) = delete;
    call &operator=(const call &) = delete;
};
struct call_state {
    std::array<call, OA_MAX_TOOL_CALLS> calls;
};
static_assert(!std::is_copy_constructible_v<call_state>);

void set(oa_callset *cs, int slot, int index, const char *id, const char *name, const char *args,
         bool replace) {
    if (slot < 0 || slot > cs->n || slot >= OA_MAX_TOOL_CALLS) return;
    if (!cs->owner) cs->owner = tny::make_owner<call_state>().release();
    auto &c = static_cast<call_state *>(cs->owner)->calls[size_t(slot)];
    if (id && !c.has_id) {
        tny::append(c.id, id);
        c.has_id = true;
    }
    if (name && !c.has_name) {
        tny::append(c.name, name);
        c.has_name = true;
    }
    if (args) {
        if (replace) c.args.clear();
        tny::append(c.args, args);
        c.has_args = true;
    }
    cs->calls[slot] = {c.has_id ? c.id.data() : nullptr,
                       c.has_name ? c.name.data() : nullptr,
                       {c.has_args ? c.args.data() : nullptr, c.args.size(), 0, false},
                       index};
    if (slot == cs->n) cs->n++;
}
int by_id(const oa_callset *cs, const char *id) {
    for (int i = 0; i < cs->n; i++)
        if (cs->calls[i].id && std::strcmp(cs->calls[i].id, id) == 0) return i;
    return -1;
}
int by_index(const oa_callset *cs, int index) {
    for (int i = cs->n - 1; i >= 0; i--)
        if (cs->calls[i].wire_index == index) return i;
    return -1;
}
int fail(oa_callset *cs) noexcept {
    /* No stale views or partially assembled calls are usable after OOM. */
    oa_calls_reset(cs);
    return cs->status = -2;
}
} // namespace
extern "C" int oa_calls_set(oa_callset *cs, int slot, int wire_index, const char *id,
                            const char *name, const char *args, bool replace_args) {
    if (cs->status) return cs->status;
    try {
        set(cs, slot, wire_index, id, name, args, replace_args);
        return 0;
    } catch (const std::bad_alloc &) { return fail(cs); }
}
extern "C" int oa_calls_feed(oa_callset *cs, yyjson_val *tool_calls) {
    if (cs->status) return cs->status;
    try {
        size_t ai, amax;
        yyjson_val *tc;
        yyjson_arr_foreach(tool_calls, ai, amax, tc) {
            const char *id = jget_str(tc, "id");
            if (id && !*id) id = nullptr;
            bool has_index = jget(tc, "index") != nullptr;
            int64_t raw_index = jget_int(tc, "index", -1);
            if (has_index && (raw_index < 0 || raw_index > INT_MAX)) continue;
            int index = static_cast<int>(raw_index);
            int slot = -1;
            if (id) {
                slot = by_id(cs, id);
                if (slot < 0 && has_index) {
                    int candidate = by_index(cs, index);
                    if (candidate >= 0 && !cs->calls[candidate].id) slot = candidate;
                }
                if (slot < 0) slot = cs->n;
            } else if (has_index) {
                slot = by_index(cs, index);
                if (slot < 0) slot = cs->n;
            } else if (cs->n) slot = cs->n - 1;
            if (slot < 0 || slot >= OA_MAX_TOOL_CALLS) continue;
            if (slot < cs->n) index = cs->calls[slot].wire_index;
            else if (!has_index) index = cs->n;
            auto *fn = jget(tc, "function");
            set(cs, slot, index, id, jget_str(fn, "name"), jget_str(fn, "arguments"), false);
        }
        return 0;
    } catch (const std::bad_alloc &) { return fail(cs); }
}
extern "C" void oa_calls_reset(oa_callset *cs) {
    tny::owner<call_state> cleanup(static_cast<call_state *>(cs->owner));
    *cs = {};
}
extern "C" const char *oa_call_id(const oa_call *pc, int slot, char *buf, size_t buflen) {
    if (pc->id) return pc->id;
    std::snprintf(buf, buflen, "call_%d", slot);
    return buf;
}
