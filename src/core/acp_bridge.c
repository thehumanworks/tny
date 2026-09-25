#include "core/acp_bridge.h"
#include "lib/custom_tools.h"
#include "core/swarm.h"
#include "core/team_runtime.h"
#include "util/alloc.h"
#include "net/net.h"
#include "util/process.h"
#include "util/tny_poll.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define BRIDGE_CLIENTS    4
#define BRIDGE_BYTES      (8u * 1024u * 1024u)
#define BRIDGE_TIMEOUT_MS 300000

typedef struct {
    int fd;
    bool initialized, read_closed;
    buf_t input, output;
    char *id, *original_args, *args;
    char tool_id[64];
    tools_call call;
    bool pending, permission, recorded;
    int64_t deadline;
} bridge_client;

struct tny_acp_bridge {
    tools_env env;
    tny_openai_control_cb control;
    void *control_ud;
    int listener;
    char *directory, *path, *servers;
    bool active, stop_requested;
    unsigned long long sequence;
    size_t forwarded_context;
    bridge_client clients[BRIDGE_CLIENTS];
};

static void response_free(tny_openai_control_response *r) {
    free(r->arguments_json);
    free(r->result);
    free(r->extension);
    free(r->reason);
}

static void pending_clear(bridge_client *c) {
    tools_call_free(&c->call);
    free(c->id);
    free(c->args);
    free(c->original_args);
    c->id = c->args = c->original_args = NULL;
    c->pending = c->permission = c->recorded = false;
    c->deadline = 0;
}

static void client_close(bridge_client *c) {
    pending_clear(c);
    if (c->fd >= 0) close(c->fd);
    buf_free(&c->input);
    buf_free(&c->output);
    c->fd = -1;
    c->initialized = c->read_closed = false;
}

static int queue_reply(bridge_client *c, const char *id, const char *body, bool error) {
    if (!body || tny_alloc_scope_failed()) return -1;
    buf_t line = {0};
    buf_appendf(&line, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"%s\":%s}\n", id ? id : "null",
                error ? "error" : "result", body);
    if (buf_oom(&line) || !line.data || !body || line.len > BRIDGE_BYTES ||
        c->output.len > BRIDGE_BYTES - line.len) {
        buf_free(&line);
        return -1;
    }
    buf_append(&c->output, line.data, line.len);
    buf_free(&line);
    return buf_oom(&c->output) ? -1 : 0;
}

static int rpc_error(bridge_client *c, const char *id, int code, const char *message) {
    buf_t body = {0};
    buf_appendf(&body, "{\"code\":%d,\"message\":", code);
    jescape(&body, message);
    buf_appends(&body, "}");
    int rc = buf_oom(&body) ? -1 : queue_reply(c, id, body.data, true);
    buf_free(&body);
    return rc;
}

static void execution_control(const tny_openai_control_request *request,
                              tny_openai_control_response *response, void *ud) {
    tny_acp_bridge *b = ud;
    if (b->control && !tny_alloc_scope_failed()) b->control(request, response, b->control_ud);
    if (response->stop) b->stop_requested = true;
}

static tny_openai_control_response control(tny_acp_bridge *b, bridge_client *c,
                                           tny_openai_control_kind kind, const char *result,
                                           bool ok) {
    tny_openai_control_response response = {0};
    if (b->control && !tny_alloc_scope_failed()) {
        tny_openai_control_request request = {
            .kind = kind,
            .tool_id = c->tool_id,
            .tool_name = c->call.name,
            .arguments_json = c->args,
            .original_arguments_json = c->original_args,
            .result = result,
            .original_ok = ok,
            .permission_summary = c->call.summary,
            .permission_options = TNY_PERM_ALLOW_ONCE | TNY_PERM_ALLOW_ALWAYS | TNY_PERM_DENY};
        b->control(&request, &response, b->control_ud);
    }
    return response;
}

