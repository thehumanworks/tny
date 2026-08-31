/* sdk_client.h -- complete sdk.v1 adapter-to-bridge route inventory and
 * bounded raw-JSON client. Generated protobuf bindings are deliberately not
 * required: bridge JSON is the forward-compatible interchange boundary. */
#ifndef TNY_BACKENDS_CURSOR_SDK_CLIENT_H
#define TNY_BACKENDS_CURSOR_SDK_CLIENT_H

#include "backends/cursor/cursor.h"
#include "backends/cursor/sdk_error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CURSOR_SDK_PROTOCOL_VERSION   "sdk.v1"
#define CURSOR_SDK_MAX_CAPABILITIES   64u
#define CURSOR_SDK_MAX_CAPABILITY_LEN 95u

typedef enum {
    CURSOR_SDK_RPC_PING = 0,
    CURSOR_SDK_RPC_SHUTDOWN,
    CURSOR_SDK_RPC_GET_VERSION,
    CURSOR_SDK_RPC_SET_TOOL_CALLBACK,
    CURSOR_SDK_RPC_ME,
    CURSOR_SDK_RPC_LIST_MODELS,
    CURSOR_SDK_RPC_LIST_REPOSITORIES,
    CURSOR_SDK_RPC_CREATE_AGENT,
    CURSOR_SDK_RPC_RESUME_AGENT,
    CURSOR_SDK_RPC_RELOAD_AGENT,
    CURSOR_SDK_RPC_CLOSE_AGENT,
    CURSOR_SDK_RPC_SEND,
    CURSOR_SDK_RPC_WAIT_LIVE_RUN,
    CURSOR_SDK_RPC_GET_RUN,
    CURSOR_SDK_RPC_LIST_RUNS,
    CURSOR_SDK_RPC_GET_RUN_CONVERSATION,
    CURSOR_SDK_RPC_OBSERVE_RUN,
    CURSOR_SDK_RPC_CANCEL_RUN,
    CURSOR_SDK_RPC_GET_AGENT,
    CURSOR_SDK_RPC_LIST_AGENTS,
    CURSOR_SDK_RPC_ARCHIVE_AGENT,
    CURSOR_SDK_RPC_UNARCHIVE_AGENT,
    CURSOR_SDK_RPC_DELETE_AGENT,
    CURSOR_SDK_RPC_LIST_AGENT_MESSAGES,
    CURSOR_SDK_RPC_LIST_ARTIFACTS,
    CURSOR_SDK_RPC_DOWNLOAD_ARTIFACT,
    CURSOR_SDK_RPC_GET_USAGE,
    CURSOR_SDK_RPC_COUNT
} cursor_sdk_rpc_id;

typedef enum {
    CURSOR_SDK_UNARY,
    CURSOR_SDK_SERVER_STREAM,
} cursor_sdk_rpc_kind;

typedef struct {
    cursor_sdk_rpc_id id;
    const char *service;
    const char *method;
    cursor_sdk_rpc_kind kind;
    /* NULL means part of the sdk.v1 handshake/control baseline. */
    const char *required_capability;
} cursor_sdk_route;

typedef struct {
    char bridge_version[64];
    char protocol_version[32];
    char capabilities[CURSOR_SDK_MAX_CAPABILITIES][CURSOR_SDK_MAX_CAPABILITY_LEN + 1u];
    size_t capability_count;
    bool capabilities_truncated;
} cursor_sdk_version;

typedef struct {
    cursor_rpc rpc;
    cursor_stream stream;
    cursor_sdk_version version;
    bool negotiated;
    cursor_sdk_rpc_id active_stream;
    bool stream_saw_end;
    bool stream_protocol_error;
    char stream_protocol_error_message[192];
    char *stream_end_error_payload;
} cursor_sdk_client;

const cursor_sdk_route *cursor_sdk_route_by_id(cursor_sdk_rpc_id id);
const cursor_sdk_route *cursor_sdk_route_find(const char *service, const char *method);

int cursor_sdk_version_parse(cursor_sdk_version *version, const char *json, size_t len, char *err,
                             size_t errlen);
bool cursor_sdk_has_capability(const cursor_sdk_version *version, const char *capability);
bool cursor_sdk_route_supported(const cursor_sdk_version *version, const cursor_sdk_route *route);

void cursor_sdk_client_init(cursor_sdk_client *client, const char *base_url, const char *token);
void cursor_sdk_client_close(cursor_sdk_client *client);

/* Ping/GetVersion, require protocolVersion == "sdk.v1", retain all bounded
 * capability strings. */
int cursor_sdk_client_negotiate(cursor_sdk_client *client, int timeout_ms,
                                cursor_sdk_error *sdk_error, char *err, size_t errlen);

/* Invoke an enumerated route with a raw JSON object. Unknown response fields
 * stay intact in the returned malloc'd JSON. Error details are decoded into
 * sdk_error when present. */
/* Non-NULL sdk_error arguments must have been initialized with
 * cursor_sdk_error_init(). */
char *cursor_sdk_invoke_unary(cursor_sdk_client *client, cursor_sdk_rpc_id id,
                              const char *request_json, int timeout_ms, cursor_sdk_error *sdk_error,
                              char *err, size_t errlen);
int cursor_sdk_invoke_stream(cursor_sdk_client *client, cursor_sdk_rpc_id id,
                             const char *request_json, char *err, size_t errlen);
int cursor_sdk_stream_fd(cursor_sdk_client *client);
int cursor_sdk_stream_pump(cursor_sdk_client *client, connect_frame_cb cb, void *ud,
                           cursor_sdk_error *sdk_error, char *err, size_t errlen);
void cursor_sdk_stream_stop(cursor_sdk_client *client);

#endif
