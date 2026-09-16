/* Private registry and async leases; public handles and callbacks retain C ABI. */
#include "cpp/owners.hpp"
#include "lib/custom_tools.h"
extern "C" {
#include "util/tny_wake.h"
#include "util/util.h"
}
#include <cctype>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <vector>

namespace {
class mutex {
    pthread_mutex_t value;

  public:
    mutex() {
        if (pthread_mutex_init(&value, nullptr) != 0) throw std::bad_alloc();
    }
    ~mutex() noexcept { pthread_mutex_destroy(&value); }
    mutex(const mutex &) = delete;
    mutex &operator=(const mutex &) = delete;
    void lock() noexcept { pthread_mutex_lock(&value); }
    void unlock() noexcept { pthread_mutex_unlock(&value); }
};
using guard = std::lock_guard<mutex>;
struct free_bytes {
    void operator()(char *p) const noexcept { std::free(p); }
};
using buffer = std::unique_ptr<char, free_bytes>;
struct registry_state;
struct call_state;
} // namespace

struct tny_tool_registration {
    registry_state *registry = nullptr; /* retained by call state, not a cycle */
    void *runtime = nullptr;
    void *user_data = nullptr;
    tny::string name, description, schema;
    uint32_t sensitivity = 0;
    uint64_t max_argument_bytes = 0, max_result_bytes = 0;
    tny_tool_invoke_fn invoke = nullptr;
    bool active = false;
};

namespace {
struct registry_state {
    mutex lock;
    std::weak_ptr<registry_state> self;
    tny_wake wake{-1, -1};
    std::vector<tny::owner<tny_tool_registration>,
                tny::allocator<tny::owner<tny_tool_registration>>>
        registrations;
    call_state *calls = nullptr; /* borrowed from pending owner, under lock */
    uint64_t next_generation = 1, epoch = 1;
    bool in_callback = false, closing = false;
    registry_state() {
        if (tny_wake_init(&wake) != 0) throw std::bad_alloc();
    }
    ~registry_state() noexcept { tny_wake_close(&wake); }
};
struct call_state {
    std::shared_ptr<registry_state> registry;
    tny_tool_registration *registration = nullptr;
    uint64_t generation = 0, epoch = 0;
    bool active = false, completed = false, is_error = false;
    buffer result;
    call_state *next = nullptr;
};
/* Must hold registry lock; list is non-owning and cannot keep unrelated calls alive. */
void detach(call_state &call) noexcept {
    auto &registry = *call.registry;
    call.active = false;
    call_state **link = &registry.calls;
    while (*link && *link != &call) link = &(*link)->next;
    if (*link) *link = call.next;
    call.next = nullptr;
}
void invalidate_all(registry_state &registry) noexcept {
    ++registry.epoch;
    if (!registry.epoch) registry.epoch = 1;
    while (registry.calls) detach(*registry.calls);
}
struct callback_guard {
    registry_state &registry;
    explicit callback_guard(registry_state &r) noexcept : registry(r) {
        registry.in_callback = true;
    }
    ~callback_guard() noexcept { registry.in_callback = false; }
};
} // namespace

/* Two unique handles share exactly the async state: the provider's pending
 * lease and the host worker's explicit-release lease. Neither is borrowed
 * from the other; early host release cannot invalidate the provider handle. */
struct tny_tool_call {
    std::shared_ptr<call_state> state;
};
struct custom_tool_pending {
    std::shared_ptr<call_state> state;
};
struct custom_tool_registry {
    std::shared_ptr<registry_state> state;
};

static const char *RESERVED[] = {
    "list_files",
    "glob_files",
    "grep_files",
    "read_file",
    "read_image",
    "write_file",
    "edit_file",
    "delete_file",
    "rename_file",
    "copy_file",
    "create_folder",
    "file_info",
    "semantic_search",
    "open_file",
    "terminal",
    "run_command",
    "web_fetch",
    "web_search",
    "memory",
    "read_tool_result",
    "skill",
    "install_skill",
    "subagent",
    "mcp_search_tools",
    "mcp_select_tool",
    "mcp_features",
    "ask_user_question",
    "vision",
    NULL,
};