static void emit(tny_acp_bridge *b, bridge_client *c, tny_event_kind kind, const char *detail,
                 bool ok) {
    if (!b->env.ev_cb || tny_alloc_scope_failed()) return;
    tny_backend_event ev = {.kind = kind,
                            .tool_id = c->tool_id,
                            .tool_name = c->call.name,
                            .tool_detail = detail,
                            .tool_ok = ok,
                            .perm_id = c->tool_id,
                            .perm_summary = detail,
                            .perm_options =
                                TNY_PERM_ALLOW_ONCE | TNY_PERM_ALLOW_ALWAYS | TNY_PERM_DENY};
    b->env.ev_cb(&ev, b->env.ev_ud);
}

static void subagent_control(tny_acp_bridge *b, bridge_client *c, tny_openai_control_kind kind,
                             const char *result, bool ok) {
    if (!b->control || !c->call.name || strcmp(c->call.name, "subagent") != 0 ||
        tny_alloc_scope_failed())
        return;
    const char *action = jget_str(c->call.args, "action");
    if (!action || (strcmp(action, "create") != 0 && strcmp(action, "message") != 0)) return;
    const char *id = jget_str(c->call.args, "id");
    tny_openai_control_request request = {
        .kind = kind,
        .subagent_id = id && *id ? id : c->tool_id,
        .subagent_action = action,
        .subagent_outcome =
            kind == TNY_OPENAI_CONTROL_SUBAGENT_END ? (ok ? "done" : "error") : NULL,
        .subagent_ok = ok,
        .result = result};
    tny_openai_control_response response = {0};
    b->control(&request, &response, b->control_ud);
    if (response.stop) b->stop_requested = true;
    response_free(&response);
}

static int complete(tny_acp_bridge *b, bridge_client *c, const char *result, bool is_error) {
    if (tny_alloc_scope_failed()) return -1;
    size_t image_bytes = 0;
    for (int i = 0; i < b->env.n_pending_images; i++) {
        const tools_pending_capture *capture = &b->env.pending_capture[i];
        if (!capture->data || capture->len > BRIDGE_BYTES / 2 - image_bytes) {
            result = "error: captured images exceed ACP MCP delivery limit (4 MiB total)";
            is_error = true;
            break;
        }
        image_bytes += capture->len;
    }
    subagent_control(b, c, TNY_OPENAI_CONTROL_SUBAGENT_END, result, !is_error);
    emit(b, c, TNY_EV_TOOL_END, result, !is_error);
    tny_openai_control_response response =
        control(b, c, TNY_OPENAI_CONTROL_POST_TOOL, result, !is_error);
    if (tny_alloc_scope_failed()) {
        response_free(&response);
        return -1;
    }
    const char *effective = response.result_replaced && response.result ? response.result : result;
    bool failed = response.result_replaced ? response.result_is_error : is_error;

    buf_t body = {0};
    buf_appendf(&body, "{\"isError\":%s,\"content\":[{\"type\":\"text\",\"text\":",
                failed ? "true" : "false");
    jescape(&body, effective);
    buf_appends(&body, "}");
    /* read_image captures bytes through the same queue as native tools. Deliver
     * those bytes in the MCP result, without re-reading a possibly changed path. */
    for (int i = 0; i < b->env.n_pending_images; i++) {
        tools_pending_capture *capture = &b->env.pending_capture[i];
        if (!failed && capture->data && capture->len <= BRIDGE_BYTES / 2) {
            buf_t encoded_buf = {0};
            b64_encode(capture->data, capture->len, &encoded_buf);
            char *encoded = buf_detach(&encoded_buf);
            if (encoded) {
                buf_appends(&body, ",{\"type\":\"image\",\"mimeType\":");
                jescape(&body, capture->mime);
                buf_appends(&body, ",\"data\":");
                jescape(&body, encoded);
                buf_appends(&body, "}");
                free(encoded);
            } else body.oom = true;
        }
    }
    tools_discard_pending_images(&b->env);
    buf_appends(&body, "]}");
    int rc;
    if (buf_oom(&body) || tny_alloc_scope_failed()) {
        buf_free(&body);
        response_free(&response);
        return -1;
    }
    if (body.len > BRIDGE_BYTES) {
        effective = "error: ACP MCP tool result exceeds 8 MiB delivery limit";
        rc = queue_reply(c, c->id,
                         "{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":"
                         "\"error: ACP MCP tool result exceeds 8 MiB delivery limit\"}]}",
                         false);
    } else rc = queue_reply(c, c->id, body.data, false);
    if (rc == 0 && c->recorded && b->env.session)
        session_add_tool_result(b->env.session, c->tool_id, effective);
    buf_free(&body);
    bool stop = response.stop;
    response_free(&response);
    response = control(b, c, TNY_OPENAI_CONTROL_TOOL_BATCH, NULL, true);
    stop = stop || response.stop;
    response_free(&response);
    pending_clear(c);
    if (stop) {
        b->active = false;
        b->stop_requested = true;
    }
    return rc;
}

