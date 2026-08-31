/* management.c -- bridge lifecycle and bounded raw sdk.v1 invocations. */
#include "backends/cursor/management.h"

#include "core/backend.h"
#include "core/cursor_config.h"
#include "json/json.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *management_api_key(const tny_ctx *ctx) {
    const char *key = getenv("CURSOR_API_KEY");
    if (key && *key) return key;
    if (ctx && ctx->backend == TNY_BK_CURSOR && ctx->api_key && *ctx->api_key) return ctx->api_key;
    return NULL;
}

static bool json_store_is_custom(const char *json) {
    if (!json) return false;
    yyjson_doc *doc = jparse(json, strlen(json));
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *type = jget_str(root, "type");
    bool custom = type && strcmp(type, "custom") == 0;
    yyjson_doc_free(doc);
    return custom;
}

static bool agent_store_is_custom(const tny_ctx *ctx) {
    const char *json = ctx && ctx->cursor_config ? ctx->cursor_config->agent_options_json : NULL;
    yyjson_doc *doc = json ? jparse(json, strlen(json)) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *type = jget_str(jget(jget(root, "local"), "store"), "type");
    bool custom = type && strcmp(type, "custom") == 0;
    yyjson_doc_free(doc);
    return custom;
}

const cursor_sdk_route *cursor_management_route(const char *service, const char *method) {
    if (!service || !*service || !method || !*method) return NULL;
    char canonical[96];
    if (service[0] == '/') {
        if (!str_starts(service, "/sdk.v1.")) return NULL;
        if (snprintf(canonical, sizeof canonical, "%s", service) >= (int)sizeof canonical)
            return NULL;
    } else if (str_starts(service, "sdk.v1.")) {
        if (snprintf(canonical, sizeof canonical, "/%s", service) >= (int)sizeof canonical)
            return NULL;
    } else {
        if (strchr(service, '/') || strchr(service, '.')) return NULL;
        if (snprintf(canonical, sizeof canonical, "/sdk.v1.%s", service) >= (int)sizeof canonical)
            return NULL;
    }
    return cursor_sdk_route_find(canonical, method);
}

