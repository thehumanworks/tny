/* libtny ABI 1 — stable headless embedding API.
 * See docs/adr/0036-libtny-abi-1.md. */
#ifndef LIBTNY_TNY_H
#define LIBTNY_TNY_H

#include <stdint.h>

#if defined(_WIN32)
#  if defined(TNY_BUILDING_LIBRARY)
#    define TNY_API __declspec(dllexport)
#  else
#    define TNY_API __declspec(dllimport)
#  endif
#  define TNY_CALL __cdecl
#elif defined(__GNUC__) || defined(__clang__)
#  define TNY_API __attribute__((visibility("default")))
#  define TNY_CALL
#else
#  define TNY_API
#  define TNY_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TNY_ABI_MAJOR 1u
#define TNY_ABI_MINOR 0u
#define TNY_ABI_VERSION ((TNY_ABI_MAJOR << 16) | TNY_ABI_MINOR)

/* Non-error outcomes. */
#define TNY_STATUS_OK       0
#define TNY_STATUS_EVENT    1
#define TNY_STATUS_TIMEOUT  2
#define TNY_STATUS_DRAINED  3

/* Stable error categories. */
#define TNY_STATUS_INVALID_ARGUMENT (-1)
#define TNY_STATUS_BAD_STATE        (-2)
#define TNY_STATUS_BUSY             (-3)
#define TNY_STATUS_OOM              (-4)
#define TNY_STATUS_CONFIG           (-5)
#define TNY_STATUS_AUTH             (-6)
#define TNY_STATUS_IO               (-7)
#define TNY_STATUS_TIMEOUT_ERROR    (-8)
#define TNY_STATUS_UNSUPPORTED      (-9)
#define TNY_STATUS_PROTOCOL         (-10)
#define TNY_STATUS_BACKPRESSURE     (-11)
#define TNY_STATUS_CANCELLED        (-12)
#define TNY_STATUS_INTERNAL         (-13)

#define TNY_PERMISSION_ASK  0u
#define TNY_PERMISSION_AUTO 1u
#define TNY_PERMISSION_YOLO 2u

#define TNY_PERMISSION_ALLOW        0u
#define TNY_PERMISSION_ALLOW_ALWAYS 1u
#define TNY_PERMISSION_DENY         2u

#define TNY_PERMISSION_OPTION_ALLOW        (1u << 0)
#define TNY_PERMISSION_OPTION_ALLOW_ALWAYS (1u << 1)
#define TNY_PERMISSION_OPTION_DENY         (1u << 2)

#define TNY_EVENT_TEXT_DELTA    0u
#define TNY_EVENT_THINKING      1u
#define TNY_EVENT_TOOL_START    2u
#define TNY_EVENT_TOOL_END      3u
#define TNY_EVENT_PERMISSION    4u
#define TNY_EVENT_PLAN          5u
#define TNY_EVENT_USAGE         6u
#define TNY_EVENT_TURN_END      7u
#define TNY_EVENT_ERROR         8u
#define TNY_EVENT_STATUS        9u
#define TNY_EVENT_STEER_REJECTED 10u
#define TNY_EVENT_CUSTOM_MESSAGE 11u
#define TNY_EVENT_USER_MESSAGE   12u
#define TNY_EVENT_TOOL_PROGRESS  13u

#define TNY_STOP_REASON_DONE        0u
#define TNY_STOP_REASON_INTERRUPTED 1u
#define TNY_STOP_REASON_DENIED      2u
#define TNY_STOP_REASON_STEP_LIMIT  3u
#define TNY_STOP_REASON_ERROR       4u

typedef struct tny_runtime tny_runtime;
typedef struct tny_session tny_session;
typedef struct tny_event tny_event;
typedef struct tny_error tny_error;
typedef struct tny_tool_registration tny_tool_registration;
typedef struct tny_tool_call tny_tool_call;

typedef struct {
    const char *ptr;
    uint64_t len;
} tny_bytes;