static int execute(tny_acp_bridge *b, bridge_client *c) {
    if (tny_alloc_scope_failed()) return -1;
    if (!b->active || (b->env.cancelled && b->env.cancelled(b->env.cancelled_ud)))
        return complete(b, c, "error: ACP tool call cancelled", true);
    /* Human approval (either a parked event or a blocking prompt) consumes no
     * execution budget. Every approved call starts its deadline here. */
    c->deadline = monotonic_ms() + BRIDGE_TIMEOUT_MS;
    emit(b, c, TNY_EV_TOOL_START, tools_call_label(&c->call) ? tools_call_label(&c->call) : c->args,
         true);
    if (tny_alloc_scope_failed()) return -1;
    subagent_control(b, c, TNY_OPENAI_CONTROL_SUBAGENT_START, NULL, false);
    if (b->stop_requested) return complete(b, c, "error: extension stopped subagent", true);
    if (tny_alloc_scope_failed()) return -1;
    char *result = tools_call_execute(&b->env, &c->call);
    if (!result && tools_call_pending(&c->call)) return 0;
    int rc = complete(b, c, result ? result : "error: tool returned no result",
                      !result || str_starts(result, "error:"));
    free(result);
    return rc;
}

static int deny(tny_acp_bridge *b, bridge_client *c) {
    b->env.perm_blocked = true;
    return complete(b, c, "error: permission denied", true);
}

