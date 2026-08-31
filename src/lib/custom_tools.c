#include "lib/custom_tools.h"

#include "json/json.h"
#include "util/tny_wake.h"
#include "util/util.h"

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct tny_tool_registration {
    custom_tool_registry *registry;
    void *runtime;
    void *user_data;
    char *name;
    char *description;
    char *schema;
    uint32_t sensitivity;
    uint64_t max_argument_bytes;
    uint64_t max_result_bytes;
    tny_tool_invoke_fn invoke;
    bool active;
    struct tny_tool_registration *next;
};

struct tny_tool_call {
    pthread_mutex_t mutex;
    atomic_uint references;
    custom_tool_registry *registry;
    tny_tool_registration *registration;
    uint64_t generation;
    uint64_t epoch;
    bool active;
    bool completed;
    bool is_error;
    char *result;
    struct tny_tool_call *next;
};

static void call_release(tny_tool_call *call) {
    if (!call) return;
    if (atomic_fetch_sub_explicit(&call->references, 1, memory_order_acq_rel) == 1) {
        pthread_mutex_destroy(&call->mutex);
        free(call->result);
        free(call);
    }
}

static void call_destroy_unpublished(tny_tool_call *call) {
    if (!call) return;
    pthread_mutex_destroy(&call->mutex);
    free(call->result);
    free(call);
}

void tny_tool_call_release(tny_tool_call *call) { call_release(call); }

