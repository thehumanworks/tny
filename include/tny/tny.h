/* libtny ABI 0 — experimental headless embedding API.
 * See docs/adr/0023-libtny-embedding-abi.md. */
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

#define TNY_ABI_MAJOR 0u
#define TNY_ABI_MINOR 1u
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

#define TNY_STOP_REASON_DONE        0u
#define TNY_STOP_REASON_INTERRUPTED 1u
#define TNY_STOP_REASON_DENIED      2u
#define TNY_STOP_REASON_STEP_LIMIT  3u
#define TNY_STOP_REASON_ERROR       4u

typedef struct tny_runtime tny_runtime;
typedef struct tny_session tny_session;
typedef struct tny_event tny_event;
typedef struct tny_error tny_error;

typedef struct {
    const char *ptr;
    uint64_t len;
} tny_bytes;

typedef struct {
    uint32_t struct_size;
    uint32_t permission_mode;
    uint32_t persistence; /* 0 = process-local, 1 = save under state_dir */
    uint32_t max_steps;
    uint64_t max_tool_result_bytes;
    tny_bytes workspace;  /* required existing directory */
    tny_bytes state_dir;  /* required; created lazily when persistence is on */
    tny_bytes provider;   /* empty or "openai" in ABI 0 */
    tny_bytes model;      /* empty = provider default */
    tny_bytes base_url;   /* OpenAI-compatible provider only */
    tny_bytes api_key;    /* copied; never persisted by libtny */
    tny_bytes wire_api;   /* empty/responses or chat */
    uint64_t reserved[8];
} tny_runtime_options_v0;

TNY_API uint32_t TNY_CALL tny_abi_version(void);
TNY_API tny_bytes TNY_CALL tny_library_version(void);
TNY_API void TNY_CALL tny_runtime_options_init(tny_runtime_options_v0 *options);

/* ABI 0 handles are owner-thread-affine, including cancel. Inputs are valid
 * only for the call; libtny copies retained UTF-8. Event views remain valid
 * until tny_event_free. Free child handles before their parents. */

TNY_API int32_t TNY_CALL tny_runtime_create(
    const tny_runtime_options_v0 *options,
    tny_runtime **out_runtime, tny_error **out_error);
TNY_API void TNY_CALL tny_runtime_free(tny_runtime *runtime);

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

TNY_API uint32_t TNY_CALL tny_event_get_kind(const tny_event *event);
TNY_API tny_bytes TNY_CALL tny_event_text(const tny_event *event);
TNY_API tny_bytes TNY_CALL tny_event_tool_name(const tny_event *event);
TNY_API tny_bytes TNY_CALL tny_event_tool_id(const tny_event *event);
TNY_API tny_bytes TNY_CALL tny_event_tool_detail(const tny_event *event);
TNY_API uint32_t TNY_CALL tny_event_tool_ok(const tny_event *event);
TNY_API tny_bytes TNY_CALL tny_event_permission_id(const tny_event *event);
TNY_API tny_bytes TNY_CALL tny_event_permission_summary(const tny_event *event);
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
