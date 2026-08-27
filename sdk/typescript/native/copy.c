#define _POSIX_C_SOURCE 200809L
#include "addon_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *sdk_copy_n(const char *value, size_t len) {
    char *copy = (char *)malloc(len + 1u);
    if (!copy) return NULL;
    if (len) memcpy(copy, value, len);
    copy[len] = '\0';
    return copy;
}
char *sdk_copy_cstr(const char *value) {
    return value ? sdk_copy_n(value, strlen(value)) : NULL;
}

char *sdk_copy_bytes(tny_bytes value) {
    if (!value.ptr || value.len == 0u) return sdk_copy_n("", 0u);
    if (value.len > SIZE_MAX - 1u) return NULL;
    return sdk_copy_n(value.ptr, (size_t)value.len);
}

sdk_owned_bytes sdk_copy_owned(tny_bytes value) {
    sdk_owned_bytes copy = {0};
    copy.ptr = sdk_copy_bytes(value);
    if (copy.ptr) copy.len = value.len;
    return copy;
}

static void free_event_copy(event_copy *event) {
    if (!event) return;
    free(event->provider.ptr);
    free(event->session_id.ptr);
    free(event->turn_id.ptr);
    free(event->text.ptr);
    free(event->message_id.ptr);
    free(event->tool_name.ptr);
    free(event->tool_id.ptr);
    free(event->tool_detail.ptr);
    free(event->permission_id.ptr);
    free(event->permission_summary.ptr);
    free(event->message_type.ptr);
    free(event);
}

static void free_capabilities(capability_copy *capabilities) {
    free(capabilities->library_version.ptr);
    free(capabilities->platform_family.ptr);
    free(capabilities->architecture.ptr);
    free(capabilities->transport.ptr);
    free(capabilities->tls_implementation.ptr);
    free(capabilities->linkage.ptr);
    memset(capabilities, 0, sizeof(*capabilities));
}

static void free_create_options(create_options *options) {
    free(options->workspace.ptr);
    free(options->state_dir.ptr);
    free(options->provider.ptr);
    free(options->model.ptr);
    free(options->base_url.ptr);
    sdk_wipe_owned_bytes(&options->api_key);
    free(options->wire_api.ptr);
    memset(options, 0, sizeof(*options));
}

void sdk_wipe_owned_bytes(sdk_owned_bytes *value) {
    volatile unsigned char *cursor;
    uint64_t index;
    if (!value || !value->ptr) return;
    cursor = (volatile unsigned char *)value->ptr;
    for (index = 0u; index < value->len; index++) cursor[index] = 0u;
    free(value->ptr);
    value->ptr = NULL;
    value->len = 0u;
}

void sdk_free_command(command *cmd) {
    if (!cmd) return;
    free(cmd->text.ptr);
    free_create_options(&cmd->create);
    free(cmd->error_message);
    free(cmd->string_result);
    free_event_copy(cmd->event);
    free_capabilities(&cmd->capabilities);
    free(cmd);
}

tny_bytes sdk_view_of(sdk_owned_bytes value) {
    tny_bytes bytes;
    bytes.ptr = value.ptr;
    bytes.len = value.len;
    return bytes;
}

char *sdk_take_error(int32_t status, tny_error *error) {
    const char *category = "internal failure";
    if (error) tny_error_free(error);
    switch (status) {
    case TNY_STATUS_INVALID_ARGUMENT: category = "invalid argument"; break;
    case TNY_STATUS_BAD_STATE: category = "bad state"; break;
    case TNY_STATUS_BUSY: category = "busy"; break;
    case TNY_STATUS_OOM: category = "out of memory"; break;
    case TNY_STATUS_CONFIG: category = "configuration failure"; break;
    case TNY_STATUS_AUTH: category = "authentication failure"; break;
    case TNY_STATUS_IO: category = "I/O failure"; break;
    case TNY_STATUS_TIMEOUT_ERROR: category = "timeout"; break;
    case TNY_STATUS_UNSUPPORTED: category = "unsupported"; break;
    case TNY_STATUS_PROTOCOL: category = "protocol failure"; break;
    case TNY_STATUS_BACKPRESSURE: category = "backpressure"; break;
    case TNY_STATUS_CANCELLED: category = "cancelled"; break;
    default: break;
    }
    return sdk_copy_cstr(category);
}