#ifdef __cplusplus
#  define TNY_CALLBACK_NOEXCEPT noexcept
#else
#  define TNY_CALLBACK_NOEXCEPT
#endif

#define TNY_HOST_SERVICES_ABI_VERSION 1u
#define TNY_RUNTIME_OPTIONS_ABI_VERSION 1u
#define TNY_DIAGNOSTIC_DEBUG 0u
#define TNY_DIAGNOSTIC_INFO  1u
#define TNY_DIAGNOSTIC_WARN  2u
#define TNY_DIAGNOSTIC_ERROR 3u

typedef int32_t (TNY_CALL *tny_host_diagnostic_fn)(
    void *user_data, uint32_t level, tny_bytes component,
    tny_bytes message) TNY_CALLBACK_NOEXCEPT;
typedef int32_t (TNY_CALL *tny_host_monotonic_ms_fn)(
    void *user_data, int64_t *out_ms) TNY_CALLBACK_NOEXCEPT;
typedef int32_t (TNY_CALL *tny_host_secure_random_fn)(
    void *user_data, void *buffer, uint64_t buffer_size) TNY_CALLBACK_NOEXCEPT;
typedef int32_t (TNY_CALL *tny_host_storage_load_fn)(
    void *user_data, tny_bytes key, uint64_t *out_revision,
    void *buffer, uint64_t buffer_capacity,
    uint64_t *out_size) TNY_CALLBACK_NOEXCEPT;
typedef int32_t (TNY_CALL *tny_host_storage_store_fn)(
    void *user_data, tny_bytes key, uint64_t expected_revision,
    const void *data, uint64_t data_size,
    uint64_t *out_revision) TNY_CALLBACK_NOEXCEPT;
typedef int32_t (TNY_CALL *tny_host_open_url_fn)(
    void *user_data, tny_bytes url) TNY_CALLBACK_NOEXCEPT;
typedef int32_t (TNY_CALL *tny_host_notify_scheduler_fn)(
    void *user_data) TNY_CALLBACK_NOEXCEPT;

/* Frozen host-services v1 table. libtny copies the declared prefix during
 * runtime creation; the table itself may be released immediately afterward.
 * user_data and resources reachable from it must outlive the runtime. All
 * callbacks are synchronous, owner-thread-only and non-reentrant. Borrowed
 * input buffers are valid only for the callback. A callback returns a stable
 * TNY_STATUS_* value and must never unwind across this C boundary. */
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    void *user_data;
    tny_host_diagnostic_fn diagnostic;
    tny_host_monotonic_ms_fn monotonic_ms;
    tny_host_secure_random_fn secure_random;
    tny_host_storage_load_fn storage_load;
    tny_host_storage_store_fn storage_store;
    tny_host_open_url_fn open_url;
    tny_host_notify_scheduler_fn notify_scheduler;
    uint64_t reserved[8];
} tny_host_services_v1;

#define TNY_TOOL_SPEC_ABI_VERSION 1u
#define TNY_TOOL_RESULT_ABI_VERSION 1u
#define TNY_TOOL_INVOKE_SYNC  0
#define TNY_TOOL_INVOKE_ASYNC 1
#define TNY_TOOL_SENSITIVITY_SAFE      0u
#define TNY_TOOL_SENSITIVITY_SENSITIVE 1u
#define TNY_CUSTOM_TOOL_MAX_COUNT 64u
#define TNY_CUSTOM_TOOL_NAME_MAX 64u
#define TNY_CUSTOM_TOOL_DESCRIPTION_MAX 4096u
#define TNY_CUSTOM_TOOL_SCHEMA_MAX 65536u
#define TNY_CUSTOM_TOOL_ARGUMENTS_MAX 262144u
#define TNY_CUSTOM_TOOL_RESULT_MAX 1048576u

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    tny_bytes data;
    uint32_t is_error;
    uint32_t reserved_scalar;
    uint64_t reserved[4];
} tny_tool_result_v1;

