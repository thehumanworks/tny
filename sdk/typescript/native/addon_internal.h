#ifndef TNY_NODE_ADDON_INTERNAL_H
#define TNY_NODE_ADDON_INTERNAL_H

#include <node_api.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <tny/tny.h>

#define SDK_ABI_MAJOR    1u
#define SDK_ABI_MINOR    0u
#define COMMAND_CAPACITY 64u
#define OWNER_POLL_MS    5000u
#define MAX_SESSIONS     1u

typedef enum {
    CMD_RUNTIME_CREATE,
    CMD_SESSION_CREATE,
    CMD_SESSION_OPEN,
    CMD_SEND,
    CMD_NEXT,
    CMD_STEER,
    CMD_PERMISSION,
    CMD_CANCEL,
    CMD_ABORT,
    CMD_CAPABILITIES,
    CMD_SESSION_CLOSE,
    CMD_RUNTIME_CLOSE,
    CMD_ENV_CLEANUP
} command_kind;

typedef enum {
    RESULT_VOID,
    RESULT_RUNTIME,
    RESULT_SESSION,
    RESULT_EVENT,
    RESULT_CAPABILITIES
} result_kind;

typedef struct sdk_owned_bytes {
    char *ptr;
    uint64_t len;
} sdk_owned_bytes;

typedef struct capability_copy {
    uint32_t schema_version, abi_version, provider_selected;
    uint32_t provider_initialized, endpoint_reachability;
    uint32_t threading_model, cancel_model;
    uint64_t provider_available_mask, feature_available_mask, feature_enabled_mask;
    uint32_t event_queue_max, event_reserved;
    uint64_t event_payload_bytes_max, event_reserved_bytes;
    sdk_owned_bytes library_version, platform_family, architecture, transport;
    sdk_owned_bytes tls_implementation, linkage;
} capability_copy;

typedef struct event_copy {
    uint32_t kind, schema_version, tool_ok, permission_options, stop_reason;
    int32_t error_code;
    uint32_t has_cost;
    uint64_t sequence;
    int64_t timestamp_ms, input_tokens, output_tokens, context_used, context_size;
    double cost;
    sdk_owned_bytes provider, session_id, turn_id, text, message_id;
    sdk_owned_bytes tool_name, tool_id, tool_detail;
    sdk_owned_bytes permission_id, permission_summary, message_type;
} event_copy;

typedef struct create_options {
    sdk_owned_bytes workspace, state_dir, provider, model, base_url, api_key, wire_api;
    uint32_t permission_mode, persistence, max_steps;
    uint64_t max_tool_result_bytes;
    sdk_owned_bytes task_name, task_instructions;
    int task_set;
} create_options;

struct runtime_state;
typedef struct tsfn_token {
    _Atomic(struct runtime_state *) state;
} tsfn_token;
typedef struct command {
    command_kind kind;
    result_kind result;
    napi_deferred deferred;
    struct runtime_state *owner;
    uint32_t session_handle, decision;
    sdk_owned_bytes text;
    create_options create;
    int32_t status;
    char *error_message, *string_result;
    uint32_t abi_version;
    event_copy *event;
    capability_copy capabilities;
    int destroy_owner;
    int silent;
    int embedded;
} command;

typedef struct runtime_state {
    uint32_t id;
    napi_env env;
    napi_threadsafe_function tsfn;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_mutex_t session_mutex;
    pthread_cond_t cond;
    command *queue[COMMAND_CAPACITY];
    command *pending_next;
    command *priority_abort;
    command *priority_session_close;
    command *priority_close;
    size_t head, count, refs;
    int closing;
    atomic_int env_closing;
    tsfn_token *token;
    tny_runtime *runtime;
    tny_session *session;
    uint32_t session_handle;
    command cleanup_command;
    struct runtime_state *next;
} runtime_state;

char *sdk_copy_n(const char *value, size_t len);
char *sdk_copy_cstr(const char *value);
char *sdk_copy_bytes(tny_bytes value);
sdk_owned_bytes sdk_copy_owned(tny_bytes value);
void sdk_free_command(command *cmd);
tny_bytes sdk_view_of(sdk_owned_bytes value);
void sdk_wipe_owned_bytes(sdk_owned_bytes *value);
char *sdk_take_error(int32_t status, tny_error *error);
int sdk_snapshot_event(tny_event *source, event_copy **out);
int sdk_snapshot_capabilities(tny_runtime *runtime, capability_copy *copy);
void sdk_runtime_add(runtime_state *state);
runtime_state *sdk_runtime_acquire(uint32_t id, napi_env env);
void sdk_runtime_retain(runtime_state *state);
void sdk_runtime_release(runtime_state *state);
void sdk_runtime_abandon(runtime_state *state);
void sdk_runtime_destroy(runtime_state *state);
void sdk_cleanup_env(napi_env env);
void sdk_finalize_env_state(runtime_state *state);
int sdk_register_env_cleanup(napi_env env);
void sdk_unregister_env_cleanup(napi_env env);
int sdk_queue_push(runtime_state *state, command *cmd);
void *sdk_owner_main(void *opaque);
void sdk_define_probe(napi_env env, napi_value exports);
napi_value sdk_event_to_js(napi_env env, const event_copy *event);
int sdk_parse_create_options(napi_env env, napi_value object, create_options *options);
int sdk_arg_uint32(napi_env env, napi_value value, uint32_t *out);
int sdk_arg_string(napi_env env, napi_value value, sdk_owned_bytes *out);

#endif