int sdk_snapshot_event(tny_event *source, event_copy **out) {
    tny_event_view_v0 view;
    event_copy *event;
    int32_t status;
    status = tny_event_view_init(&view, sizeof view);
    if (status != TNY_STATUS_OK) return status;
    status = tny_event_read(source, &view, sizeof view);
    if (status != TNY_STATUS_OK) return status;
    event = (event_copy *)calloc(1u, sizeof(*event));
    if (!event) return TNY_STATUS_OOM;
    event->kind = view.kind;
    event->schema_version = view.schema_version;
    event->tool_ok = view.tool_ok;
    event->permission_options = view.permission_options;
    event->stop_reason = view.stop_reason;
    event->error_code = view.error_code;
    event->has_cost = view.has_cost;
    event->sequence = view.sequence;
    event->timestamp_ms = view.timestamp_ms;
    event->input_tokens = view.input_tokens;
    event->output_tokens = view.output_tokens;
    event->context_used = view.context_used;
    event->context_size = view.context_size;
    event->cost = view.cost;
#define COPY_VIEW_FIELD(name) do { event->name = sdk_copy_owned(view.name); if (!event->name.ptr) goto oom; } while (0)
    COPY_VIEW_FIELD(provider);
    COPY_VIEW_FIELD(session_id);
    COPY_VIEW_FIELD(turn_id);
    COPY_VIEW_FIELD(text);
    COPY_VIEW_FIELD(message_id);
    COPY_VIEW_FIELD(tool_name);
    COPY_VIEW_FIELD(tool_id);
    COPY_VIEW_FIELD(tool_detail);
    COPY_VIEW_FIELD(permission_id);
    COPY_VIEW_FIELD(permission_summary);
    COPY_VIEW_FIELD(message_type);
#undef COPY_VIEW_FIELD
    *out = event;
    return TNY_STATUS_OK;
oom:
    free_event_copy(event);
    return TNY_STATUS_OOM;
}
int sdk_snapshot_capabilities(tny_runtime *runtime, capability_copy *copy) {
    tny_capabilities_v0 view;
    int32_t status;
    status = tny_capabilities_init(&view, sizeof view);
    if (status != TNY_STATUS_OK) return status;
    status = tny_runtime_get_capabilities(runtime, &view, sizeof view);
    if (status != TNY_STATUS_OK) return status;
    copy->schema_version = view.schema_version;
    copy->abi_version = view.abi_version;
    copy->provider_selected = view.provider_selected;
    copy->provider_initialized = view.provider_initialized;
    copy->endpoint_reachability = view.endpoint_reachability;
    copy->threading_model = view.threading_model;
    copy->cancel_model = view.cancel_model;
    copy->provider_available_mask = view.provider_available_mask;
    copy->feature_available_mask = view.feature_available_mask;
    copy->feature_enabled_mask = view.feature_enabled_mask;
    copy->event_queue_max = view.event_queue_max;
    copy->event_reserved = view.event_reserved;
    copy->event_payload_bytes_max = view.event_payload_bytes_max;
    copy->event_reserved_bytes = view.event_reserved_bytes;
#define COPY_CAPABILITY_FIELD(name) do { copy->name = sdk_copy_owned(view.name); if (!copy->name.ptr) goto oom; } while (0)
    COPY_CAPABILITY_FIELD(library_version);
    COPY_CAPABILITY_FIELD(platform_family);
    COPY_CAPABILITY_FIELD(architecture);
    COPY_CAPABILITY_FIELD(transport);
    COPY_CAPABILITY_FIELD(tls_implementation);
    COPY_CAPABILITY_FIELD(linkage);
#undef COPY_CAPABILITY_FIELD
    return TNY_STATUS_OK;
oom:
    free_capabilities(copy);
    return TNY_STATUS_OOM;
}