typedef int32_t (TNY_CALL *tny_tool_invoke_fn)(
    void *user_data, tny_tool_call *call, uint64_t generation,
    tny_bytes arguments_json,
    tny_tool_result_v1 *out_result) TNY_CALLBACK_NOEXCEPT;

/* Frozen custom-tool v1 descriptor. Every byte view and callback pointer is
 * copied by registration. user_data remains host-owned through unregister.
 * invoke runs synchronously on the runtime owner and may return SYNC with a
 * borrowed result, ASYNC for later completion, or a stable TNY_STATUS_* error. */
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    void *user_data;
    tny_bytes name;
    tny_bytes description;
    tny_bytes input_schema_json;
    uint32_t sensitivity;
    uint32_t reserved_scalar;
    uint64_t max_argument_bytes;
    uint64_t max_result_bytes;
    tny_tool_invoke_fn invoke;
    uint64_t reserved[8];
} tny_tool_spec_v1;


#define TNY_EVENT_SCHEMA_VERSION 1u

/* Frozen-v0 event snapshot. Its reserved tail is part of the frozen size.
 * Future growth requires a v1 type plus new initializer/read symbols. */
typedef struct {
    uint32_t struct_size;
    uint32_t kind;
    uint32_t schema_version;
    uint32_t tool_ok;
    uint32_t permission_options;
    uint32_t stop_reason;
    int32_t error_code;
    uint32_t has_cost;
    uint64_t sequence;
    int64_t timestamp_ms;
    int64_t input_tokens;
    int64_t output_tokens;
    int64_t context_used;
    int64_t context_size;
    double cost;
    tny_bytes provider;
    tny_bytes session_id;
    tny_bytes turn_id;
    tny_bytes text;
    tny_bytes message_id;
    tny_bytes tool_name;
    tny_bytes tool_id;
    tny_bytes tool_detail;
    tny_bytes permission_id;
    tny_bytes permission_summary;
    tny_bytes message_type;
    uint64_t reserved[8];
} tny_event_view_v0;

#define TNY_CAPABILITY_SCHEMA_VERSION 1u

#define TNY_PROVIDER_NONE   0u
#define TNY_PROVIDER_OPENAI 1u
#define TNY_PROVIDER_CURSOR 2u
#define TNY_PROVIDER_CODEX  3u
#define TNY_PROVIDER_ACP    4u

#define TNY_PROVIDER_MASK_OPENAI (UINT64_C(1) << 0)
#define TNY_PROVIDER_MASK_CURSOR (UINT64_C(1) << 1)
#define TNY_PROVIDER_MASK_CODEX  (UINT64_C(1) << 2)
#define TNY_PROVIDER_MASK_ACP    (UINT64_C(1) << 3)

#define TNY_CAP_FEATURE_TLS              (UINT64_C(1) << 0)
#define TNY_CAP_FEATURE_PERSISTENCE      (UINT64_C(1) << 1)
#define TNY_CAP_FEATURE_SHARED_LIBRARY   (UINT64_C(1) << 2)
#define TNY_CAP_FEATURE_STATIC_LIBRARY   (UINT64_C(1) << 3)
#define TNY_CAP_FEATURE_MCP              (UINT64_C(1) << 4)
#define TNY_CAP_FEATURE_CUSTOM_TOOLS     (UINT64_C(1) << 5)
#define TNY_CAP_FEATURE_TERMINAL         (UINT64_C(1) << 6)
#define TNY_CAP_FEATURE_CROSS_THREAD_CANCEL (UINT64_C(1) << 7)
#define TNY_CAP_FEATURE_WINDOWS          (UINT64_C(1) << 8)
#define TNY_CAP_FEATURE_WASM             (UINT64_C(1) << 9)
#define TNY_CAP_FEATURE_FULLY_STATIC_TLS (UINT64_C(1) << 10)
#define TNY_CAP_FEATURE_HOST_SERVICES     (UINT64_C(1) << 11)