static int call_tool(tny_acp_bridge *b, bridge_client *c, const char *id, yyjson_val *params) {
    if (!b->active || !b->env.session || !b->env.perm)
        return rpc_error(c, id, -32000, "tny tool calls require an active owning turn");
    if (c->pending) return rpc_error(c, id, -32000, "one outstanding tool call per connection");
    size_t name_len = 0;
    const char *name = jget_strn(params, "name", &name_len);
    yyjson_val *args = jget(params, "arguments");
    if (!name || !*name || (args && !yyjson_is_obj(args)))
        return rpc_error(c, id, -32602, "expected tool name and object arguments");
    if (name_len != strlen("run_code") || strcmp(name, "run_code") != 0)
        return rpc_error(c, id, -32602, "only run_code is exposed; use tools.call inside Lua");
    c->id = xstrdup(id);
    c->original_args = args ? jwrite_val(args) : xstrdup("{}");
    c->args = xstrdup(c->original_args);
    c->call.name = xstrdup(name);
    if (!c->id || !c->original_args || !c->args || !c->call.name) return -1;
    c->pending = true;
    snprintf(c->tool_id, sizeof c->tool_id, "tny-acp-%llu", ++b->sequence);
    tny_openai_control_response pre = control(b, c, TNY_OPENAI_CONTROL_PRE_TOOL, NULL, true);
    if (pre.arguments_json) {
        free(c->args);
        c->args = xstrdup(pre.arguments_json);
    }
    if (!c->args || tny_alloc_scope_failed()) {
        response_free(&pre);
        return -1;
    }
    if (pre.stop || pre.deny) {
        bool stop = pre.stop;
        response_free(&pre);
        int rc = complete(b, c, "error: extension refused tool call", true);
        if (stop) {
            b->active = false;
            b->stop_requested = true;
        }
        return rc;
    }
    response_free(&pre);
    free(c->call.name);
    memset(&c->call, 0, sizeof c->call);
    if (tools_call_prepare(&b->env, name, c->args, &c->call) != 0)
        return complete(b, c, c->call.error ? c->call.error : "error: invalid tool arguments",
                        true);
    buf_t transcript = {0};
    buf_appends(&transcript, "[{\"id\":");
    jescape(&transcript, c->tool_id);
    buf_appends(&transcript, ",\"type\":\"function\",\"function\":{\"name\":");
    jescape(&transcript, c->call.name);
    buf_appends(&transcript, ",\"arguments\":");
    jescape(&transcript, c->args);
    buf_appends(&transcript, "}}]");
    if (buf_oom(&transcript)) {
        buf_free(&transcript);
        return -1;
    }
    session_add_assistant(b->env.session, NULL, transcript.data);
    buf_free(&transcript);
    c->recorded = true;
    if (tny_alloc_scope_failed()) return -1;
    if (session_save(b->env.session) != 0)
        return complete(b, c, "error: could not persist admitted tool call", true);
    if (tny_alloc_scope_failed()) return -1;
    if (c->call.verdict == PERM_DENY) return deny(b, c);
    if (c->call.verdict == PERM_ALLOW) return execute(b, c);
    tny_openai_control_response permission =
        control(b, c, TNY_OPENAI_CONTROL_PERMISSION, NULL, true);
    if (tny_alloc_scope_failed()) {
        response_free(&permission);
        return -1;
    }
    bool stop = permission.stop;
    tny_openai_permission_decision decision = permission.permission;
    response_free(&permission);
    if (stop) {
        b->stop_requested = true;
        return complete(b, c, "error: extension stopped tool call", true);
    }
    if (decision == TNY_OPENAI_PERMISSION_DENY) return deny(b, c);
    if (decision == TNY_OPENAI_PERMISSION_ALLOW_ONCE) return execute(b, c);
    if (b->env.prompt) {
        tny_perm_decision d = b->env.prompt(c->call.name, c->call.summary, b->env.prompt_ud);
        if (d == TNY_PERM_DECISION_DENY) return deny(b, c);
        if (d == TNY_PERM_DECISION_ALLOW_ALWAYS) tools_call_grant(&b->env, &c->call);
        return execute(b, c);
    }
    c->permission = true;
    emit(b, c, TNY_EV_PERMISSION, c->call.summary, false);
    return 0;
}

static int list_tools(tny_acp_bridge *b, bridge_client *c, const char *id) {
    /* Catalog discovery (tny models) needs metadata before a runtime session
     * is bound. tools/call separately requires an active owning session. */
    char *schema = tools_schema_json(&b->env);
    yyjson_doc *doc = schema ? jparse(schema, strlen(schema)) : NULL;
    free(schema);
    if (!doc) return rpc_error(c, id, -32603, "could not build tools schema");
    buf_t body = {0};
    buf_appends(&body, "{\"tools\":[");
    size_t idx, max;
    yyjson_val *item;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_arr_foreach(root, idx, max, item) {
        yyjson_val *fn = jget(item, "function");
        if (idx) buf_appends(&body, ",");
        buf_appends(&body, "{\"name\":");
        jescape(&body, jget_str(fn, "name"));
        buf_appends(&body, ",\"description\":");
        jescape(&body, jget_str(fn, "description"));
        buf_appends(&body, ",\"inputSchema\":");
        char *parameters = jwrite_val(jget(fn, "parameters"));
        if (!parameters) {
            buf_free(&body);
            yyjson_doc_free(doc);
            return -1;
        }
        buf_appends(&body, parameters);
        free(parameters);
        buf_appends(&body, "}");
    }
    buf_appends(&body, "]}");
    int rc = buf_oom(&body) || tny_alloc_scope_failed() ? -1 : queue_reply(c, id, body.data, false);
    buf_free(&body);
    yyjson_doc_free(doc);
    return rc;
}

