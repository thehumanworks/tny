/* management.h -- short-lived sdk.v1 bridge client for `tny cursor`.
 *
 * This is intentionally separate from the conversational backend: management
 * commands expose the public bridge services without manufacturing a tny
 * session or entering Cursor's agent event mapper. */
#ifndef TNY_BACKENDS_CURSOR_MANAGEMENT_H
#define TNY_BACKENDS_CURSOR_MANAGEMENT_H

#include "backends/cursor/callbacks.h"
#include "backends/cursor/sdk_client.h"

typedef struct {
    cursor_bridge bridge;
    cursor_sdk_client sdk;
    cursor_callbacks *callbacks;
    bool open;
    bool shutdown_sent;
} cursor_management;

typedef int (*cursor_management_frame_cb)(uint8_t flags, const char *payload, size_t len, void *ud,
                                          char *err, size_t errlen);

/* Accept the readable service spelling (SdkAgentService), its sdk.v1 fully
 * qualified spelling, or the canonical /sdk.v1 path. The method remains
 * case-sensitive. Callback services and unknown routes are rejected. */
const cursor_sdk_route *cursor_management_route(const char *service, const char *method);

/* Raw request bodies must be bounded sdk.v1 JSON objects. */
int cursor_management_validate_json(const char *json, char *err, size_t errlen);

int cursor_management_open(cursor_management *management, tny_ctx *ctx, int timeout_ms, char *err,
                           size_t errlen);
void cursor_management_close(cursor_management *management);

char *cursor_management_unary(cursor_management *management, cursor_sdk_rpc_id id,
                              const char *request_json, int timeout_ms, char *err, size_t errlen);
int cursor_management_stream(cursor_management *management, cursor_sdk_rpc_id id,
                             const char *request_json, int timeout_ms,
                             cursor_management_frame_cb cb, void *ud, char *err, size_t errlen);

#endif
