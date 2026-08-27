#define _POSIX_C_SOURCE 200809L
#include "addon_internal.h"

#include <stdlib.h>
#include <string.h>

static _Thread_local napi_status build_status = napi_ok;

static void note_status(napi_status status) {
    if (build_status == napi_ok && status != napi_ok) build_status = status;
}

static napi_value create_record(napi_env env) {
    napi_value source, object;
    napi_status status = napi_create_string_utf8(
        env, "({__proto__:null})", NAPI_AUTO_LENGTH, &source);
    if (status == napi_ok) status = napi_run_script(env, source, &object);
    note_status(status);
    return status == napi_ok ? object : NULL;
}

static napi_status set_named(napi_env env, napi_value object, const char *name,
                             napi_value value) {
    napi_property_descriptor property = {
        name, NULL, NULL, NULL, NULL, value,
        napi_writable | napi_enumerable | napi_configurable, NULL
    };
    napi_status status = object && value
        ? napi_define_properties(env, object, 1u, &property)
        : napi_invalid_arg;
    note_status(status);
    return status;
}

static napi_value js_string(napi_env env, const char *value) {
    napi_value result;
    const char *text = value ? value : "";
    napi_status status = napi_create_string_utf8(env, text, strlen(text), &result);
    note_status(status);
    return status == napi_ok ? result : NULL;
}

static napi_value js_owned(napi_env env, sdk_owned_bytes value) {
    napi_value result;
    napi_status status = value.len > SIZE_MAX ? napi_invalid_arg :
        napi_create_string_utf8(env, value.ptr ? value.ptr : "", (size_t)value.len, &result);
    note_status(status);
    return status == napi_ok ? result : NULL;
}

static napi_value js_uint32(napi_env env, uint32_t value) {
    napi_value result;
    napi_status status = napi_create_uint32(env, value, &result);
    note_status(status);
    return status == napi_ok ? result : NULL;
}

static napi_value js_int32(napi_env env, int32_t value) {
    napi_value result;
    napi_status status = napi_create_int32(env, value, &result);
    note_status(status);
    return status == napi_ok ? result : NULL;
}

static napi_value js_bool(napi_env env, int value) {
    napi_value result;
    napi_status status = napi_get_boolean(env, value != 0, &result);
    note_status(status);
    return status == napi_ok ? result : NULL;
}

static napi_value js_big_uint(napi_env env, uint64_t value) {
    napi_value result;
    napi_status status = napi_create_bigint_uint64(env, value, &result);
    note_status(status);
    return status == napi_ok ? result : NULL;
}

static napi_value js_big_int(napi_env env, int64_t value) {
    napi_value result;
    napi_status status = napi_create_bigint_int64(env, value, &result);
    note_status(status);
    return status == napi_ok ? result : NULL;
}

static napi_value js_double(napi_env env, double value) {
    napi_value result;
    napi_status status = napi_create_double(env, value, &result);
    note_status(status);
    return status == napi_ok ? result : NULL;
}

static const char *event_type(uint32_t kind) {
    static const char *types[] = {
        "text_delta", "thinking", "tool_start", "tool_end",
        "permission_request", "plan", "usage", "turn_end", "error",
        "status", "steer_rejected", "custom_message", "user_message",
        "tool_progress"
    };
    return kind < sizeof(types) / sizeof(types[0]) ? types[kind] : NULL;
}

static const char *stop_reason(uint32_t reason) {
    static const char *reasons[] = {"done", "interrupted", "denied", "step_limit", "error"};
    return reason < sizeof(reasons) / sizeof(reasons[0]) ? reasons[reason] : "unknown";
}

static const char *stable_error_text(int32_t status) {
    switch (status) {
    case TNY_STATUS_AUTH: return "authentication failure";
    case TNY_STATUS_IO: return "I/O failure";
    case TNY_STATUS_TIMEOUT_ERROR: return "timeout";
    case TNY_STATUS_PROTOCOL: return "protocol failure";
    case TNY_STATUS_BACKPRESSURE: return "backpressure";
    case TNY_STATUS_CANCELLED: return "cancelled";
    default: return "runtime failure";
    }
}

