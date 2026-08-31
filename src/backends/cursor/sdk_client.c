/* sdk_client.c -- route-gated sdk.v1 JSON client over cursor rpc.c. */
#include "backends/cursor/sdk_client.h"

#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONTROL CURSOR_SVC_CONTROL
#define CURSOR  CURSOR_SVC_CURSOR
#define AGENT   CURSOR_SVC_AGENT

/* v1.0.30: all 27 adapter-to-bridge RPCs. Callback-service RPCs are omitted
 * because the bridge calls those in the opposite direction. */
static const cursor_sdk_route ROUTES[CURSOR_SDK_RPC_COUNT] = {
    {CURSOR_SDK_RPC_PING, CONTROL, "Ping", CURSOR_SDK_UNARY, NULL},
    {CURSOR_SDK_RPC_SHUTDOWN, CONTROL, "Shutdown", CURSOR_SDK_UNARY, NULL},
    {CURSOR_SDK_RPC_GET_VERSION, CONTROL, "GetVersion", CURSOR_SDK_UNARY, NULL},
    {CURSOR_SDK_RPC_SET_TOOL_CALLBACK, CONTROL, "SetToolCallback", CURSOR_SDK_UNARY, NULL},
    {CURSOR_SDK_RPC_ME, CURSOR, "Me", CURSOR_SDK_UNARY, "cursor.catalog"},
    {CURSOR_SDK_RPC_LIST_MODELS, CURSOR, "ListModels", CURSOR_SDK_UNARY, "cursor.catalog"},
    {CURSOR_SDK_RPC_LIST_REPOSITORIES, CURSOR, "ListRepositories", CURSOR_SDK_UNARY,
     "cursor.catalog"},
    {CURSOR_SDK_RPC_CREATE_AGENT, AGENT, "CreateAgent", CURSOR_SDK_UNARY, "agent.create"},
    {CURSOR_SDK_RPC_RESUME_AGENT, AGENT, "ResumeAgent", CURSOR_SDK_UNARY, "agent.resume"},
    {CURSOR_SDK_RPC_RELOAD_AGENT, AGENT, "ReloadAgent", CURSOR_SDK_UNARY, "agent.management"},
    {CURSOR_SDK_RPC_CLOSE_AGENT, AGENT, "CloseAgent", CURSOR_SDK_UNARY, "agent.management"},
    {CURSOR_SDK_RPC_SEND, AGENT, "Send", CURSOR_SDK_SERVER_STREAM, "agent.send"},
    {CURSOR_SDK_RPC_WAIT_LIVE_RUN, AGENT, "WaitLiveRun", CURSOR_SDK_UNARY, "run.wait"},
    {CURSOR_SDK_RPC_GET_RUN, AGENT, "GetRun", CURSOR_SDK_UNARY, "run.observe"},
    {CURSOR_SDK_RPC_LIST_RUNS, AGENT, "ListRuns", CURSOR_SDK_UNARY, "run.observe"},
    {CURSOR_SDK_RPC_GET_RUN_CONVERSATION, AGENT, "GetRunConversation", CURSOR_SDK_UNARY,
     "run.observe"},
    {CURSOR_SDK_RPC_OBSERVE_RUN, AGENT, "ObserveRun", CURSOR_SDK_SERVER_STREAM, "run.observe"},
    {CURSOR_SDK_RPC_CANCEL_RUN, AGENT, "CancelRun", CURSOR_SDK_UNARY, "run.cancel"},
    {CURSOR_SDK_RPC_GET_AGENT, AGENT, "GetAgent", CURSOR_SDK_UNARY, "agent.management"},
    {CURSOR_SDK_RPC_LIST_AGENTS, AGENT, "ListAgents", CURSOR_SDK_UNARY, "agent.management"},
    {CURSOR_SDK_RPC_ARCHIVE_AGENT, AGENT, "ArchiveAgent", CURSOR_SDK_UNARY, "agent.management"},
    {CURSOR_SDK_RPC_UNARCHIVE_AGENT, AGENT, "UnarchiveAgent", CURSOR_SDK_UNARY, "agent.management"},
    {CURSOR_SDK_RPC_DELETE_AGENT, AGENT, "DeleteAgent", CURSOR_SDK_UNARY, "agent.management"},
    {CURSOR_SDK_RPC_LIST_AGENT_MESSAGES, AGENT, "ListAgentMessages", CURSOR_SDK_UNARY,
     "agent.management"},
    {CURSOR_SDK_RPC_LIST_ARTIFACTS, AGENT, "ListArtifacts", CURSOR_SDK_UNARY, "artifacts.chunked"},
    {CURSOR_SDK_RPC_DOWNLOAD_ARTIFACT, AGENT, "DownloadArtifact", CURSOR_SDK_SERVER_STREAM,
     "artifacts.chunked"},
    {CURSOR_SDK_RPC_GET_USAGE, AGENT, "GetUsage", CURSOR_SDK_UNARY, "agent.usage"},
};

