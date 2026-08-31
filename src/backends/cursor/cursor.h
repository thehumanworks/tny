/* cursor.h — internal contract for the Cursor SDK Bridge backend
 * (docs/backends/cursor-bridge.md). Nothing here escapes this directory. */
#ifndef TNY_BACKENDS_CURSOR_H
#define TNY_BACKENDS_CURSOR_H

#include "core/backend.h"
#include "core/config.h"
#include "json/json.h"
#include "net/net.h"
#include "util/util.h"

#include <stdbool.h>
#include <sys/types.h>

/* The bridge is a host process: treat everything it prints or returns as
 * untrusted input and cap it. */
#define CURSOR_MAX_MSG_BYTES            (8u * 1024u * 1024u)
#define CURSOR_MAX_STDERR               (1u * 1024u * 1024u)
#define CURSOR_READY_PREFIX             "cursor-sdk-bridge ready "
#define CURSOR_BRIDGE_READY_TIMEOUT_ENV "TNY_CURSOR_BRIDGE_READY_TIMEOUT_MS"

/* Parse the optional ready-line timeout override shared by conversational and
 * management bridge startup. NULL selects the 30-second default. */
int cursor_bridge_ready_timeout_ms(const char *value, int *timeout_ms, char *err, size_t errlen);

/* Connect service paths (docs/backends/cursor-bridge.md, "Services tny must
 * call"). Method names are appended by the RPC helpers. */
#define CURSOR_SVC_CONTROL "/sdk.v1.SdkBridgeControlService"
#define CURSOR_SVC_CURSOR  "/sdk.v1.SdkCursorService"
#define CURSOR_SVC_AGENT   "/sdk.v1.SdkAgentService"

/* ---- ready line (stderr handshake) ---- */

typedef struct {
    int schema_version;
    char transport[16];
    char protocol[16];
    char url[512]; /* normalized "http://host:port" */
    char auth_token_file[1024];
    char auth_token[512]; /* inline token on older bridges; "" if absent */
} cursor_ready;

/* Parse one stderr line.
 *   1  line was the ready line and is valid (out filled)
 *   0  line is not a ready line (out untouched)
 *  -1  line was a ready line but is invalid (err filled)
 * Never copies the token into err. */
int cursor_ready_parse(const char *line, size_t len, cursor_ready *out, char *err, size_t errlen);
bool cursor_ready_is_line(const char *line, size_t len);

/* ---- request bodies ---- */

/* Append the ModelSelection "fast" param for a speed tier (TNY_CAP_FAST):
 * fast tiers pin the fast variant, "default" pins the standard one, NULL or
 * empty appends nothing so the model's own default variant applies. */
void cursor_append_model_params(buf_t *b, const char *tier);
bool cursor_append_fast_param(buf_t *b, const char *tier, bool first);

/* ---- bridge process ---- */

typedef struct {
    pid_t pid;
    int err_fd; /* bridge stdout+stderr read end, non-blocking */
    buf_t acc;  /* partial line */
    bool ready;
    bool quiet; /* embedding host owns diagnostics */
    cursor_ready info;
    char token[512];
} cursor_bridge;

typedef struct {
    const char *state_root;         /* optional --state-root */
    const char *local_store_json;   /* optional --local-store object */
    const char *store_callback_url; /* both callback fields or neither */
    const char *store_callback_token;
} cursor_bridge_launch_options;

void cursor_bridge_init(cursor_bridge *bp);
/* Spawn, block until the ready line (timeout_ms), load the bearer token. */
int cursor_bridge_spawn(cursor_bridge *bp, tny_ctx *ctx, const char *api_key,
                        const cursor_bridge_launch_options *options, int timeout_ms, char *err,
                        size_t errlen);
/* Non-blocking: forward complete lines to our stderr. Never the ready line. */
void cursor_bridge_pump(cursor_bridge *bp);
/* SIGTERM, wait grace_ms, SIGKILL. Idempotent. */
void cursor_bridge_stop(cursor_bridge *bp, int grace_ms);

/* ---- Connect unary RPC (JSON codec) ---- */

typedef struct {
    http_conn *conn;
    char base_url[512];
    char token[512];
} cursor_rpc;

void cursor_rpc_init(cursor_rpc *r, const char *base_url, const char *token);
void cursor_rpc_close(cursor_rpc *r);
/* Blocking POST. Returns the malloc'd response body, or NULL with err set. */
char *cursor_rpc_unary(cursor_rpc *r, const char *service, const char *method, const char *body,
                       int timeout_ms, char *err, size_t errlen);
/* Transport-level variant: returns the bounded body for every HTTP status and
 * writes status_out. NULL means no complete HTTP response was received. */
char *cursor_rpc_unary_raw(cursor_rpc *r, const char *service, const char *method, const char *body,
                           int timeout_ms, int *status_out, char *err, size_t errlen);

/* ---- Connect server stream ---- */

typedef enum { CS_IDLE, CS_HEADERS, CS_BODY } cursor_stream_state;

typedef struct {
    http_conn *conn;
    connect_decoder dec;
    cursor_stream_state state;
    char base_url[512];
    char token[512];
} cursor_stream;

void cursor_stream_init(cursor_stream *s, const char *base_url, const char *token);
/* Sends body as one Connect envelope. 0 ok, -1 with err set. */
int cursor_stream_start(cursor_stream *s, const char *service, const char *method, const char *body,
                        char *err, size_t errlen);
void cursor_stream_stop(cursor_stream *s);
int cursor_stream_fd(cursor_stream *s); /* -1 when idle */
/* Pump readable bytes; cb runs per decoded envelope.
 * 0 need more, 1 stream ended, -1 fatal (err set). */
int cursor_stream_pump(cursor_stream *s, connect_frame_cb cb, void *ud, char *err, size_t errlen);
/* As above, but returns a non-200 Connect body to the caller for structured
 * sdk.v1 error decoding. error_body_out is malloc'd when set. */
int cursor_stream_pump_raw(cursor_stream *s, connect_frame_cb cb, void *ud, int *status_out,
                           char **error_body_out, char *err, size_t errlen);

/* Shared: turn a Connect error body into a one-line message. */
void cursor_error_line(const char *body, size_t len, const char *fallback, char *out,
                       size_t outlen);

#endif
