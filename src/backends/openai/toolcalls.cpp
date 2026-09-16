/* Id-first assembly with owned fragments and C borrowed views. */
#include "util/ownership.hpp"
#include "backends/openai/toolcalls.h"
#include "net/net.h"
#include <array>
#include <climits>
#include <cstdio>
#include <cstring>

namespace {
struct call {
    tny::string id, name, args;
    bool has_id = false, has_name = false, has_args = false;
    int index = -1;
    call() = default;
    call(const call &) = delete;
    call &operator=(const call &) = delete;
    call(call &&) = default;
    call &operator=(call &&) = default;
};
struct calls {
    std::array<call, OA_MAX_TOOL_CALLS> items;
    int n = 0;
    calls() = default;
    calls(const calls &) = delete;
    calls &operator=(const calls &) = delete;
    calls(calls &&) = default;
    calls &operator=(calls &&) = default;
    call *by_id(const char *id) {
        for (int i = 0; i < n; ++i)
            if (items[i].has_id && !std::strcmp(items[i].id.c_str(), id)) return &items[i];
        return nullptr;
    }
    call *by_index(int index) {
        for (int i = n - 1; i >= 0; --i)
            if (items[i].index == index) return &items[i];
        return nullptr;
    }
    call *add(int index) {
        if (n == OA_MAX_TOOL_CALLS) return nullptr;
        auto &c = items[n++];
        c.index = index;
        return &c;
    }
    call *select(const char *id, bool has_index, int index, bool append_only) {
        call *c = nullptr;
        if (id && *id) {
            c = by_id(id);
            if (!c && has_index) {
                auto *q = by_index(index);
                if (q && !q->has_id) c = q;
            }
            if (!c && !append_only) c = add(has_index ? index : n);
        } else if (has_index) {
            c = by_index(index);
            if (!c && !append_only) c = add(index);
        } else if (n) c = &items[n - 1];
        return c;
    }
    void update(call *c, const char *id, const char *name, const char *args, bool replace) {
        if (!c) return;
        if (id && *id && !c->has_id) {
            c->id = id;
            c->has_id = true;
        }
        if (name && !c->has_name) {
            c->name = name;
            c->has_name = true;
        }
        if (args) {
            if (replace) c->args.assign(args);
            else c->args.append(args);
            c->has_args = true;
        }
    }
};
calls &get(oa_callset *cs) {
    if (!cs->owner) cs->owner = tny::make_owned<calls>().release();
    return *static_cast<calls *>(cs->owner);
}
void view(oa_callset *cs, calls &s, const call *c) noexcept {
    cs->n = s.n;
    if (!c) return;
    /* Fixed slots never move; only this call's strings can invalidate a view. */
    auto slot = static_cast<size_t>(c - s.items.data());
    cs->calls[slot] = {c->has_id ? c->id.c_str() : nullptr,
                       c->has_name ? c->name.c_str() : nullptr,
                       {c->has_args ? c->args.c_str() : nullptr, c->args.size()},
                       c->index};
}
void views(oa_callset *cs) noexcept {
    auto &s = *static_cast<calls *>(cs->owner);
    for (int i = 0; i < s.n; ++i) view(cs, s, &s.items[i]);
}
int failed(oa_callset *cs) noexcept {
    /* No half-assembled batch is exposed after failure. The sticky status
     * prevents a later delta from accidentally turning it into success. */
    oa_calls_reset(cs);
    return cs->status = TNY_PARSE_OOM;
}
} // namespace

int oa_calls_feed(oa_callset *cs, yyjson_val *tool_calls) {
    if (cs->status) return cs->status;
    if (!yyjson_is_arr(tool_calls) || !yyjson_arr_size(tool_calls)) return TNY_PARSE_OK;
    try {
        auto *state = static_cast<calls *>(cs->owner);
        size_t ai, amax;
        yyjson_val *tc;
        yyjson_arr_foreach(tool_calls, ai, amax, tc) {
            const char *id = jget_str(tc, "id");
            bool has_index = jget(tc, "index") != nullptr;
            /* Avoid narrowing/real-to-integer overflow on malformed indices. */
            yyjson_val *iv = jget(tc, "index");
            int64_t index = yyjson_is_int(iv) ? yyjson_get_sint(iv) : -1;
            if (yyjson_is_real(iv)) {
                double value = yyjson_get_real(iv);
                if (value >= 0 && value <= INT_MAX) index = static_cast<int64_t>(value);
            }
            if (has_index && (index < 0 || index > INT_MAX)) continue;
            if (!state) {
                if ((!id || !*id) && !has_index) continue;
                state = &get(cs);
            }
            auto *c = state->select(id, has_index, static_cast<int>(index), false);
            yyjson_val *fn = jget(tc, "function");
            state->update(c, id, jget_str(fn, "name"), jget_str(fn, "arguments"), false);
            view(cs, *state, c);
        }
        return TNY_PARSE_OK;
    } catch (const std::bad_alloc &) { return failed(cs); } catch (const std::length_error &) {
        return failed(cs);
    }
}
int oa_calls_item(oa_callset *cs, int64_t index, const char *id, const char *name, const char *args,
                  bool replace_args, bool append_only) {
    if (cs->status) return cs->status;
    if (index < 0 || index > INT_MAX || (append_only && !cs->owner)) return TNY_PARSE_OK;
    try {
        auto &s = get(cs);
        auto *c = s.select(id, true, static_cast<int>(index), append_only);
        s.update(c, id, name, args, replace_args);
        view(cs, s, c);
        return TNY_PARSE_OK;
    } catch (const std::bad_alloc &) { return failed(cs); } catch (const std::length_error &) {
        return failed(cs);
    }
}
int oa_calls_restore(oa_callset *cs, yyjson_val *records) {
    oa_calls_reset(cs);
    if (!yyjson_is_arr(records) || yyjson_arr_size(records) > OA_MAX_TOOL_CALLS)
        return TNY_PARSE_INVALID;
    if (!yyjson_arr_size(records)) return TNY_PARSE_OK;
    try {
        auto &s = get(cs);
        size_t i, n;
        yyjson_val *record;
        yyjson_arr_foreach(records, i, n, record) {
            yyjson_val *iv = jget(record, "index");
            int64_t index = yyjson_is_int(iv) ? yyjson_get_sint(iv) : -1;
            if (index < INT_MIN || index > INT_MAX) index = -1;
            auto *c = s.add(static_cast<int>(index));
            const char *args = jget_str(record, "args");
            s.update(c, jget_str(record, "id"), jget_str(record, "name"), args ? args : "{}", true);
        }
        views(cs);
        return TNY_PARSE_OK;
    } catch (const std::bad_alloc &) { return failed(cs); } catch (const std::length_error &) {
        return failed(cs);
    }
}
void oa_calls_reset(oa_callset *cs) {
    tny::owned<calls> owner(static_cast<calls *>(cs->owner));
    *cs = {};
}
const char *oa_call_id(const oa_call *pc, int slot, char *buf, size_t buflen) {
    if (pc->id) return pc->id;
    std::snprintf(buf, buflen, "call_%d", slot);
    return buf;
}