#define TNY_ENDPOINT_REACHABILITY_UNKNOWN     0u
#define TNY_ENDPOINT_REACHABILITY_REACHABLE   1u
#define TNY_ENDPOINT_REACHABILITY_UNREACHABLE 2u

#define TNY_THREADING_OWNER_THREAD 1u
#define TNY_CANCEL_OWNER_THREAD_ASYNC 1u
#define TNY_CANCEL_CROSS_THREAD_ASYNC_WAKE 2u

/* Frozen-v0 runtime capability snapshot. Byte views are borrowed from the
 * runtime (or immutable library storage) and remain valid until the runtime
 * is freed. Unknown mask bits and scalar values must be ignored. A future
 * larger snapshot uses a v1 type and new init/query symbols. */
typedef struct {
    uint32_t struct_size;
    uint32_t schema_version;
    uint32_t abi_version;
    uint32_t provider_selected;
    uint32_t provider_initialized;
    uint32_t endpoint_reachability;
    uint32_t threading_model;
    uint32_t cancel_model;
    uint64_t provider_available_mask;
    uint64_t feature_available_mask;
    uint64_t feature_enabled_mask;
    uint32_t event_queue_max;
    uint32_t event_reserved;
    uint64_t event_payload_bytes_max;
    uint64_t event_reserved_bytes;
    tny_bytes library_version;
    tny_bytes platform_family;
    tny_bytes architecture;
    tny_bytes transport;
    tny_bytes tls_implementation;
    tny_bytes linkage;
    uint64_t reserved[8];
} tny_capabilities_v0;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    tny_capabilities_v0 base;
    uint32_t custom_tool_max_count;
    uint32_t custom_tool_name_max;
    uint64_t custom_tool_schema_max;
    uint64_t custom_tool_arguments_max;
    uint64_t custom_tool_result_max;
    uint64_t reserved[8];
} tny_capabilities_v1;

/* Frozen-v0 runtime options. Future options require a v1 type and new
 * initializer/create entry point; reserved is not appendable storage. */
typedef struct {
    uint32_t struct_size;
    uint32_t permission_mode;
    uint32_t persistence; /* 0 = process-local, 1 = save under state_dir */
    uint32_t max_steps;   /* 0 = unlimited; otherwise <= INT32_MAX */
    uint64_t max_tool_result_bytes;
    tny_bytes workspace;  /* required existing directory */
    tny_bytes state_dir;  /* required iff persistence=1; not created when 0 */
    tny_bytes provider;   /* empty or "openai" in ABI 0 */
    tny_bytes model;      /* empty = provider default */
    tny_bytes base_url;   /* OpenAI-compatible provider only */
    tny_bytes api_key;    /* copied; never persisted by libtny */
    tny_bytes wire_api;   /* empty/responses or chat */
    uint64_t reserved[8];
} tny_runtime_options_v0;

/* v1 is a new initializer target; v0 remains frozen and is embedded by value.
 * Future v1-compatible additions may consume only this v1 reserved tail. */
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    tny_runtime_options_v0 runtime;
    const tny_host_services_v1 *host_services;
    uint64_t reserved[8];
} tny_runtime_options_v1;

/* Public runtimes use a fixed 15-second native connection deadline. Destruction closes
 * an active OpenAI HTTP transport without waiting for provider completion.
 * Process-spawning tools are unavailable in public runtimes. */

TNY_API uint32_t TNY_CALL tny_abi_version(void);
TNY_API tny_bytes TNY_CALL tny_library_version(void);
/* ABI 1 initializers reject NULL, capacities below the record's minimum
 * prefix, and capacities above UINT32_MAX. They touch at most the intersection
 * of the caller capacity and ABI-1 frozen size, record the caller capacity in
 * struct_size, and never read or clear an unknown caller tail. ABI-0.8's
 * unsafe one-argument signatures exist only in the compat0 artifact/header. */
TNY_API int32_t TNY_CALL tny_runtime_options_init(
    tny_runtime_options_v0 *options, uint64_t capacity);