static int message(tny_acp_bridge *b, bridge_client *c, const char *line, size_t len) {
    yyjson_doc *doc = jparse(line, len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *method = jget_str(root, "method");
    const char *version = jget_str(root, "jsonrpc");
    yyjson_val *id_val = jget(root, "id");
    if (!root || !method || !version || strcmp(version, "2.0") != 0 ||
        (id_val && !yyjson_is_str(id_val) && !yyjson_is_int(id_val))) {
        yyjson_doc_free(doc);
        return -1;
    }
    char *id = id_val ? jwrite_val(id_val) : NULL;
    if (id_val && !id) {
        yyjson_doc_free(doc);
        return -1;
    }
    yyjson_val *params = jget(root, "params");
    int rc = 0;
    if (!id) {
        if (strcmp(method, "notifications/cancelled") == 0 && c->pending) {
            char *cancel_id = jwrite_val(jget(params, "requestId"));
            if (cancel_id && strcmp(cancel_id, c->id) == 0)
                rc = complete(b, c, "error: MCP tool call cancelled", true);
            free(cancel_id);
        }
    } else if (strcmp(method, "initialize") == 0) {
        c->initialized = true;
        rc = queue_reply(c, id,
                         "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
                         "\"serverInfo\":{\"name\":\"tny\",\"version\":\"1\"}}",
                         false);
    } else if (!c->initialized) rc = rpc_error(c, id, -32000, "initialize first");
    else if (strcmp(method, "ping") == 0) rc = queue_reply(c, id, "{}", false);
    else if (strcmp(method, "tools/list") == 0) rc = list_tools(b, c, id);
    else if (strcmp(method, "tools/call") == 0) rc = call_tool(b, c, id, params);
    else rc = rpc_error(c, id, -32601, "method not found");
    free(id);
    yyjson_doc_free(doc);
    return rc;
}

static int drain_client(tny_acp_bridge *b, bridge_client *c) {
    char data[16384];
    /* Bound work per event-loop tick even if a peer floods complete frames. */
    for (int iteration = 0; iteration < 32; iteration++) {
        char *newline = c->input.data ? memchr(c->input.data, '\n', c->input.len) : NULL;
        if (newline) {
            size_t len = (size_t)(newline - c->input.data);
            if (len && message(b, c, c->input.data, len) != 0) return -1;
            memmove(c->input.data, newline + 1, c->input.len - len - 1);
            c->input.len -= len + 1;
            c->input.data[c->input.len] = 0;
            continue;
        }
        if (c->read_closed) break;
        ssize_t n = read(c->fd, data, sizeof data);
        if (n == 0) {
            c->read_closed = true;
            if (c->input.len) return -1;
            break;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        if ((size_t)n > BRIDGE_BYTES - c->input.len) return -1;
        buf_append(&c->input, data, (size_t)n);
        if (buf_oom(&c->input)) return -1;
    }
    if (c->pending && !c->permission) {
        char *result = NULL;
        bool is_error = false;
        int state = tools_call_take_async(&c->call, &result, &is_error);
        if (state != 0) {
            char *bounded = result ? tool_bound_result(&b->env, result, strlen(result)) : NULL;
            int rc = complete(b, c, bounded ? bounded : "error: custom tool invalidated",
                              state < 0 || is_error);
            free(bounded);
            free(result);
            if (rc != 0) return rc;
        }
    }
    if (c->pending && !c->permission && c->deadline && monotonic_ms() >= c->deadline)
        if (complete(b, c, "error: ACP tool call timed out", true) != 0) return -1;
    if (c->output.len) {
        ssize_t n = socket_write(c->fd, c->output.data, c->output.len);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
        if (n > 0) {
            memmove(c->output.data, c->output.data + n, c->output.len - (size_t)n);
            c->output.len -= (size_t)n;
            c->output.data[c->output.len] = 0;
        }
    }
    return c->read_closed && !c->pending && !c->output.len ? -1 : 0;
}

tny_acp_bridge *tny_acp_bridge_new(tny_ctx *ctx, char *err, size_t len) {
    tny_acp_bridge *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->listener = -1;
    b->env.ctx = ctx;
    for (int i = 0; i < BRIDGE_CLIENTS; i++) b->clients[i].fd = -1;
    const char *configured = ctx->library_mode ? getenv("TNY_ACP_BRIDGE_EXECUTABLE") : NULL;
    char *executable =
        ctx->library_mode ? (configured ? xstrdup(configured) : NULL) : tny_process_self_path();
    if (!executable || executable[0] != '/' || access(executable, X_OK) != 0) {
        snprintf(err, len,
                 "ACP MCP bridge needs an absolute executable tny path; embedders set "
                 "TNY_ACP_BRIDGE_EXECUTABLE");
        free(executable);
        tny_acp_bridge_destroy(b);
        return NULL;
    }
    char directory[] = "/tmp/tny-acp-XXXXXX";
    if (!mkdtemp(directory)) goto fail;
    b->directory = xstrdup(directory);
    if (!b->directory) {
        rmdir(directory);
        goto fail;
    }
    b->path = path_join(directory, "mcp.sock");
    if (!b->path) goto fail;
    b->listener = unix_listen(b->path);
    if (b->listener < 0) goto fail;
    (void)fcntl(b->listener, F_SETFD, FD_CLOEXEC);
    buf_t servers = {0};
    buf_appends(&servers, "[{\"name\":\"tny\",\"command\":");
    jescape(&servers, executable);
    buf_appends(&servers, ",\"args\":[\"--acp-mcp-bridge\",");
    jescape(&servers, b->path);
    buf_appends(&servers, "],\"env\":[]}]");
    b->servers = buf_detach(&servers);
    if (!b->servers) goto fail;
    free(executable);
    return b;
fail:
    snprintf(err, len, "could not create private ACP MCP bridge");
    free(executable);
    tny_acp_bridge_destroy(b);
    return NULL;
}

void tny_acp_bridge_bind(tny_acp_bridge *b, const tools_env *env, tny_openai_control_cb cb,
                         void *ud) {
    if (!b || !env) return;
    b->env = *env;
    b->control = cb;
    b->control_ud = ud;
    b->env.execution_control = cb ? execution_control : NULL;
    b->env.execution_control_ud = b;
}
const char *tny_acp_bridge_servers_json(const tny_acp_bridge *b) { return b ? b->servers : "[]"; }
void tny_acp_bridge_begin_turn(tny_acp_bridge *b, tny_backend_event_cb cb, void *ud) {
    if (!b) return;
    b->env.ev_cb = cb;
    b->env.ev_ud = ud;
    b->env.perm_blocked = false;
    b->active = true;
    b->stop_requested = false;
}
int tny_acp_bridge_prepare_prompt(tny_acp_bridge *b, buf_t *context, char *err, size_t len) {
    if (!b || !b->env.session) return -1;
    b->forwarded_context = 0;
    if (tny_swarm_activate(&b->env, err, len) != 0 || tny_team_deliver(&b->env, err, len) != 0)
        return -1;
    yyjson_mut_val *pending =
        yyjson_mut_obj_get(yyjson_mut_doc_get_root(b->env.session->doc), "acp_pending_context");
    size_t i, count, bytes = 0;
    yyjson_mut_val *value;
    if (pending && (!yyjson_mut_is_arr(pending) || yyjson_mut_arr_size(pending) > 256)) goto fail;
    yyjson_mut_arr_foreach(pending, i, count, value) {
        if (!yyjson_mut_is_str(value) || yyjson_mut_get_len(value) > 1024u * 1024u - bytes)
            goto fail;
        bytes += yyjson_mut_get_len(value);
        buf_appends(context, "\n[Durable tny coordination context]\n");
        buf_append(context, yyjson_mut_get_str(value), yyjson_mut_get_len(value));
        buf_appends(context, "\n");
        if (buf_oom(context)) goto fail;
        b->forwarded_context++;
    }
    return 0;
fail:
    snprintf(err, len, "could not prepare bounded ACP coordination context");
    return -1;
}

int tny_acp_bridge_ack_context(tny_acp_bridge *b) {
    if (!b || !b->forwarded_context) return 0;
    tny_session_state *session = b->env.session;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(session->doc);
    yyjson_mut_val *pending = yyjson_mut_obj_get(root, "acp_pending_context");
    yyjson_mut_val *remaining = yyjson_mut_arr(session->doc);
    size_t i, count;
    yyjson_mut_val *value;
    if (!remaining || yyjson_mut_arr_size(pending) < b->forwarded_context) return -1;
    yyjson_mut_arr_foreach(pending, i, count, value) {
        if (i >= b->forwarded_context &&
            !yyjson_mut_arr_add_strcpy(session->doc, remaining, yyjson_mut_get_str(value)))
            return -1;
    }
    yyjson_mut_val *key = yyjson_mut_strcpy(session->doc, "acp_pending_context");
    if (!key || !yyjson_mut_obj_put(root, key, remaining)) return -1;
    if (session_save(session) != 0) {
        (void)yyjson_mut_obj_put(root, key, pending);
        return -1;
    }
    b->forwarded_context = 0;
    return 0;
}

void tny_acp_bridge_end_turn(tny_acp_bridge *b) {
    if (!b) return;
    b->active = false;
    for (int i = 0; i < BRIDGE_CLIENTS; i++) {
        bridge_client *c = &b->clients[i];
        if (c->pending && complete(b, c, "error: owning ACP turn ended", true) != 0)
            client_close(c);
    }
    tools_discard_pending_images(&b->env);
}
void tny_acp_bridge_abort(tny_acp_bridge *b) {
    if (!b) return;
    b->active = false;
    for (int i = 0; i < BRIDGE_CLIENTS; i++) pending_clear(&b->clients[i]);
    tools_discard_pending_images(&b->env);
}
const char *tny_acp_bridge_directory(const tny_acp_bridge *b) { return b ? b->directory : NULL; }
void tny_acp_bridge_cancel(tny_acp_bridge *b) { tny_acp_bridge_end_turn(b); }
bool tny_acp_bridge_stop_requested(const tny_acp_bridge *b) { return b && b->stop_requested; }
bool tny_acp_bridge_permission_blocked(const tny_acp_bridge *b) { return b && b->env.perm_blocked; }
bool tny_acp_bridge_respond_permission(tny_acp_bridge *b, const char *id, tny_perm_decision d) {
    if (!b || !id) return false;
    for (int i = 0; i < BRIDGE_CLIENTS; i++) {
        bridge_client *c = &b->clients[i];
        if (!c->pending || !c->permission || strcmp(id, c->tool_id) != 0) continue;
        c->permission = false;
        if (d == TNY_PERM_DECISION_ALLOW_ALWAYS) tools_call_grant(&b->env, &c->call);
        int rc = d == TNY_PERM_DECISION_DENY ? deny(b, c) : execute(b, c);
        if (rc != 0) client_close(c);
        return true;
    }
    return false;
}
int tny_acp_bridge_pollfds(tny_acp_bridge *b, struct pollfd *fds, int max) {
    if (!b || max < 1) return 0;
    int n = 0;
    fds[n++] = (struct pollfd){.fd = b->listener, .events = POLLIN};
    for (int i = 0; i < BRIDGE_CLIENTS && n < max; i++) {
        bridge_client *c = &b->clients[i];
        if (c->fd >= 0)
            fds[n++] = (struct pollfd){
                .fd = c->fd,
                .events = (short)((c->read_closed ? 0 : POLLIN) | (c->output.len ? POLLOUT : 0))};
    }
    int wake = custom_tools_wake_fd(b->env.ctx->custom_tools);
    if (wake >= 0 && n < max) fds[n++] = (struct pollfd){.fd = wake, .events = POLLIN};
    return n;
}
int tny_acp_bridge_dispatch(tny_acp_bridge *b) {
    if (!b) return 0;
    if (tny_alloc_scope_failed()) {
        tny_acp_bridge_abort(b);
        return -1;
    }
    int listener = b->listener;
    for (int attempt = 0; listener >= 0 && attempt < BRIDGE_CLIENTS; attempt++) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) break;
        if (set_nonblock(fd, true) != 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
            close(fd);
            continue;
        }
        int slot = 0;
        while (slot < BRIDGE_CLIENTS && b->clients[slot].fd >= 0) slot++;
        if (slot == BRIDGE_CLIENTS) close(fd);
        else b->clients[slot].fd = fd;
    }
    custom_tools_wake_drain(b->env.ctx->custom_tools);
    for (int i = 0; i < BRIDGE_CLIENTS; i++)
        if (b->clients[i].fd >= 0 && drain_client(b, &b->clients[i]) != 0)
            client_close(&b->clients[i]);
    if (tny_alloc_scope_failed()) {
        tny_alloc_provider_failed();
        tny_acp_bridge_abort(b);
        return -1;
    }
    return 0;
}
void tny_acp_bridge_destroy(tny_acp_bridge *b) {
    if (!b) return;
    for (int i = 0; i < BRIDGE_CLIENTS; i++) client_close(&b->clients[i]);
    tools_discard_pending_images(&b->env);
    if (b->listener >= 0) close(b->listener);
    if (b->path) unlink(b->path);
    if (b->directory) rmdir(b->directory);
    free(b->path);
    free(b->directory);
    free(b->servers);
    free(b);
}