static bool utf8_valid(const unsigned char *text, uint64_t size) {
    for (uint64_t i = 0; i < size;) {
        unsigned value = text[i++];
        if (value == 0) return false;
        if (value < 0x80) continue;
        unsigned need, minimum;
        if ((value & 0xE0) == 0xC0) {
            need = 1;
            minimum = 0x80;
            value &= 0x1F;
        } else if ((value & 0xF0) == 0xE0) {
            need = 2;
            minimum = 0x800;
            value &= 0x0F;
        } else if ((value & 0xF8) == 0xF0) {
            need = 3;
            minimum = 0x10000;
            value &= 0x07;
        } else return false;
        if (size - i < need) return false;
        for (unsigned j = 0; j < need; j++) {
            unsigned next = text[i++];
            if ((next & 0xC0) != 0x80) return false;
            value = (value << 6) | (next & 0x3F);
        }
        if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
            return false;
    }
    return true;
}

static bool bytes_text(tny_bytes value, uint64_t limit, tny::string &out) {
    if (!value.ptr || !value.len || value.len > limit || value.len > SIZE_MAX - 1 ||
        !utf8_valid(reinterpret_cast<const unsigned char *>(value.ptr), value.len))
        return false;
    out.assign(static_cast<const char *>(value.ptr), static_cast<size_t>(value.len));
    return true;
}