napi_value sdk_event_to_js(napi_env env, const event_copy *event) {
    napi_value object;
    build_status = napi_ok;
    const char *type = event_type(event->kind);
    object = create_record(env);
    set_named(env, object, "schemaVersion", js_uint32(env, event->schema_version));
    set_named(env, object, "sequence", js_big_uint(env, event->sequence));
    set_named(env, object, "timestampMs", js_big_int(env, event->timestamp_ms));
    set_named(env, object, "provider", js_owned(env, event->provider));
    set_named(env, object, "sessionId", js_owned(env, event->session_id));
    set_named(env, object, "turnId", js_owned(env, event->turn_id));
    set_named(env, object, "kind", js_uint32(env, event->kind));
    if (!type) {
        napi_value payload;
        set_named(env, object, "type", js_string(env, "unknown"));
        payload = create_record(env);
        set_named(env, payload, "kind", js_uint32(env, event->kind));
        set_named(env, payload, "text", js_owned(env, event->text));
        set_named(env, payload, "messageId", js_owned(env, event->message_id));
        set_named(env, payload, "messageType", js_owned(env, event->message_type));
        set_named(env, payload, "toolName", js_owned(env, event->tool_name));
        set_named(env, payload, "toolId", js_owned(env, event->tool_id));
        set_named(env, payload, "toolDetail", js_owned(env, event->tool_detail));
        set_named(env, payload, "permissionId", js_owned(env, event->permission_id));
        set_named(env, payload, "permissionSummary", js_owned(env, event->permission_summary));
        set_named(env, payload, "permissionOptions", js_uint32(env, event->permission_options));
        set_named(env, payload, "toolOk", js_bool(env, event->tool_ok));
        set_named(env, payload, "stopReason", js_uint32(env, event->stop_reason));
        set_named(env, payload, "errorCode", js_int32(env, event->error_code));
        set_named(env, payload, "inputTokens", js_big_int(env, event->input_tokens));
        set_named(env, payload, "outputTokens", js_big_int(env, event->output_tokens));
        set_named(env, payload, "contextUsed", js_big_int(env, event->context_used));
        set_named(env, payload, "contextSize", js_big_int(env, event->context_size));
        set_named(env, payload, "hasCost", js_bool(env, event->has_cost));
        if (event->has_cost) {
            set_named(env, payload, "cost", js_double(env, event->cost));
        }
        set_named(env, object, "payload", payload);
        return build_status == napi_ok ? object : NULL;
    }
    set_named(env, object, "type", js_string(env, type));
    switch (event->kind) {
    case TNY_EVENT_TEXT_DELTA:
    case TNY_EVENT_THINKING:
    case TNY_EVENT_PLAN:
    case TNY_EVENT_STATUS:
    case TNY_EVENT_STEER_REJECTED:
    case TNY_EVENT_CUSTOM_MESSAGE:
    case TNY_EVENT_USER_MESSAGE:
        set_named(env, object, "text", js_owned(env, event->text));
        if (event->message_id.len)
            set_named(env, object, "messageId", js_owned(env, event->message_id));
        if (event->kind == TNY_EVENT_CUSTOM_MESSAGE && event->message_type.len)
            set_named(env, object, "messageType", js_owned(env, event->message_type));
        break;
    case TNY_EVENT_TOOL_START:
    case TNY_EVENT_TOOL_END:
    case TNY_EVENT_TOOL_PROGRESS:
        set_named(env, object, "toolName", js_owned(env, event->tool_name));
        set_named(env, object, "toolId", js_owned(env, event->tool_id));
        set_named(env, object, "toolDetail", js_owned(env, event->tool_detail));
        if (event->kind == TNY_EVENT_TOOL_END)
            set_named(env, object, "toolOk", js_bool(env, event->tool_ok));
        break;
    case TNY_EVENT_PERMISSION:
        set_named(env, object, "permissionId", js_owned(env, event->permission_id));
        set_named(env, object, "permissionSummary", js_owned(env, event->permission_summary));
        set_named(env, object, "permissionOptions", js_uint32(env, event->permission_options));
        break;
    case TNY_EVENT_USAGE:
        set_named(env, object, "inputTokens", js_big_int(env, event->input_tokens));
        set_named(env, object, "outputTokens", js_big_int(env, event->output_tokens));
        set_named(env, object, "contextUsed", js_big_int(env, event->context_used));
        set_named(env, object, "contextSize", js_big_int(env, event->context_size));
        set_named(env, object, "hasCost", js_bool(env, event->has_cost));
        if (event->has_cost) {
            set_named(env, object, "cost", js_double(env, event->cost));
        }
        break;
    case TNY_EVENT_TURN_END:
        set_named(env, object, "stopReason", js_string(env, stop_reason(event->stop_reason)));
        break;
    case TNY_EVENT_ERROR:
        set_named(env, object, "text", js_string(env, stable_error_text(event->error_code)));
        set_named(env, object, "errorCode", js_int32(env, event->error_code));
        break;
    default:
        break;
    }
    return build_status == napi_ok ? object : NULL;
}