_Static_assert(sizeof ROUTES / sizeof ROUTES[0] == 27, "sdk.v1 route inventory drift");
_Static_assert(CURSOR_SDK_RPC_COUNT == 27, "sdk.v1 route enum drift");

const cursor_sdk_route *cursor_sdk_route_by_id(cursor_sdk_rpc_id id) {
    return id >= 0 && id < CURSOR_SDK_RPC_COUNT ? &ROUTES[id] : NULL;
}

const cursor_sdk_route *cursor_sdk_route_find(const char *service, const char *method) {
    if (!service || !method) return NULL;
    for (size_t i = 0; i < sizeof ROUTES / sizeof ROUTES[0]; i++) {
        if (strcmp(service, ROUTES[i].service) == 0 && strcmp(method, ROUTES[i].method) == 0)
            return &ROUTES[i];
    }
    return NULL;
}

int cursor_sdk_version_parse(cursor_sdk_version *version, const char *json, size_t len, char *err,
                             size_t errlen) {
    if (!version || (!json && len) || len > CURSOR_MAX_MSG_BYTES) {
        snprintf(err, errlen, "cursor: invalid GetVersion response");
        return -1;
    }
    memset(version, 0, sizeof *version);
    yyjson_doc *doc = jparse(json ? json : "", len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *bridge = jget_str(root, "bridgeVersion");
    const char *protocol = jget_str(root, "protocolVersion");
    yyjson_val *caps = jget(root, "capabilities");
    if (!yyjson_is_obj(root) || !bridge || !*bridge || !protocol || !*protocol ||
        !yyjson_is_arr(caps)) {
        yyjson_doc_free(doc);
        snprintf(err, errlen, "cursor: malformed GetVersion response");
        return -1;
    }
    snprintf(version->bridge_version, sizeof version->bridge_version, "%.63s", bridge);
    snprintf(version->protocol_version, sizeof version->protocol_version, "%.31s", protocol);
    size_t idx, max;
    yyjson_val *cap;
    yyjson_arr_foreach(caps, idx, max, cap) {
        const char *name = yyjson_get_str(cap);
        size_t n = name ? strlen(name) : 0;
        if (!name || !n || n > CURSOR_SDK_MAX_CAPABILITY_LEN ||
            version->capability_count == CURSOR_SDK_MAX_CAPABILITIES) {
            version->capabilities_truncated = true;
            continue;
        }
        bool duplicate = false;
        size_t i = 0;
        while (i < version->capability_count) {
            if (strcmp(version->capabilities[i], name) == 0) duplicate = true;
            i++; /* capability duplicate scan */
        }
        if (duplicate) continue;
        memcpy(version->capabilities[version->capability_count], name, n + 1u);
        version->capability_count++;
    }
    yyjson_doc_free(doc);
    return 0;
}

bool cursor_sdk_has_capability(const cursor_sdk_version *version, const char *capability) {
    if (!version || !capability) return false;
    for (size_t i = 0; i < version->capability_count; i++)
        if (strcmp(version->capabilities[i], capability) == 0) return true;
    return false;
}

bool cursor_sdk_route_supported(const cursor_sdk_version *version, const cursor_sdk_route *route) {
    return route && (!route->required_capability ||
                     cursor_sdk_has_capability(version, route->required_capability));
}

void cursor_sdk_client_init(cursor_sdk_client *client, const char *base_url, const char *token) {
    memset(client, 0, sizeof *client);
    cursor_rpc_init(&client->rpc, base_url, token);
    cursor_stream_init(&client->stream, base_url, token);
    client->active_stream = CURSOR_SDK_RPC_COUNT;
}

void cursor_sdk_client_close(cursor_sdk_client *client) {
    if (!client) return;
    cursor_stream_stop(&client->stream);
    cursor_rpc_close(&client->rpc);
    free(client->stream_end_error_payload);
    client->stream_end_error_payload = NULL;
    client->negotiated = false;
    client->active_stream = CURSOR_SDK_RPC_COUNT;
}

static int validate_request(cursor_sdk_client *client, cursor_sdk_rpc_id id, const char *json,
                            cursor_sdk_rpc_kind kind, const cursor_sdk_route **route_out, char *err,
                            size_t errlen) {
    const cursor_sdk_route *route = cursor_sdk_route_by_id(id);
    if (!client || !route || route->kind != kind || !json) {
        snprintf(err, errlen, "cursor: invalid sdk.v1 invocation");
        return -1;
    }
    size_t len = strlen(json);
    if (len > CURSOR_MAX_MSG_BYTES) {
        snprintf(err, errlen, "cursor: sdk.v1 request exceeds %u bytes", CURSOR_MAX_MSG_BYTES);
        return -1;
    }
    yyjson_doc *doc = jparse(json, len);
    bool object = doc && yyjson_is_obj(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (!object) {
        snprintf(err, errlen, "cursor: sdk.v1 request must be a JSON object");
        return -1;
    }
    if (route->required_capability &&
        (!client->negotiated || !cursor_sdk_route_supported(&client->version, route))) {
        snprintf(err, errlen, "cursor: bridge does not advertise required capability %s",
                 route->required_capability);
        return -1;
    }
    *route_out = route;
    return 0;
}

static void parse_or_synthesize_error(cursor_sdk_error *sdk_error, const char *body, size_t len,
                                      int status, const char *transport_error) {
    if (!sdk_error) return;
    if (body && cursor_sdk_error_parse(sdk_error, body, len, status) == 0) return;
    cursor_sdk_error_free(sdk_error);
    sdk_error->http_status = status;
    sdk_error->message = xstrdup(transport_error ? transport_error : "sdk.v1 request failed");
}

char *cursor_sdk_invoke_unary(cursor_sdk_client *client, cursor_sdk_rpc_id id,
                              const char *request_json, int timeout_ms, cursor_sdk_error *sdk_error,
                              char *err, size_t errlen) {
    const cursor_sdk_route *route;
    if (sdk_error) cursor_sdk_error_free(sdk_error);
    if (validate_request(client, id, request_json, CURSOR_SDK_UNARY, &route, err, errlen) != 0)
        return NULL;
    int status = 0;
    char *body = cursor_rpc_unary_raw(&client->rpc, route->service, route->method, request_json,
                                      timeout_ms, &status, err, errlen);
    if (!body) {
        parse_or_synthesize_error(sdk_error, NULL, 0, status, err);
        return NULL;
    }
    if (status != 200) {
        parse_or_synthesize_error(sdk_error, body, strlen(body), status, err);
        char line[300];
        cursor_error_line(body, strlen(body), "bridge RPC failed", line, sizeof line);
        snprintf(err, errlen, "%s failed: %s", route->method, line);
        free(body);
        return NULL;
    }
    return body;
}

int cursor_sdk_client_negotiate(cursor_sdk_client *client, int timeout_ms,
                                cursor_sdk_error *sdk_error, char *err, size_t errlen) {
    char *ping = cursor_sdk_invoke_unary(client, CURSOR_SDK_RPC_PING, "{}", timeout_ms, sdk_error,
                                         err, errlen);
    if (!ping) return -1;
    free(ping);
    char *version = cursor_sdk_invoke_unary(client, CURSOR_SDK_RPC_GET_VERSION, "{}", timeout_ms,
                                            sdk_error, err, errlen);
    if (!version) return -1;
    int rc = cursor_sdk_version_parse(&client->version, version, strlen(version), err, errlen);
    free(version);
    if (rc != 0) return -1;
    if (strcmp(client->version.protocol_version, CURSOR_SDK_PROTOCOL_VERSION) != 0) {
        snprintf(err, errlen, "cursor: bridge speaks protocol %.31s; expected %s",
                 client->version.protocol_version, CURSOR_SDK_PROTOCOL_VERSION);
        return -1;
    }
    client->negotiated = true;
    return 0;
}

int cursor_sdk_invoke_stream(cursor_sdk_client *client, cursor_sdk_rpc_id id,
                             const char *request_json, char *err, size_t errlen) {
    const cursor_sdk_route *route;
    if (validate_request(client, id, request_json, CURSOR_SDK_SERVER_STREAM, &route, err, errlen) !=
        0)
        return -1;
    if (cursor_stream_start(&client->stream, route->service, route->method, request_json, err,
                            errlen) != 0)
        return -1;
    client->active_stream = id;
    free(client->stream_end_error_payload);
    client->stream_end_error_payload = NULL;
    client->stream_saw_end = false;
    client->stream_protocol_error = false;
    client->stream_protocol_error_message[0] = 0;
    return 0;
}

int cursor_sdk_stream_fd(cursor_sdk_client *client) {
    return client ? cursor_stream_fd(&client->stream) : -1;
}

typedef struct {
    cursor_sdk_client *client;
    connect_frame_cb cb;
    void *ud;
} sdk_frame_ctx;

static void stream_protocol_error(sdk_frame_ctx *ctx, const char *message) {
    if (ctx->client->stream_protocol_error) return;
    ctx->client->stream_protocol_error = true;
    snprintf(ctx->client->stream_protocol_error_message,
             sizeof ctx->client->stream_protocol_error_message, "%s", message);
}

static bool end_stream_payload_valid(const char *payload, size_t len, bool *has_error) {
    *has_error = false;
    if (!payload || !len || len > CURSOR_MAX_MSG_BYTES) return false;
    yyjson_doc *doc = jparse(payload, len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return false;
    }
    yyjson_val *error = jget(root, "error");
    if (error && !yyjson_is_null(error)) {
        if (!yyjson_is_obj(error)) {
            yyjson_doc_free(doc);
            return false;
        }
        yyjson_val *code = jget(error, "code");
        yyjson_val *message = jget(error, "message");
        yyjson_val *details = jget(error, "details");
        if ((code && !yyjson_is_str(code) && !yyjson_is_null(code)) ||
            (message && !yyjson_is_str(message) && !yyjson_is_null(message)) ||
            (details && !yyjson_is_arr(details) && !yyjson_is_null(details))) {
            yyjson_doc_free(doc);
            return false;
        }
        *has_error = true;
    }
    yyjson_val *metadata = jget(root, "metadata");
    if (metadata && !yyjson_is_null(metadata)) {
        if (!yyjson_is_obj(metadata)) {
            yyjson_doc_free(doc);
            return false;
        }
        size_t idx, max;
        yyjson_val *key, *value;
        yyjson_obj_foreach(metadata, idx, max, key, value) {
            if (!yyjson_is_arr(value) && !yyjson_is_null(value)) {
                yyjson_doc_free(doc);
                return false;
            }
            if (yyjson_is_null(value)) continue;
            size_t value_idx, value_max;
            yyjson_val *item;
            yyjson_arr_foreach(value, value_idx, value_max, item) {
                if (!yyjson_is_str(item)) {
                    yyjson_doc_free(doc);
                    return false;
                }
            }
        }
    }
    yyjson_doc_free(doc);
    return true;
}

static void on_sdk_frame(uint8_t flags, const char *payload, size_t len, void *ud) {
    sdk_frame_ctx *ctx = ud;
    if (ctx->client->stream_protocol_error) return;
    if (flags != 0 && flags != 0x02u) {
        stream_protocol_error(ctx, (flags & 0x01u)
                                       ? "cursor: compressed Connect envelopes are unsupported"
                                       : "cursor: bridge sent unknown Connect envelope flags");
        return;
    }
    if (flags == 0x02u) {
        if (ctx->client->stream_saw_end) {
            stream_protocol_error(ctx, "cursor: bridge sent duplicate EndStream envelope");
            return;
        }
        bool has_error = false;
        if (!end_stream_payload_valid(payload, len, &has_error)) {
            stream_protocol_error(ctx, "cursor: bridge sent malformed EndStream payload");
            return;
        }
        ctx->client->stream_saw_end = true;
        if (!has_error) return;
        ctx->client->stream_end_error_payload = xstrndup(payload, len);
        if (!ctx->client->stream_end_error_payload)
            stream_protocol_error(ctx, "cursor: could not retain EndStream error");
        return;
    }
    if (ctx->client->stream_saw_end) {
        stream_protocol_error(ctx, "cursor: bridge sent data after EndStream envelope");
        return;
    }
    if (ctx->cb) ctx->cb(flags, payload, len, ctx->ud);
}

int cursor_sdk_stream_pump(cursor_sdk_client *client, connect_frame_cb cb, void *ud,
                           cursor_sdk_error *sdk_error, char *err, size_t errlen) {
    if (!client) {
        snprintf(err, errlen, "cursor: no sdk.v1 stream");
        return -1;
    }
    if (sdk_error) cursor_sdk_error_free(sdk_error);
    sdk_frame_ctx ctx = {client, cb, ud};
    int status = 0;
    char *error_body = NULL;
    int rc = cursor_stream_pump_raw(&client->stream, on_sdk_frame, &ctx, &status, &error_body, err,
                                    errlen);
    if (error_body) {
        parse_or_synthesize_error(sdk_error, error_body, strlen(error_body), status, err);
        free(error_body);
        cursor_sdk_stream_stop(client);
        return -1;
    }
    if (client->stream_protocol_error) {
        snprintf(err, errlen, "%s", client->stream_protocol_error_message);
        parse_or_synthesize_error(sdk_error, NULL, 0, status, err);
        cursor_sdk_stream_stop(client);
        return -1;
    }
    if (rc < 0) {
        parse_or_synthesize_error(sdk_error, NULL, 0, status, err);
        cursor_sdk_stream_stop(client);
        return -1;
    }
    if (rc != 0) {
        if (client->stream.dec.acc.len != 0) {
            snprintf(err, errlen, "cursor: bridge stream ended with a truncated Connect envelope");
            parse_or_synthesize_error(sdk_error, NULL, 0, status, err);
            cursor_sdk_stream_stop(client);
            return -1;
        }
        if (!client->stream_saw_end) {
            snprintf(err, errlen, "cursor: bridge stream ended without an EndStream envelope");
            parse_or_synthesize_error(sdk_error, NULL, 0, status, err);
            cursor_sdk_stream_stop(client);
            return -1;
        }
        if (client->stream_end_error_payload) {
            cursor_sdk_error temporary;
            cursor_sdk_error *parsed = sdk_error;
            if (!parsed) {
                cursor_sdk_error_init(&temporary);
                parsed = &temporary;
            }
            (void)cursor_sdk_error_parse(parsed, client->stream_end_error_payload,
                                         strlen(client->stream_end_error_payload), 200);
            snprintf(err, errlen, "cursor: bridge stream ended with %s",
                     parsed->message           ? parsed->message
                     : parsed->connect_code[0] ? parsed->connect_code
                                               : "a Connect error");
            if (!sdk_error) cursor_sdk_error_free(&temporary);
            cursor_sdk_stream_stop(client);
            return -1;
        }
        cursor_sdk_stream_stop(client);
    }
    return rc;
}

void cursor_sdk_stream_stop(cursor_sdk_client *client) {
    if (!client) return;
    cursor_stream_stop(&client->stream);
    free(client->stream_end_error_payload);
    client->stream_end_error_payload = NULL;
    client->active_stream = CURSOR_SDK_RPC_COUNT;
}

#undef CONTROL
#undef CURSOR
#undef AGENT