int tny_acp_bridge_relay_main(const char *path) {
    int fd = unix_connect(path);
    if (fd < 0) return 1;
    if (set_nonblock(STDIN_FILENO, true) != 0 || set_nonblock(STDOUT_FILENO, true) != 0) {
        close(fd);
        return 1;
    }
    buf_t to_server = {0}, to_stdout = {0};
    int rc = 0;
    bool done = false, stdin_eof = false, socket_eof = false, write_closed = false;
    while (!done) {
        struct pollfd fds[3] = {
            {.fd = stdin_eof ? -1 : STDIN_FILENO,
             .events = to_server.len < BRIDGE_BYTES ? POLLIN : 0},
            {.fd = fd,
             .events = (short)((!socket_eof && to_stdout.len < BRIDGE_BYTES ? POLLIN : 0) |
                               (to_server.len ? POLLOUT : 0))},
            {.fd = STDOUT_FILENO, .events = to_stdout.len ? POLLOUT : 0}};
        if (tny_poll(fds, 3, 1000) < 0) {
            if (errno == EINTR) continue;
            rc = 1;
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            buf_t *dest = i == 0 ? &to_server : &to_stdout;
            char bytes[16384];
            size_t capacity = BRIDGE_BYTES - dest->len;
            if (capacity > sizeof bytes) capacity = sizeof bytes;
            if (!capacity) continue;
            ssize_t n = read(fds[i].fd, bytes, capacity);
            if (n > 0) buf_append(dest, bytes, (size_t)n);
            else if (n == 0) {
                if (i == 0) stdin_eof = true;
                else socket_eof = true;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                rc = 1;
                done = true;
            }
        }
        if (stdin_eof && !to_server.len && !write_closed) {
            (void)shutdown(fd, SHUT_WR);
            write_closed = true;
        }
        if (socket_eof && !to_stdout.len) {
            done = true;
            if (to_server.len) rc = 1;
        }
        for (int i = 1; i < 3; i++) {
            if (!(fds[i].revents & POLLOUT)) continue;
            buf_t *source = i == 1 ? &to_server : &to_stdout;
            if (!source->len) continue;
            if (!source->data) {
                rc = 1;
                done = true;
                break;
            }
            ssize_t n = i == 1 ? socket_write(fd, source->data, source->len)
                               : write(STDOUT_FILENO, source->data, source->len);
            if (n > 0 && (size_t)n > source->len) {
                rc = 1;
                done = true;
                break;
            }
            if (n > 0) {
                memmove(source->data, source->data + n, source->len - (size_t)n);
                source->len -= (size_t)n;
                source->data[source->len] = 0;
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                rc = 1;
                done = true;
            }
        }
    }
    buf_free(&to_server);
    buf_free(&to_stdout);
    close(fd);
    return rc;
}