TNY_API int32_t TNY_CALL tny_runtime_options_v1_init(
    tny_runtime_options_v1 *options, uint64_t capacity);
TNY_API int32_t TNY_CALL tny_host_services_v1_init(
    tny_host_services_v1 *services, uint64_t capacity);
TNY_API int32_t TNY_CALL tny_tool_spec_v1_init(
    tny_tool_spec_v1 *spec, uint64_t capacity);
TNY_API int32_t TNY_CALL tny_tool_result_v1_init(
    tny_tool_result_v1 *result, uint64_t capacity);
TNY_API int32_t TNY_CALL tny_capabilities_init(
    tny_capabilities_v0 *capabilities, uint64_t capacity);
TNY_API int32_t TNY_CALL tny_capabilities_v1_init(
    tny_capabilities_v1 *capabilities, uint64_t capacity);

/* Public handles are owner-thread-affine except tny_session_cancel, which is
 * safe and idempotent from any thread. The owner must keep the session alive
 * until every concurrent cancel call has returned; free and all other calls
 * remain owner-thread-only. Inputs are valid only for the call; libtny copies
 * retained UTF-8. Event views remain valid until tny_event_free. Free child
 * handles before their parents. */

TNY_API int32_t TNY_CALL tny_runtime_create(
    const tny_runtime_options_v0 *options, uint64_t capacity,
    tny_runtime **out_runtime, tny_error **out_error);
TNY_API int32_t TNY_CALL tny_runtime_create_v1(
    const tny_runtime_options_v1 *options, uint64_t capacity,
    tny_runtime **out_runtime, tny_error **out_error);
TNY_API void TNY_CALL tny_runtime_free(tny_runtime *runtime);
/* Authoritative repeat-safe teardown. The owner passes its handle slot; on a
 * valid owner call libtny nulls it before releasing children and storage. */
TNY_API int32_t TNY_CALL tny_runtime_destroy(tny_runtime **runtime);
TNY_API int32_t TNY_CALL tny_runtime_get_capabilities(
    const tny_runtime *runtime, tny_capabilities_v0 *capabilities,
    uint64_t capacity);
TNY_API int32_t TNY_CALL tny_runtime_get_capabilities_v1(
    const tny_runtime *runtime, tny_capabilities_v1 *capabilities,
    uint64_t capacity);

TNY_API int32_t TNY_CALL tny_runtime_register_tool(
    tny_runtime *runtime, const tny_tool_spec_v1 *spec,
    tny_tool_registration **out_registration, tny_error **out_error);
TNY_API int32_t TNY_CALL tny_tool_registration_unregister(
    tny_tool_registration *registration, tny_error **out_error);
TNY_API uint64_t TNY_CALL tny_tool_call_generation(const tny_tool_call *call);
/* Thread-safe while the owning runtime remains alive. The result is copied
 * before return. A generation mismatch, second completion, cancellation, or
 * unregistered tool returns TNY_STATUS_BAD_STATE and never invokes a callback. */
TNY_API int32_t TNY_CALL tny_tool_call_complete(
    tny_tool_call *call, uint64_t generation,
    const tny_tool_result_v1 *result, tny_error **out_error);
/* Release the host's async-call reference. Call exactly once after the final
 * completion attempt; after return the handle must not be touched. */
TNY_API void TNY_CALL tny_tool_call_release(tny_tool_call *call);

/* Direct service requests use the same callback guard as internal calls.
 * Native monotonic/random defaults are available without host callbacks.
 * Storage, URL opening and explicit scheduler notification return
 * TNY_STATUS_UNSUPPORTED when their callback is absent. */
TNY_API int32_t TNY_CALL tny_runtime_host_monotonic_ms(
    tny_runtime *runtime, int64_t *out_ms, tny_error **out_error);
TNY_API int32_t TNY_CALL tny_runtime_host_secure_random(
    tny_runtime *runtime, void *buffer, uint64_t buffer_size,
    tny_error **out_error);