struct custom_tool_registry {
    pthread_mutex_t mutex;
    tny_wake wake;
    tny_tool_registration *registrations;
    tny_tool_call *calls;
    uint64_t next_generation;
    uint64_t epoch;
    bool in_callback;
    bool closing;
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

static bool bytes_text(tny_bytes value, uint64_t limit, bool required, char **out) {
    *out = NULL;
    if (!value.ptr || !value.len) return !required;
    if (value.len > limit || value.len > SIZE_MAX - 1 ||
        !utf8_valid((const unsigned char *)value.ptr, value.len))
        return false;
    char *copy = malloc((size_t)value.len + 1);
    if (!copy) return false;
    memcpy(copy, value.ptr, (size_t)value.len);
    copy[value.len] = 0;
    *out = copy;
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
    char *copy = malloc((size_t)result->data.len + 1);
    if (!copy) return TNY_STATUS_OOM;
    if (result->data.len) memcpy(copy, result->data.ptr, (size_t)result->data.len);
    copy[result->data.len] = 0;
    *out = copy;
    *is_error = result->is_error != 0;
    return TNY_STATUS_OK;
}

custom_tool_registry *custom_tools_new(void) {
    custom_tool_registry *registry = calloc(1, sizeof *registry);
    if (!registry) return NULL;
    if (pthread_mutex_init(&registry->mutex, NULL) != 0) {
        free(registry);
        return NULL;
    }
    if (tny_wake_init(&registry->wake) != 0) {
        pthread_mutex_destroy(&registry->mutex);
        free(registry);
        return NULL;
    }
    registry->next_generation = 1;
    registry->epoch = 1;
    return registry;
}

void custom_tools_free(custom_tool_registry *registry) {
    if (!registry) return;
    pthread_mutex_lock(&registry->mutex);
    registry->closing = true;
    tny_tool_call *call = registry->calls;
    registry->calls = NULL;
    pthread_mutex_unlock(&registry->mutex);
    for (tny_tool_call *item = call; item; item = item->next) {
        pthread_mutex_lock(&item->mutex);
        item->active = false;
        item->registry = NULL;
        pthread_mutex_unlock(&item->mutex);
    }
    tny_wake_close(&registry->wake);
    while (call) {
        tny_tool_call *next = call->next;
        call_release(call); /* registry reference; host may retain async ref */
        call = next;
    }
    tny_tool_registration *registration = registry->registrations;
    while (registration) {
        tny_tool_registration *next = registration->next;
        free(registration->name);
        free(registration->description);
        free(registration->schema);
        free(registration);
        registration = next;
    }
    pthread_mutex_destroy(&registry->mutex);
    free(registry);
}

bool custom_tools_in_callback(const custom_tool_registry *registry) {
    return registry && registry->in_callback;
}

size_t custom_tools_active_count(custom_tool_registry *registry) {
    size_t count = 0;
    if (!registry) return 0;
    for (tny_tool_registration *item = registry->registrations; item; item = item->next)
        if (item->active) count++;
    return count;
}

int32_t custom_tools_register(custom_tool_registry *registry, void *runtime,
                              const tny_tool_spec_v1 *spec, tny_tool_registration **out) {
    if (!registry || !spec || !out) return TNY_STATUS_INVALID_ARGUMENT;
    *out = NULL;
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
    tny_tool_registration *item = calloc(1, sizeof *item);
    if (!item) return TNY_STATUS_OOM;
    if (!bytes_text(spec->name, TNY_CUSTOM_TOOL_NAME_MAX, true, &item->name) ||
        !valid_name(item->name) ||
        !bytes_text(spec->description, TNY_CUSTOM_TOOL_DESCRIPTION_MAX, true, &item->description) ||
        !bytes_text(spec->input_schema_json, TNY_CUSTOM_TOOL_SCHEMA_MAX, true, &item->schema)) {
        free(item->name);
        free(item->description);
        free(item->schema);
        free(item);
        return TNY_STATUS_INVALID_ARGUMENT;
    }
    yyjson_doc *schema = jparse(item->schema, strlen(item->schema));
    yyjson_val *root = schema ? yyjson_doc_get_root(schema) : NULL;
    if (!schema_supported(root)) {
        yyjson_doc_free(schema);
        free(item->name);
        free(item->description);
        free(item->schema);
        free(item);
        return TNY_STATUS_INVALID_ARGUMENT;
    }
    yyjson_doc_free(schema);
    pthread_mutex_lock(&registry->mutex);
    size_t count = 0;
    for (tny_tool_registration *existing = registry->registrations; existing;
         existing = existing->next) {
        if (existing->active && strcmp(existing->name, item->name) == 0) {
            pthread_mutex_unlock(&registry->mutex);
            free(item->name);
            free(item->description);
            free(item->schema);
            free(item);
            return TNY_STATUS_BAD_STATE;
        }
        count++;
    }
    if (registry->closing || count >= TNY_CUSTOM_TOOL_MAX_COUNT) {
        pthread_mutex_unlock(&registry->mutex);
        free(item->name);
        free(item->description);
        free(item->schema);
        free(item);
        return count >= TNY_CUSTOM_TOOL_MAX_COUNT ? TNY_STATUS_BACKPRESSURE : TNY_STATUS_BAD_STATE;
    }
    item->registry = registry;
    item->runtime = runtime;
    item->user_data = spec->user_data;
    item->sensitivity = spec->sensitivity;
    item->max_argument_bytes = argument_limit;
    item->max_result_bytes = result_limit;
    item->invoke = spec->invoke;
    item->active = true;
    item->next = registry->registrations;
    registry->registrations = item;
    pthread_mutex_unlock(&registry->mutex);
    *out = item;
    return TNY_STATUS_OK;
}

int32_t custom_tools_unregister(tny_tool_registration *registration) {
    if (!registration || !registration->registry) return TNY_STATUS_INVALID_ARGUMENT;
    custom_tool_registry *registry = registration->registry;
    pthread_mutex_lock(&registry->mutex);
    if (!registration->active) {
        pthread_mutex_unlock(&registry->mutex);
        return TNY_STATUS_BAD_STATE;
    }
    registration->active = false;
    tny_tool_call *detached = NULL;
    tny_tool_call **link = &registry->calls;
    while (*link) {
        tny_tool_call *call = *link;
        if (call->registration != registration) {
            link = &call->next;
            continue;
        }
        *link = call->next;
        call->next = detached;
        detached = call;
    }
    pthread_mutex_unlock(&registry->mutex);
    while (detached) {
        tny_tool_call *call = detached;
        detached = detached->next;
        pthread_mutex_lock(&call->mutex);
        call->active = false;
        call->registry = NULL;
        pthread_mutex_unlock(&call->mutex);
        call_release(call);
    }
    tny_wake_signal(&registry->wake);
    return TNY_STATUS_OK;
}

void *custom_tool_registration_runtime(tny_tool_registration *registration) {
    return registration ? registration->runtime : NULL;
}

tny_tool_registration *custom_tools_find(custom_tool_registry *registry, const char *name) {
    if (!registry || !name) return NULL;
    for (tny_tool_registration *item = registry->registrations; item; item = item->next)
        if (item->active && strcmp(item->name, name) == 0) return item;
    return NULL;
}

const char *custom_tool_name(const tny_tool_registration *item) { return item ? item->name : NULL; }
const char *custom_tool_description(const tny_tool_registration *item) {
    return item ? item->description : NULL;
}
const char *custom_tool_schema(const tny_tool_registration *item) {
    return item ? item->schema : NULL;
}
bool custom_tool_sensitive(const tny_tool_registration *item) {
    return item && item->sensitivity == TNY_TOOL_SENSITIVITY_SENSITIVE;
}
uint64_t custom_tool_argument_limit(const tny_tool_registration *item) {
    return item ? item->max_argument_bytes : 0;
}

bool custom_tools_visit(custom_tool_registry *registry, custom_tools_visit_fn visit, void *ud) {
    if (!registry || !visit) return false;
    bool completed = true;
    pthread_mutex_lock(&registry->mutex);
    for (tny_tool_registration *item = registry->registrations; item; item = item->next) {
        if (item->active && !visit(item, ud)) {
            completed = false;
            break;
        }
    }
    pthread_mutex_unlock(&registry->mutex);
    return completed;
}

char *custom_tools_schema_json(custom_tool_registry *registry) {
    buf_t out;
    buf_init(&out);
    buf_appends(&out, "[");
    bool first = true;
    if (registry)
        for (tny_tool_registration *item = registry->registrations; item; item = item->next) {
            if (!item->active) continue;
            if (!first) buf_appends(&out, ",");
            first = false;
            buf_appends(&out, "{\"type\":\"function\",\"function\":{\"name\":");
            jescape(&out, item->name);
            buf_appends(&out, ",\"description\":");
            jescape(&out, item->description);
            buf_appends(&out, ",\"parameters\":");
            buf_appends(&out, item->schema);
            buf_appends(&out, "}}");
        }
    buf_appends(&out, "]");
    return buf_detach(&out);
}

int32_t custom_tool_invoke(tny_tool_registration *registration, const char *arguments_json,
                           tny_tool_call **out_call, char **out_result, bool *out_is_error) {
    if (!registration || !out_call || !out_result || !out_is_error)
        return TNY_STATUS_INVALID_ARGUMENT;
    *out_call = NULL;
    *out_result = NULL;
    *out_is_error = false;
    size_t arguments_size = strlen(arguments_json);
    if (arguments_size > registration->max_argument_bytes) return TNY_STATUS_BACKPRESSURE;
    custom_tool_registry *registry = registration->registry;
    tny_tool_call *call = calloc(1, sizeof *call);
    if (!call) return TNY_STATUS_OOM;
    if (pthread_mutex_init(&call->mutex, NULL) != 0) {
        free(call);
        return TNY_STATUS_OOM;
    }
    atomic_init(&call->references, 2); /* registry + async host handle */
    pthread_mutex_lock(&registry->mutex);
    if (!registration->active || registry->closing || registry->next_generation == 0) {
        int32_t failure =
            registry->next_generation == 0 ? TNY_STATUS_BACKPRESSURE : TNY_STATUS_BAD_STATE;
        pthread_mutex_unlock(&registry->mutex);
        call_destroy_unpublished(call);
        return failure;
    }
    call->registry = registry;
    call->registration = registration;
    call->generation = registry->next_generation++;
    call->epoch = registry->epoch;
    call->active = true;
    call->next = registry->calls;
    registry->calls = call;
    pthread_mutex_unlock(&registry->mutex);

    tny_tool_result_v1 result;
    memset(&result, 0, sizeof result);
    result.abi_version = TNY_TOOL_RESULT_ABI_VERSION;
    result.struct_size = sizeof result;
    registry->in_callback = true;
    int32_t status = registration->invoke(registration->user_data, call, call->generation,
                                          (tny_bytes){arguments_json, arguments_size}, &result);
    registry->in_callback = false;
    if (status == TNY_TOOL_INVOKE_ASYNC) {
        *out_call = call;
        return TNY_TOOL_INVOKE_ASYNC;
    }
    if (status != TNY_TOOL_INVOKE_SYNC) {
        pthread_mutex_lock(&registry->mutex);
        registry->calls = call->next;
        pthread_mutex_lock(&call->mutex);
        call->active = false;
        call->registry = NULL;
        pthread_mutex_unlock(&call->mutex);
        pthread_mutex_unlock(&registry->mutex);
        call_destroy_unpublished(call);
        return status <= TNY_STATUS_INVALID_ARGUMENT && status >= TNY_STATUS_INTERNAL
                   ? status
                   : TNY_STATUS_INTERNAL;
    }
    status = result_copy(registration, &result, out_result, out_is_error);
    pthread_mutex_lock(&registry->mutex);
    registry->calls = call->next;
    pthread_mutex_lock(&call->mutex);
    call->active = false;
    call->registry = NULL;
    pthread_mutex_unlock(&call->mutex);
    pthread_mutex_unlock(&registry->mutex);
    call_destroy_unpublished(call);
    return status;
}

int32_t custom_tool_complete(tny_tool_call *call, uint64_t generation,
                             const tny_tool_result_v1 *result) {
    if (!call) return TNY_STATUS_INVALID_ARGUMENT;
    pthread_mutex_lock(&call->mutex);
    custom_tool_registry *registry = call->registry;
    if (!call->active || call->completed || call->generation != generation || !registry) {
        pthread_mutex_unlock(&call->mutex);
        return TNY_STATUS_BAD_STATE;
    }
    pthread_mutex_lock(&registry->mutex);
    if (registry->closing || !call->registration->active || call->epoch != registry->epoch) {
        pthread_mutex_unlock(&registry->mutex);
        pthread_mutex_unlock(&call->mutex);
        return TNY_STATUS_BAD_STATE;
    }
    char *copy = NULL;
    bool is_error = false;
    int32_t status = result_copy(call->registration, result, &copy, &is_error);
    if (status == TNY_STATUS_OK) {
        call->result = copy;
        call->is_error = is_error;
        call->completed = true;
    }
    if (status == TNY_STATUS_OK) tny_wake_signal(&registry->wake);
    pthread_mutex_unlock(&registry->mutex);
    pthread_mutex_unlock(&call->mutex);
    return status;
}

uint64_t tny_tool_call_generation(const tny_tool_call *call) { return call ? call->generation : 0; }

int custom_tool_take(tny_tool_call *call, char **out_result, bool *out_is_error) {
    if (!call || !out_result || !out_is_error) return -1;
    pthread_mutex_lock(&call->mutex);
    custom_tool_registry *registry = call->registry;
    if (!registry) {
        pthread_mutex_unlock(&call->mutex);
        return -1;
    }
    pthread_mutex_lock(&registry->mutex);
    if (!call->active) {
        pthread_mutex_unlock(&registry->mutex);
        pthread_mutex_unlock(&call->mutex);
        return -1;
    }
    if (!call->completed) {
        pthread_mutex_unlock(&registry->mutex);
        pthread_mutex_unlock(&call->mutex);
        return 0;
    }
    *out_result = call->result;
    *out_is_error = call->is_error;
    call->result = NULL;
    call->active = false;
    call->registry = NULL;
    tny_tool_call **link = &registry->calls;
    while (*link && *link != call) link = &(*link)->next;
    if (*link == call) *link = call->next;
    pthread_mutex_unlock(&registry->mutex);
    pthread_mutex_unlock(&call->mutex);
    call_release(call); /* registry reference */
    return 1;
}

void custom_tool_invalidate(tny_tool_call *call) {
    if (!call) return;
    pthread_mutex_lock(&call->mutex);
    custom_tool_registry *registry = call->registry;
    if (!registry) {
        pthread_mutex_unlock(&call->mutex);
        return;
    }
    pthread_mutex_lock(&registry->mutex);
    call->active = false;
    call->registry = NULL;
    tny_tool_call **link = &registry->calls;
    while (*link && *link != call) link = &(*link)->next;
    if (*link == call) *link = call->next;
    pthread_mutex_unlock(&registry->mutex);
    pthread_mutex_unlock(&call->mutex);
    call_release(call);
}

void custom_tools_invalidate_all(custom_tool_registry *registry) {
    if (!registry) return;
    pthread_mutex_lock(&registry->mutex);
    registry->epoch++;
    if (registry->epoch == 0) registry->epoch = 1;
    tny_tool_call *call = registry->calls;
    registry->calls = NULL;
    pthread_mutex_unlock(&registry->mutex);
    for (tny_tool_call *item = call; item; item = item->next) {
        pthread_mutex_lock(&item->mutex);
        item->registry = NULL;
        item->active = false;
        pthread_mutex_unlock(&item->mutex);
    }
    while (call) {
        tny_tool_call *next = call->next;
        call_release(call);
        call = next;
    }
    tny_wake_signal(&registry->wake);
}

int custom_tools_wake_fd(custom_tool_registry *registry) {
    return registry ? tny_wake_fd(&registry->wake) : -1;
}
void custom_tools_wake_drain(custom_tool_registry *registry) {
    if (registry) tny_wake_drain(&registry->wake);
}