static bool valid_name(const char *name) {
    if (!name || !(isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (!(isalnum(*p) || *p == '_' || *p == '-')) return false;
    if (str_starts(name, "mcp_")) return false;
    for (int i = 0; RESERVED[i]; i++)
        if (strcmp(name, RESERVED[i]) == 0) return false;
    return true;
}

static bool supported_type(const char *type) {
    return type && (strcmp(type, "string") == 0 || strcmp(type, "integer") == 0 ||
                    strcmp(type, "number") == 0 || strcmp(type, "boolean") == 0 ||
                    strcmp(type, "object") == 0 || strcmp(type, "array") == 0);
}

static bool schema_supported(yyjson_val *root) {
    if (!root || !yyjson_is_obj(root) ||
        strcmp(jget_str(root, "type") ? jget_str(root, "type") : "", "object") != 0)
        return false;
    size_t index, maximum;
    yyjson_val *key, *value;
    yyjson_obj_foreach(root, index, maximum, key, value) {
        const char *name = yyjson_get_str(key);
        if (!name || (strcmp(name, "type") != 0 && strcmp(name, "properties") != 0 &&
                      strcmp(name, "required") != 0 && strcmp(name, "additionalProperties") != 0))
            return false;
    }
    yyjson_val *properties = jget(root, "properties");
    if (properties && !yyjson_is_obj(properties)) return false;
    if (properties) {
        yyjson_val *property_key, *property_schema;
        yyjson_obj_foreach(properties, index, maximum, property_key, property_schema) {
            if (!yyjson_is_obj(property_schema) || yyjson_obj_size(property_schema) != 1 ||
                !supported_type(jget_str(property_schema, "type")))
                return false;
        }
    }
    yyjson_val *required = jget(root, "required");
    if (required) {
        if (!yyjson_is_arr(required)) return false;
        size_t item_index, item_max;
        yyjson_val *item;
        yyjson_arr_foreach(required, item_index, item_max, item) {
            const char *name = yyjson_get_str(item);
            if (!name || !properties || !jget(properties, name)) return false;
        }
    }
    yyjson_val *additional = jget(root, "additionalProperties");
    return !additional || yyjson_is_bool(additional);
}

static int32_t result_copy(const tny_tool_registration *registration,
                           const tny_tool_result_v1 *result, char **out, bool *is_error) {
    *out = NULL;
    if (!result || result->abi_version != TNY_TOOL_RESULT_ABI_VERSION ||
        result->struct_size < offsetof(tny_tool_result_v1, reserved) || result->is_error > 1 ||
        result->data.len > registration->max_result_bytes || result->data.len > SIZE_MAX - 1 ||
        (result->data.len && !result->data.ptr) ||
        (result->data.len &&
         !utf8_valid((const unsigned char *)result->data.ptr, result->data.len)))
        return TNY_STATUS_INVALID_ARGUMENT;
    char *copy = static_cast<char *>(tny_alloc_malloc((size_t)result->data.len + 1));
    if (!copy) return TNY_STATUS_OOM;
    if (result->data.len) memcpy(copy, result->data.ptr, (size_t)result->data.len);
    copy[result->data.len] = 0;
    *out = copy;
    *is_error = result->is_error != 0;
    return TNY_STATUS_OK;
}

custom_tool_registry *custom_tools_new(void) {
    try {
        auto registry = tny::make_owner<custom_tool_registry>();
        registry->state = std::allocate_shared<registry_state>(tny::allocator<registry_state>{});
        registry->state->self = registry->state;
        return registry.release();
    } catch (const std::bad_alloc &) { return nullptr; }
}

void custom_tools_free(custom_tool_registry *raw) {
    tny::owner<custom_tool_registry> registry(raw);
    if (!registry) return;
    guard locked(registry->state->lock);
    registry->state->closing = true;
    invalidate_all(*registry->state);
    tny_wake_close(&registry->state->wake);
}

bool custom_tools_in_callback(const custom_tool_registry *registry) {
    return registry && registry->state->in_callback;
}
size_t custom_tools_active_count(custom_tool_registry *registry) {
    size_t count = 0;
    if (registry)
        for (const auto &item : registry->state->registrations)
            if (item->active) ++count;
    return count;
}

int32_t custom_tools_register(custom_tool_registry *registry, void *runtime,
                              const tny_tool_spec_v1 *spec, tny_tool_registration **out) {
    if (!registry || !spec || !out) return TNY_STATUS_INVALID_ARGUMENT;
    *out = nullptr;
    if (spec->abi_version != TNY_TOOL_SPEC_ABI_VERSION ||
        spec->struct_size < offsetof(tny_tool_spec_v1, reserved) || !spec->invoke ||
        spec->sensitivity > TNY_TOOL_SENSITIVITY_SENSITIVE)
        return TNY_STATUS_INVALID_ARGUMENT;
    uint64_t argument_limit =
        spec->max_argument_bytes ? spec->max_argument_bytes : TNY_CUSTOM_TOOL_ARGUMENTS_MAX;
    uint64_t result_limit =
        spec->max_result_bytes ? spec->max_result_bytes : TNY_CUSTOM_TOOL_RESULT_MAX;
    if (argument_limit > TNY_CUSTOM_TOOL_ARGUMENTS_MAX || result_limit > TNY_CUSTOM_TOOL_RESULT_MAX)
        return TNY_STATUS_INVALID_ARGUMENT;
    try {
        auto item = tny::make_owner<tny_tool_registration>();
        if (!bytes_text(spec->name, TNY_CUSTOM_TOOL_NAME_MAX, item->name) ||
            !valid_name(item->name.c_str()) ||
            !bytes_text(spec->description, TNY_CUSTOM_TOOL_DESCRIPTION_MAX, item->description) ||
            !bytes_text(spec->input_schema_json, TNY_CUSTOM_TOOL_SCHEMA_MAX, item->schema))
            return TNY_STATUS_INVALID_ARGUMENT;
        auto schema = tny::parse(item->schema);
        if (!schema_supported(schema ? yyjson_doc_get_root(schema.get()) : nullptr))
            return TNY_STATUS_INVALID_ARGUMENT;
        auto &state = *registry->state;
        guard locked(state.lock);
        for (const auto &existing : state.registrations)
            if (existing->active && existing->name == item->name) return TNY_STATUS_BAD_STATE;
        if (state.registrations.size() >= TNY_CUSTOM_TOOL_MAX_COUNT) return TNY_STATUS_BACKPRESSURE;
        if (state.closing) return TNY_STATUS_BAD_STATE;
        item->registry = &state;
        item->runtime = runtime;
        item->user_data = spec->user_data;
        item->sensitivity = spec->sensitivity;
        item->max_argument_bytes = argument_limit;
        item->max_result_bytes = result_limit;
        item->invoke = spec->invoke;
        item->active = true;
        auto *published = item.get();
        state.registrations.insert(state.registrations.begin(), std::move(item));
        *out = published;
        return TNY_STATUS_OK;
    } catch (const std::bad_alloc &) { return TNY_STATUS_OOM; }
}

int32_t custom_tools_unregister(tny_tool_registration *registration) {
    if (!registration || !registration->registry) return TNY_STATUS_INVALID_ARGUMENT;
    auto &registry = *registration->registry;
    guard locked(registry.lock);
    if (!registration->active) return TNY_STATUS_BAD_STATE;
    registration->active = false;
    for (auto *call = registry.calls; call;) {
        auto *next = call->next;
        if (call->registration == registration) detach(*call);
        call = next;
    }
    tny_wake_signal(&registry.wake);
    return TNY_STATUS_OK;
}
void *custom_tool_registration_runtime(tny_tool_registration *registration) {
    return registration ? registration->runtime : nullptr;
}
tny_tool_registration *custom_tools_find(custom_tool_registry *registry, const char *name) {
    if (registry && name)
        for (const auto &item : registry->state->registrations)
            if (item->active && item->name == name) return item.get();
    return nullptr;
}
const char *custom_tool_name(const tny_tool_registration *item) {
    return item ? item->name.c_str() : nullptr;
}
const char *custom_tool_description(const tny_tool_registration *item) {
    return item ? item->description.c_str() : nullptr;
}
const char *custom_tool_schema(const tny_tool_registration *item) {
    return item ? item->schema.c_str() : nullptr;
}
bool custom_tool_sensitive(const tny_tool_registration *item) {
    return item && item->sensitivity == TNY_TOOL_SENSITIVITY_SENSITIVE;
}
uint64_t custom_tool_argument_limit(const tny_tool_registration *item) {
    return item ? item->max_argument_bytes : 0;
}

bool custom_tools_visit(custom_tool_registry *registry, custom_tools_visit_fn visit, void *ud) {
    if (!registry || !visit) return false;
    guard locked(registry->state->lock);
    for (const auto &item : registry->state->registrations)
        if (item->active && !visit(item.get(), ud)) return false;
    return true;
}
char *custom_tools_schema_json(custom_tool_registry *registry) {
    buf_t out;
    buf_init(&out);
    buf_appends(&out, "[");
    bool first = true;
    if (registry)
        for (const auto &item : registry->state->registrations) {
            if (!item->active) continue;
            if (!first) buf_appends(&out, ",");
            first = false;
            buf_appends(&out, "{\"type\":\"function\",\"function\":{\"name\":");
            jescape(&out, item->name.c_str());
            buf_appends(&out, ",\"description\":");
            jescape(&out, item->description.c_str());
            buf_appends(&out, ",\"parameters\":");
            buf_appends(&out, item->schema.c_str());
            buf_appends(&out, "}}");
        }
    buf_appends(&out, "]");
    return buf_detach(&out);
}

int32_t custom_tool_invoke(tny_tool_registration *registration, const char *arguments_json,
                           custom_tool_pending **out_call, char **out_result, bool *out_is_error) {
    if (!registration || !arguments_json || !out_call || !out_result || !out_is_error)
        return TNY_STATUS_INVALID_ARGUMENT;
    *out_call = nullptr;
    *out_result = nullptr;
    *out_is_error = false;
    size_t arguments_size = strlen(arguments_json);
    if (arguments_size > registration->max_argument_bytes) return TNY_STATUS_BACKPRESSURE;
    try {
        auto pending = tny::make_owner<custom_tool_pending>();
        auto host = tny::make_owner<tny_tool_call>();
        auto call = std::allocate_shared<call_state>(tny::allocator<call_state>{});
        /* The runtime wrapper owns the registry during invocation. The state
         * obtains its shared reference through its non-owning backlink. */
        auto &registry = *registration->registry;
        call->registry = registry.self.lock();
        if (!call->registry) return TNY_STATUS_BAD_STATE;
        pending->state = call;
        host->state = call;
        {
            guard locked(registry.lock);
            if (!registration->active || registry.closing || registry.next_generation == 0)
                return registry.next_generation == 0 ? TNY_STATUS_BACKPRESSURE
                                                     : TNY_STATUS_BAD_STATE;
            call->registration = registration;
            call->generation = registry.next_generation++;
            call->epoch = registry.epoch;
            call->active = true;
            call->next = registry.calls;
            registry.calls = call.get();
        }
        tny_tool_result_v1 result{};
        result.abi_version = TNY_TOOL_RESULT_ABI_VERSION;
        result.struct_size = sizeof result;
        /* Release before callback: an immediate async worker may already free
         * its host lease before the callback returns ASYNC. */
        auto *host_handle = host.release();
        int32_t status;
        {
            callback_guard callback(registry);
            status = registration->invoke(registration->user_data, host_handle, call->generation,
                                          tny_bytes{arguments_json, arguments_size}, &result);
        }
        if (status == TNY_TOOL_INVOKE_ASYNC) {
            *out_call = pending.release();
            return status;
        }
        host.reset(host_handle); /* synchronous callback never receives a release obligation */
        {
            guard locked(registry.lock);
            detach(*call);
        }
        if (status == TNY_TOOL_INVOKE_SYNC)
            return result_copy(registration, &result, out_result, out_is_error);
        return status <= TNY_STATUS_INVALID_ARGUMENT && status >= TNY_STATUS_INTERNAL
                   ? status
                   : TNY_STATUS_INTERNAL;
    } catch (const std::bad_alloc &) { return TNY_STATUS_OOM; }
}

int32_t custom_tool_complete(tny_tool_call *handle, uint64_t generation,
                             const tny_tool_result_v1 *result) {
    if (!handle) return TNY_STATUS_INVALID_ARGUMENT;
    auto &call = *handle->state;
    auto &registry = *call.registry;
    guard locked(registry.lock);
    if (!call.active || call.completed || call.generation != generation || registry.closing ||
        !call.registration->active || call.epoch != registry.epoch)
        return TNY_STATUS_BAD_STATE;
    char *copy = nullptr;
    bool is_error = false;
    int32_t status = result_copy(call.registration, result, &copy, &is_error);
    if (status == TNY_STATUS_OK) {
        call.result.reset(copy);
        call.is_error = is_error;
        call.completed = true;
        tny_wake_signal(&registry.wake);
    }
    return status;
}
uint64_t tny_tool_call_generation(const tny_tool_call *call) {
    return call ? call->state->generation : 0;
}
void tny_tool_call_release(tny_tool_call *call) { tny::owner<tny_tool_call> host(call); }

int custom_tool_take(custom_tool_pending *pending, char **out_result, bool *out_is_error) {
    if (!pending || !out_result || !out_is_error) return -1;
    auto &call = *pending->state;
    int result;
    {
        guard locked(call.registry->lock);
        if (!call.active) result = -1;
        else if (!call.completed) return 0;
        else {
            *out_result = call.result.release();
            *out_is_error = call.is_error;
            detach(call);
            result = 1;
        }
    }
    tny::owner<custom_tool_pending> consumed(pending);
    return result;
}
void custom_tool_invalidate(custom_tool_pending *pending) {
    tny::owner<custom_tool_pending> consumed(pending);
    if (!pending) return;
    guard locked(pending->state->registry->lock);
    detach(*pending->state);
}
void custom_tools_invalidate_all(custom_tool_registry *registry) {
    if (!registry) return;
    guard locked(registry->state->lock);
    invalidate_all(*registry->state);
    tny_wake_signal(&registry->state->wake);
}
int custom_tools_wake_fd(custom_tool_registry *registry) {
    return registry ? tny_wake_fd(&registry->state->wake) : -1;
}
void custom_tools_wake_drain(custom_tool_registry *registry) {
    if (registry) tny_wake_drain(&registry->state->wake);
}