TNY_API int32_t TNY_CALL tny_runtime_host_storage_load(
    tny_runtime *runtime, tny_bytes key, uint64_t *out_revision,
    void *buffer, uint64_t buffer_capacity, uint64_t *out_size,
    tny_error **out_error);
TNY_API int32_t TNY_CALL tny_runtime_host_storage_store(
    tny_runtime *runtime, tny_bytes key, uint64_t expected_revision,
    const void *data, uint64_t data_size, uint64_t *out_revision,
    tny_error **out_error);
TNY_API int32_t TNY_CALL tny_runtime_host_open_url(
    tny_runtime *runtime, tny_bytes url, tny_error **out_error);
TNY_API int32_t TNY_CALL tny_runtime_host_notify_scheduler(
    tny_runtime *runtime, tny_error **out_error);

TNY_API int32_t TNY_CALL tny_session_create(
    tny_runtime *runtime, tny_session **out_session, tny_error **out_error);
TNY_API int32_t TNY_CALL tny_session_open(
    tny_runtime *runtime, tny_bytes id,
    tny_session **out_session, tny_error **out_error);
TNY_API tny_bytes TNY_CALL tny_session_id(const tny_session *session);
TNY_API int32_t TNY_CALL tny_session_send(
    tny_session *session, tny_bytes prompt, tny_error **out_error);
TNY_API int32_t TNY_CALL tny_session_next_event(
    tny_session *session, uint32_t timeout_ms,
    tny_event **out_event, tny_error **out_error);
TNY_API int32_t TNY_CALL tny_session_steer(
    tny_session *session, tny_bytes text, tny_error **out_error);
TNY_API int32_t TNY_CALL tny_session_respond_permission(
    tny_session *session, tny_bytes request_id, uint32_t decision,
    tny_error **out_error);
TNY_API int32_t TNY_CALL tny_session_cancel(
    tny_session *session, tny_error **out_error);
TNY_API void TNY_CALL tny_session_free(tny_session *session);
TNY_API int32_t TNY_CALL tny_session_destroy(tny_session **session);

TNY_API int32_t TNY_CALL tny_event_view_init(
    tny_event_view_v0 *view, uint64_t capacity);
TNY_API int32_t TNY_CALL tny_event_read(
    const tny_event *event, tny_event_view_v0 *view, uint64_t capacity);
TNY_API uint32_t TNY_CALL tny_event_get_kind(const tny_event *event);
TNY_API tny_bytes TNY_CALL tny_event_text(const tny_event *event);
TNY_API tny_bytes TNY_CALL tny_event_tool_name(const tny_event *event);
TNY_API tny_bytes TNY_CALL tny_event_tool_id(const tny_event *event);
TNY_API tny_bytes TNY_CALL tny_event_tool_detail(const tny_event *event);
TNY_API uint32_t TNY_CALL tny_event_tool_ok(const tny_event *event);
TNY_API tny_bytes TNY_CALL tny_event_permission_id(const tny_event *event);
TNY_API tny_bytes TNY_CALL tny_event_permission_summary(const tny_event *event);
TNY_API tny_bytes TNY_CALL tny_event_message_type(const tny_event *event);
TNY_API uint32_t TNY_CALL tny_event_permission_options(const tny_event *event);
TNY_API int64_t TNY_CALL tny_event_input_tokens(const tny_event *event);
TNY_API int64_t TNY_CALL tny_event_output_tokens(const tny_event *event);
TNY_API uint32_t TNY_CALL tny_event_stop_reason(const tny_event *event);
TNY_API int32_t TNY_CALL tny_event_error_code(const tny_event *event);
TNY_API void TNY_CALL tny_event_free(tny_event *event);

TNY_API int32_t TNY_CALL tny_error_code(const tny_error *error);
TNY_API tny_bytes TNY_CALL tny_error_message(const tny_error *error);
TNY_API void TNY_CALL tny_error_free(tny_error *error);

#ifdef __cplusplus
}
#endif

#endif