int cursor_management_validate_json(const char *json, char *err, size_t errlen) {
    if (!json) {
        snprintf(err, errlen, "cursor: missing JSON request object");
        return -1;
    }
    size_t len = strlen(json);
    if (len > CURSOR_MAX_MSG_BYTES) {
        snprintf(err, errlen, "cursor: request exceeds %u bytes", CURSOR_MAX_MSG_BYTES);
        return -1;
    }
    if (!utf8_valid_bytes(json, len)) {
        snprintf(err, errlen, "cursor: request is not valid UTF-8");
        return -1;
    }
    yyjson_doc *doc = jparse(json, len);
    bool object = doc && yyjson_is_obj(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (!object) {
        snprintf(err, errlen, "cursor: request must be one JSON object");
        return -1;
    }
    return 0;
}

int cursor_management_open(cursor_management *management, tny_ctx *ctx, int timeout_ms, char *err,
                           size_t errlen) {
    if (!management || !ctx) {
        snprintf(err, errlen, "cursor: invalid management client");
        return -1;
    }
    memset(management, 0, sizeof *management);
    cursor_bridge_init(&management->bridge);
    const char *key = management_api_key(ctx);
    if (!key) {
        snprintf(err, errlen, "cursor: CURSOR_API_KEY is not set");
        return -1;
    }
    tny_cursor_config *config = ctx->cursor_config;
    bool custom_store =
        config && !ctx->no_save &&
        (json_store_is_custom(config->local_store_json) || agent_store_is_custom(ctx));
    if (config && !ctx->no_save &&
        (json_store_is_custom(config->local_store_json) || agent_store_is_custom(ctx)) &&
        !config->store_callbacks) {
        snprintf(err, errlen, "cursor: custom store requires callbacks.store=true");
        return -1;
    }
    if (custom_store) {
        cursor_callbacks_options callback_options = {
            .tools = NULL,
            .state_dir = config->state_root ? config->state_root : ctx->tny_dir,
            .enable_tools = false,
            .enable_store = true,
            .allow_sensitive_tools = false,
            .thread_create = NULL,
        };
        management->callbacks = cursor_callbacks_start(&callback_options, err, errlen);
        if (!management->callbacks) return -1;
    }
    cursor_bridge_launch_options launch = {
        config && !ctx->no_save ? config->state_root : NULL,
        config && !ctx->no_save ? config->local_store_json : NULL,
        custom_store ? cursor_callbacks_url(management->callbacks) : NULL,
        custom_store ? cursor_callbacks_token(management->callbacks) : NULL,
    };
    if (cursor_bridge_spawn(&management->bridge, ctx, key, &launch, timeout_ms, err, errlen) != 0) {
        cursor_callbacks_destroy(&management->callbacks);
        return -1;
    }
    cursor_sdk_client_init(&management->sdk, management->bridge.info.url, management->bridge.token);
    management->open = true;
    cursor_sdk_error sdk_error;
    cursor_sdk_error_init(&sdk_error);
    int rc = 0;
    if (management->callbacks &&
        cursor_callbacks_blocking_begin(management->callbacks, err, errlen) != 0)
        rc = -1;
    else {
        rc = cursor_sdk_client_negotiate(&management->sdk, timeout_ms, &sdk_error, err, errlen);
        if (management->callbacks) cursor_callbacks_blocking_end(management->callbacks);
    }
    cursor_sdk_error_free(&sdk_error);
    if (rc != 0) {
        cursor_management_close(management);
        return -1;
    }
    return 0;
}

void cursor_management_close(cursor_management *management) {
    if (!management) return;
    if (management->open && !management->shutdown_sent) {
        char ignored[128];
        char *response =
            cursor_management_unary(management, CURSOR_SDK_RPC_SHUTDOWN, "{\"graceSeconds\":1}",
                                    2000, ignored, sizeof ignored);
        free(response);
    }
    if (management->open) cursor_sdk_client_close(&management->sdk);
    cursor_bridge_stop(&management->bridge, 2000);
    cursor_callbacks_destroy(&management->callbacks);
    memset(management, 0, sizeof *management);
}

char *cursor_management_unary(cursor_management *management, cursor_sdk_rpc_id id,
                              const char *request_json, int timeout_ms, char *err, size_t errlen) {
    if (!management || !management->open) {
        snprintf(err, errlen, "cursor: management client is not connected");
        return NULL;
    }
    if (cursor_management_validate_json(request_json, err, errlen) != 0) return NULL;
    if (management->callbacks &&
        cursor_callbacks_blocking_begin(management->callbacks, err, errlen) != 0)
        return NULL;
    cursor_sdk_error sdk_error;
    cursor_sdk_error_init(&sdk_error);
    char *response = cursor_sdk_invoke_unary(&management->sdk, id, request_json, timeout_ms,
                                             &sdk_error, err, errlen);
    if (management->callbacks) cursor_callbacks_blocking_end(management->callbacks);
    cursor_sdk_error_free(&sdk_error);
    if (response && id == CURSOR_SDK_RPC_SHUTDOWN) management->shutdown_sent = true;
    return response;
}

typedef struct {
    cursor_management_frame_cb cb;
    void *ud;
    int callback_rc;
    char *err;
    size_t errlen;
} management_frame_context;

static void management_frame(uint8_t flags, const char *payload, size_t len, void *ud) {
    management_frame_context *context = ud;
    if (context->callback_rc != 0 || !context->cb) return;
    context->callback_rc =
        context->cb(flags, payload, len, context->ud, context->err, context->errlen);
}

int cursor_management_stream(cursor_management *management, cursor_sdk_rpc_id id,
                             const char *request_json, int timeout_ms,
                             cursor_management_frame_cb cb, void *ud, char *err, size_t errlen) {
    if (!management || !management->open) {
        snprintf(err, errlen, "cursor: management client is not connected");
        return -1;
    }
    if (cursor_management_validate_json(request_json, err, errlen) != 0) return -1;
    if (cursor_sdk_invoke_stream(&management->sdk, id, request_json, err, errlen) != 0) return -1;

    management_frame_context context = {cb, ud, 0, err, errlen};
    int64_t deadline = timeout_ms < 0 ? INT64_MAX : monotonic_ms() + timeout_ms;
    for (;;) {
        cursor_bridge_pump(&management->bridge);
        cursor_sdk_error sdk_error;
        cursor_sdk_error_init(&sdk_error);
        int rc = cursor_sdk_stream_pump(&management->sdk, management_frame, &context, &sdk_error,
                                        err, errlen);
        cursor_sdk_error_free(&sdk_error);
        if (rc < 0) return -1;
        if (rc > 0) return context.callback_rc;
        if (context.callback_rc != 0) {
            cursor_sdk_stream_stop(&management->sdk);
            return -1;
        }
        int left = timeout_ms < 0 ? 1000 : (int)(deadline - monotonic_ms());
        if (left <= 0) {
            cursor_sdk_stream_stop(&management->sdk);
            snprintf(err, errlen, "cursor: stream timed out");
            return -1;
        }
        if (left > 1000) left = 1000;
        int fd = cursor_sdk_stream_fd(&management->sdk);
        if (fd < 0) {
            snprintf(err, errlen, "cursor: stream closed without completion");
            return -1;
        }
        struct pollfd pollfds[1 + CURSOR_CALLBACK_POLLFD_CAPACITY];
        int count = 0;
        pollfds[count++] = (struct pollfd){fd, POLLIN, 0};
        if (management->callbacks)
            count += cursor_callbacks_pollfds(management->callbacks, pollfds + count,
                                              (int)(sizeof pollfds / sizeof pollfds[0]) - count);
        int polled = tny_poll(pollfds, (nfds_t)count, left);
        if (polled < 0 && errno != EINTR) {
            snprintf(err, errlen, "cursor: stream poll failed: %s", strerror(errno));
            cursor_sdk_stream_stop(&management->sdk);
            return -1;
        }
        if (polled > 0 && management->callbacks &&
            cursor_callbacks_dispatch(management->callbacks, pollfds, count) != 0) {
            snprintf(err, errlen, "cursor: callback server failed");
            cursor_sdk_stream_stop(&management->sdk);
            return -1;
        }
    }
}
