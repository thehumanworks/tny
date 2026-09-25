/* Harness/client and private execution server. The client handles decisions and
 * observations only; all tool preparation and effects occur in the server. */
#include "core/execution.h"
#include "core/checkpoint.h"
#include "core/code_runtime.h"
#include "core/execution_control.h"
#include "core/execution_protocol.h"
#include "core/execution_state.h"
#include "mcp/mcp.h"
#include "util/execution_host.h"
#include "util/util.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SETTLEMENT_MS 1000
#define TEARDOWN_MS   10000

typedef struct {
    tools_env env;
    int fd;
    int64_t deadline;
    int64_t settlement_deadline;
    uint64_t next_id;
    unsigned calls;
    const char *cell_id;
    bool failed;
    bool stopped;
    bool finished;
    bool can_prompt;
    bool can_ask;
    bool can_control;
    bool preview_ready;
    yyjson_doc *baseline;
} execution_server;

static yyjson_mut_doc *object_doc(void) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d) yyjson_mut_doc_set_root(d, yyjson_mut_obj(d));
    return d;
}

static char *message_from_doc(uint64_t id, tny_exec_kind kind, yyjson_mut_doc *d) {
    yyjson_doc *copy = d ? yyjson_mut_doc_imut_copy(d, NULL) : NULL;
    char *out = copy ? tny_exec_message(id, kind, yyjson_doc_get_root(copy)) : NULL;
    yyjson_doc_free(copy);
    return out;
}

static bool server_cancelled(void *ud) {
    execution_server *s = ud;
    return s->failed || s->stopped || s->finished || s->env.perm_blocked ||
           monotonic_ms() >= s->deadline || tny_exec_host_disconnected(s->fd);
}

/* Every reverse message is a request with an ack. There are no notifications,
 * uncorrelated events, or executable tool RPCs on the harness endpoint. */
static bool server_transport_cancelled(void *ud) {
    execution_server *s = ud;
    return s->failed ||
           monotonic_ms() >= (s->settlement_deadline ? s->settlement_deadline : s->deadline) ||
           tny_exec_host_disconnected(s->fd);
}

static bool server_human_cancelled(void *ud) {
    execution_server *s = ud;
    return s->failed || tny_exec_host_disconnected(s->fd);
}

static yyjson_doc *server_rpc(execution_server *s, tny_exec_kind kind, yyjson_mut_doc *params) {
    bool human = kind == TNY_EXEC_PROMPT || kind == TNY_EXEC_ASK;
    int64_t started = monotonic_ms();
    int64_t deadline = human                    ? started + 300000
                       : s->settlement_deadline ? s->settlement_deadline
                                                : s->deadline;
    tny_exec_cancel_fn cancelled = human ? server_human_cancelled : server_transport_cancelled;
    uint64_t id = s->next_id++;
    char *message = message_from_doc(id, kind, params);
    if (!message || s->failed || tny_exec_host_send(s->fd, message, deadline, cancelled, s)) {
        secure_free(message);
        s->failed = true;
        return NULL;
    }
    secure_free(message);
    char *line = tny_exec_host_receive(s->fd, deadline, cancelled, s);
    if (human) s->deadline += monotonic_ms() - started;
    yyjson_doc *d = line ? jparse(line, strlen(line)) : NULL;
    secure_free(line);
    uint64_t got = 0;
    yyjson_val *body = NULL;
    tny_exec_kind incoming = tny_exec_envelope(d ? yyjson_doc_get_root(d) : NULL, &got, &body);
    if (!tny_exec_protocol_admit(TNY_EXEC_WAIT_REPLY, incoming, got, id)) {
        yyjson_doc_free(d);
        s->failed = true;
        return NULL;
    }
    return d;
}

static yyjson_val *rpc_result(yyjson_doc *d) {
    return d ? jget(yyjson_doc_get_root(d), "result") : NULL;
}

static void server_control(execution_server *s, const tny_openai_control_request *request,
                           tny_openai_control_response *response) {
    memset(response, 0, sizeof *response);
    if (!s->can_control) return;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *body = d ? tny_execution_control_encode_request(d, request) : NULL;
    if (d) yyjson_mut_doc_set_root(d, body);
    yyjson_doc *reply = body ? server_rpc(s, TNY_EXEC_CONTROL, d) : NULL;
    if (!reply || !tny_execution_control_decode_response(rpc_result(reply), response)) {
        s->failed = true;
        response->deny = true;
    }
    if (response->stop) {
        s->stopped = true;
        /* Shorten authority immediately. The Lua budget check on callback return
         * (and every instruction hook) now unwinds even pure Lua loops. */
        s->deadline = 0;
    }
    yyjson_doc_free(reply);
    yyjson_mut_doc_free(d);
}

static tny_perm_decision server_prompt(const char *tool, const char *summary, void *ud) {
    execution_server *s = ud;
    if (!s->can_prompt) return TNY_PERM_DECISION_DENY;
    yyjson_mut_doc *d = object_doc();
    yyjson_mut_val *r = d ? yyjson_mut_doc_get_root(d) : NULL;
    char id[80];
    snprintf(id, sizeof id, "code-%s-perm-%llu", s->cell_id, (unsigned long long)s->next_id);
    bool ok = r && yyjson_mut_obj_add_strcpy(d, r, "tool", tool) &&
              yyjson_mut_obj_add_strcpy(d, r, "summary", summary) &&
              yyjson_mut_obj_add_strcpy(d, r, "id", id);
    yyjson_doc *reply = ok ? server_rpc(s, TNY_EXEC_PROMPT, d) : NULL;
    yyjson_val *v = jget(rpc_result(reply), "decision");
    tny_perm_decision result = TNY_PERM_DECISION_DENY;
    if (yyjson_is_uint(v) && yyjson_get_uint(v) <= TNY_PERM_DECISION_DENY)
        result = (tny_perm_decision)yyjson_get_uint(v);
    else s->failed = true;
    yyjson_doc_free(reply);
    yyjson_mut_doc_free(d);
    return result;
}

static char *server_ask(const char *question, void *ud) {
    execution_server *s = ud;
    yyjson_mut_doc *d = object_doc();
    yyjson_mut_val *r = d ? yyjson_mut_doc_get_root(d) : NULL;
    bool ok = r && yyjson_mut_obj_add_strcpy(d, r, "question", question);
    yyjson_doc *reply = ok ? server_rpc(s, TNY_EXEC_ASK, d) : NULL;
    const char *text = jget_str(rpc_result(reply), "answer");
    char *answer = text ? xstrdup(text) : NULL;
    if (!reply) s->failed = true;
    yyjson_doc_free(reply);
    yyjson_mut_doc_free(d);
    return answer;
}

static void server_event(const tny_backend_event *event, void *ud) {
    execution_server *s = ud;
    if (s->failed || s->stopped) return;
    /* Tool callbacks may report observations, never terminal/provider events. */
    if (event->kind != TNY_EV_TOOL_START && event->kind != TNY_EV_TOOL_END &&
        event->kind != TNY_EV_STATUS && event->kind != TNY_EV_PLAN &&
        event->kind != TNY_EV_TOOL_PROGRESS) {
        s->failed = true;
        return;
    }
    yyjson_mut_doc *d = object_doc();
    yyjson_mut_val *r = d ? yyjson_mut_doc_get_root(d) : NULL;
    bool ok = r && yyjson_mut_obj_add_int(d, r, "kind", event->kind) &&
              yyjson_mut_obj_add_bool(d, r, "ok", event->tool_ok);
    const char *names[] = {"text", "tool", "id", "detail"};
    const char *values[] = {event->text, event->tool_name, event->tool_id, event->tool_detail};
    for (size_t i = 0; i < 4 && ok; i++) {
        yyjson_mut_val *v = !values[i] ? yyjson_mut_null(d)
                            : i == 0   ? yyjson_mut_strncpy(d, values[i], event->text_len)
                                       : yyjson_mut_strcpy(d, values[i]);
        ok = v && yyjson_mut_obj_add_val(d, r, names[i], v);
    }
    yyjson_doc *reply = ok ? server_rpc(s, TNY_EXEC_EVENT, d) : NULL;
    if (!reply) s->failed = true;
    yyjson_doc_free(reply);
    yyjson_mut_doc_free(d);
}

static bool server_state(execution_server *s) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *r =
        d ? tny_execution_state_encode(d, &s->env,
                                       s->baseline ? yyjson_doc_get_root(s->baseline) : NULL, false)
          : NULL;
    if (r && !yyjson_mut_obj_add_bool(d, r, "finished", s->finished)) r = NULL;
    if (d) yyjson_mut_doc_set_root(d, r);
    yyjson_doc *reply = r ? server_rpc(s, TNY_EXEC_STATE, d) : NULL;
    bool ok = reply && jget_bool(rpc_result(reply), "ok", false);
    if (ok) {
        s->env.learning_fact = (tools_learning_fact){0};
        s->env.reserved_image_count = (int)jget_int(rpc_result(reply), "pending_count", 0);
        s->env.reserved_image_bytes = (size_t)jget_int(rpc_result(reply), "pending_bytes", 0);
    }
    yyjson_doc_free(reply);
    yyjson_mut_doc_free(d);
    if (!ok) {
        s->failed = true;
        return false;
    }
    yyjson_doc_free(s->baseline);
    s->baseline = s->env.session ? yyjson_mut_doc_imut_copy(s->env.session->doc, NULL) : NULL;
    tools_discard_pending_images(&s->env);
    if (s->env.session && !s->baseline) s->failed = true;
    return !s->failed;
}

/* Session durability belongs to the owner even when a nested operation must
 * commit context before acknowledging a mailbox/job handoff. */
static int server_save(void *ud) { return server_state(ud) ? 0 : -1; }

static tny_image_preview_status server_preview(void *ud, const tny_image_preview_identity *id,
                                               tny_image_preview_result *result) {
    execution_server *s = ud;
    const char *code = NULL;
    char error[256] = "";
    if (!tny_image_input_auto_preview_allowed(s->env.ctx)) {
        snprintf(result->code, sizeof result->code, "%s", TNY_IMAGE_PREVIEW_CODE_CAPABILITY);
        return TNY_IMAGE_PREVIEW_UNSUPPORTED;
    }
    if (!s->preview_ready || server_cancelled(s)) {
        snprintf(result->code, sizeof result->code, "%s", TNY_IMAGE_PREVIEW_CODE_NOT_READY);
        return TNY_IMAGE_PREVIEW_TURN_NOT_READY;
    }
    int rc = tools_queue_image_preview(&s->env, id->path, id->sha256, id->job ? id->job->bytes : 0,
                                       &code, error, sizeof error);
    if (code) snprintf(result->code, sizeof result->code, "%s", code);
    return rc ? TNY_IMAGE_PREVIEW_FAILED : TNY_IMAGE_PREVIEW_QUEUED;
}

static void server_subagent_control(execution_server *s, const tools_call *call, const char *id,
                                    bool finished, const char *result) {
    if (!call->name || strcmp(call->name, "subagent") != 0) return;
    const char *action = jget_str(call->args, "action");
    if (!action || (strcmp(action, "create") != 0 && strcmp(action, "message") != 0)) return;
    const char *child = jget_str(call->args, "id");
    bool ok = result && !str_starts(result, "error:");
    tny_openai_control_request request = {
        .kind = finished ? TNY_OPENAI_CONTROL_SUBAGENT_END : TNY_OPENAI_CONTROL_SUBAGENT_START,
        .subagent_id = child && *child ? child : id,
        .subagent_action = action,
        .subagent_outcome = finished ? (ok ? "done" : "error") : NULL,
        .subagent_ok = ok,
        .result = result};
    tny_openai_control_response response = {0};
    server_control(s, &request, &response);
    tny_execution_control_response_free(&response);
}

static char *server_tool(void *ud, const char *name, const char *args) {
    execution_server *s = ud;
    if (server_cancelled(s)) return tool_err("execution cancelled or deadline exceeded");
    if (++s->calls > TNY_CODE_TOOL_CALLS || !name || strcmp(name, "run_code") == 0)
        return tool_err("recursive code or nested tool limit exceeded");
    char id[64];
    snprintf(id, sizeof id, "code-%s-%u", s->cell_id, s->calls);
    tny_openai_control_request control = {.kind = TNY_OPENAI_CONTROL_PRE_TOOL,
                                          .tool_id = id,
                                          .tool_name = name,
                                          .arguments_json = args,
                                          .original_arguments_json = args};
    tny_openai_control_response response = {0};
    server_control(s, &control, &response);
    char *effective = xstrdup(response.arguments_json ? response.arguments_json : args);
    char *attribution = response.extension;
    char *reason = response.reason;
    response.extension = response.reason = NULL;
    char *result = NULL;
    bool pre_allowed = effective && !s->failed && !response.deny && !response.stop;
    if (!pre_allowed)
        result =
            tool_err("nested tool denied by execution control: %s", reason ? reason : "denied");
    tny_execution_control_response_free(&response);
    tools_call call = {0};
    if (!effective || (!pre_allowed && !result)) {
        s->failed = true;
        goto cleanup;
    }
    if (pre_allowed) {
        yyjson_doc *d = jparse(effective, strlen(effective));
        bool valid = d && tny_exec_json_valid(yyjson_doc_get_root(d));
        yyjson_doc_free(d);
        if (!valid || tools_call_prepare(&s->env, name, effective, &call)) {
            result = call.error ? xstrdup(call.error) : tool_err("nested tool preparation failed");
            if (!result) {
                s->failed = true;
                goto cleanup;
            }
        }
    }
    control.arguments_json = effective;
    if (!result && call.verdict == PERM_DENY) {
        s->env.perm_blocked = true;
        result = tool_err("permission denied for %s", call.name);
    }
    if (!result && call.verdict == PERM_PROMPT) {
        control.kind = TNY_OPENAI_CONTROL_PERMISSION;
        control.permission_summary = call.summary;
        control.permission_options = TNY_PERM_ALLOW_ONCE | TNY_PERM_ALLOW_ALWAYS | TNY_PERM_DENY;
        server_control(s, &control, &response);
        tny_perm_decision decision = TNY_PERM_DECISION_DENY;
        if (!s->failed && !response.stop && !response.deny) {
            if (response.permission == TNY_OPENAI_PERMISSION_ALLOW_ONCE)
                decision = TNY_PERM_DECISION_ALLOW;
            else if (response.permission == TNY_OPENAI_PERMISSION_ABSTAIN && s->can_prompt)
                decision = server_prompt(call.name, call.summary, s);
        }
        if (response.extension) {
            free(attribution);
            attribution = response.extension;
            response.extension = NULL;
        }
        if (response.reason) {
            free(reason);
            reason = response.reason;
            response.reason = NULL;
        }
        tny_execution_control_response_free(&response);
        if (decision == TNY_PERM_DECISION_ALLOW_ALWAYS) tools_call_grant(&s->env, &call);
        else if (decision != TNY_PERM_DECISION_ALLOW) {
            s->env.perm_blocked = true;
            result =
                tool_err("permission required or denied for %s; no approval available", call.name);
        }
    }
    bool began = false;
    if (!result && !server_cancelled(s)) {
        began = true;
        tny_backend_event event = {.kind = TNY_EV_TOOL_START,
                                   .tool_id = id,
                                   .tool_name = call.name,
                                   .tool_detail = effective};
        server_event(&event, s);
        if (!s->failed) server_subagent_control(s, &call, id, false, NULL);
        if (!server_cancelled(s)) result = tools_call_execute(&s->env, &call);
    }
    if (!result) result = tool_err("nested tool interrupted; outcome unknown, do not replay");
    if (!result) {
        s->failed = true;
        goto cleanup;
    }
    /* Push state even when code later fails. Side effects are never rolled back
     * or replayed; ACK precedes post hooks so they observe the committed state. */
    /* This phase only settles the completed call. It never changes the code /
     * tool deadline, and the runtime checks that deadline before resuming Lua. */
    s->settlement_deadline = monotonic_ms() + SETTLEMENT_MS;
    (void)server_state(s);
    if (began) server_subagent_control(s, &call, id, true, result);
    tny_backend_event event = {.kind = TNY_EV_TOOL_END,
                               .tool_id = id,
                               .tool_name = name,
                               .tool_detail = result,
                               .tool_ok = !str_starts(result, "error:")};
    server_event(&event, s);
    control.kind = TNY_OPENAI_CONTROL_POST_TOOL;
    control.result = result;
    control.original_ok = event.tool_ok;
    control.control_extension = attribution;
    control.control_reason = reason;
    server_control(s, &control, &response);
    if (response.result_replaced && response.result) {
        free(result);
        result = response.result;
        response.result = NULL;
    }
    tny_execution_control_response_free(&response);
cleanup:
    s->settlement_deadline = 0;
    tools_call_free(&call);
    free(effective);
    free(attribution);
    free(reason);
    return result;
}

static bool client_cancelled(void *ud) {
    tools_env *env = ud;
    if (env->control_pump) (void)env->control_pump(env->control_pump_ud, 0);
    return env->cancelled && env->cancelled(env->cancelled_ud);
}

static bool client_event(tools_env *env, yyjson_val *body) {
    yyjson_val *k = jget(body, "kind");
    if (!yyjson_is_uint(k) || yyjson_obj_size(body) != 6 || !yyjson_is_bool(jget(body, "ok")))
        return false;
    const char *names[] = {"text", "tool", "id", "detail"};
    for (size_t i = 0; i < 4; i++) {
        yyjson_val *v = jget(body, names[i]);
        if (!v || (!yyjson_is_str(v) && !yyjson_is_null(v))) return false;
    }
    uint64_t kind = yyjson_get_uint(k);
    if (kind != TNY_EV_TOOL_START && kind != TNY_EV_TOOL_END && kind != TNY_EV_STATUS &&
        kind != TNY_EV_PLAN && kind != TNY_EV_TOOL_PROGRESS)
        return false;
    tny_backend_event event = {.kind = (tny_event_kind)kind,
                               .text = jget_str(body, "text"),
                               .tool_name = jget_str(body, "tool"),
                               .tool_id = jget_str(body, "id"),
                               .tool_detail = jget_str(body, "detail"),
                               .tool_ok = jget_bool(body, "ok", false)};
    event.text_len = event.text ? strlen(event.text) : 0;
    if (env->ev_cb) env->ev_cb(&event, env->ev_ud);
    return true;
}

static yyjson_mut_doc *client_callback(tools_env *env, tny_exec_kind kind, yyjson_val *body) {
    yyjson_mut_doc *d = object_doc();
    yyjson_mut_val *r = d ? yyjson_mut_doc_get_root(d) : NULL;
    bool ok = r != NULL;
    if (ok && kind == TNY_EXEC_CONTROL) {
        tny_openai_control_request request;
        tny_openai_control_response response = {0};
        ok = env->execution_control && tny_execution_control_decode_request(body, &request);
        if (ok) {
            if (request.kind == TNY_OPENAI_CONTROL_POST_TOOL && env->execution_observe)
                env->execution_observe(env->execution_observe_ud);
            env->execution_control(&request, &response, env->execution_control_ud);
            r = tny_execution_control_encode_response(d, &response);
            ok = r != NULL;
            yyjson_mut_doc_set_root(d, r);
        }
        tny_execution_control_response_free(&response);
    } else if (ok && kind == TNY_EXEC_PROMPT) {
        const char *tool = jget_str(body, "tool"), *summary = jget_str(body, "summary");
        const char *id = jget_str(body, "id");
        ok = tool && summary && id && yyjson_obj_size(body) == 3;
        if (ok) {
            tny_perm_decision decision = TNY_PERM_DECISION_DENY;
            if (env->prompt) decision = env->prompt(tool, summary, env->prompt_ud);
            ok = yyjson_mut_obj_add_int(d, r, "decision", decision);
        }
    } else if (ok && kind == TNY_EXEC_ASK) {
        const char *question = jget_str(body, "question");
        ok = env->ask_user && question && yyjson_obj_size(body) == 1;
        if (ok) {
            char *answer = env->ask_user(question, env->ask_user_ud);
            yyjson_mut_val *value = answer ? yyjson_mut_strcpy(d, answer) : yyjson_mut_null(d);
            ok = value && yyjson_mut_obj_add_val(d, r, "answer", value);
            free(answer);
        }
    } else if (ok && (kind == TNY_EXEC_EVENT || kind == TNY_EXEC_STATE)) {
        ok = kind == TNY_EXEC_EVENT ? client_event(env, body)
                                    : tny_execution_state_apply(env, body, false);
        if (ok) {
            size_t bytes = 0;
            for (int i = 0; i < env->n_pending_images; i++) bytes += env->pending_capture[i].len;
            ok = yyjson_mut_obj_add_bool(d, r, "ok", true) &&
                 yyjson_mut_obj_add_int(d, r, "pending_count", env->n_pending_images) &&
                 yyjson_mut_obj_add_uint(d, r, "pending_bytes", bytes);
        }
    } else ok = false;
    if (!ok) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    return d;
}

static yyjson_mut_doc *client_snapshot(tools_env *env, yyjson_val *args) {
    yyjson_mut_doc *d = object_doc();
    yyjson_mut_val *r = d ? yyjson_mut_doc_get_root(d) : NULL;
    yyjson_mut_val *context = d ? tny_checkpoint_context(d, env->ctx) : NULL;
    /* Keep extension configuration in the snapshot so server-launched agents
     * inherit it. This server invokes hooks only through its owner RPC; the
     * restored manager remains dormant. No live callback pointer is copied. */
    yyjson_mut_val *state = d ? tny_execution_state_encode(d, env, NULL, true) : NULL;
    bool ok = r && context && state && yyjson_mut_obj_add_val(d, r, "context", context) &&
              yyjson_mut_obj_add_val(d, r, "state", state) &&
              yyjson_mut_obj_add_val(d, r, "arguments", yyjson_val_mut_copy(d, args)) &&
              yyjson_mut_obj_add_bool(d, r, "prompt", env->prompt != NULL) &&
              yyjson_mut_obj_add_bool(d, r, "ask", env->ask_user != NULL) &&
              yyjson_mut_obj_add_bool(d, r, "control", env->execution_control != NULL);
    size_t pending_bytes = 0;
    for (int i = 0; i < env->n_pending_images; i++) pending_bytes += env->pending_capture[i].len;
    if (ok)
        ok = yyjson_mut_obj_add_uint(d, r, "pending_bytes", pending_bytes) &&
             yyjson_mut_obj_add_bool(d, r, "preview_ready",
                                     env->preview_ready && env->preview_ready(env->preview_ud)) &&
             yyjson_mut_obj_add_bool(d, r, "preview_available", env->preview_ready != NULL);
    char *cell_id = gen_id();
    ok = ok && cell_id && yyjson_mut_obj_add_strcpy(d, r, "cell_id", cell_id);
    free(cell_id);
    const char *sock = env->session_sock, *id = env->session_id;
    if (ok)
        ok = yyjson_mut_obj_add_val(d, r, "session_sock",
                                    sock ? yyjson_mut_strcpy(d, sock) : yyjson_mut_null(d)) &&
             yyjson_mut_obj_add_val(d, r, "session_id",
                                    id ? yyjson_mut_strcpy(d, id) : yyjson_mut_null(d));
    if (!ok) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    return d;
}

char *tny_execution_run(tools_env *env, const char *arguments_json) {
    if (!env || !env->ctx || env->execution_server)
        return tool_err("recursive execution server refused");
    if ((env->ctx->library_mode && !env->ctx->prompt_optimisation) || env->ctx->custom_tools ||
        env->ctx->host_services)
        return tool_err(
            "execution server unavailable for embedded host/custom callbacks; no direct fallback");
    yyjson_doc *arguments = arguments_json ? jparse(arguments_json, strlen(arguments_json)) : NULL;
    yyjson_val *args = arguments ? yyjson_doc_get_root(arguments) : NULL;
    const char *code = jget_str(args, "code");
    int64_t timeout = jget_int(args, "timeout_ms", TNY_CODE_DEFAULT_TIMEOUT_MS);
    if (!code || !tny_exec_json_valid(args) || strlen(code) > TNY_CODE_SOURCE_BYTES ||
        timeout < 1 || timeout > TNY_CODE_MAX_TIMEOUT_MS) {
        yyjson_doc_free(arguments);
        return tool_err("invalid run_code arguments");
    }
    yyjson_mut_doc *snapshot = client_snapshot(env, args);
    char *request = snapshot ? message_from_doc(1, TNY_EXEC_EXECUTE, snapshot) : NULL;
    yyjson_mut_doc_free(snapshot);
    yyjson_doc_free(arguments);
    if (!request || strlen(request) > TNY_EXEC_FRAME_MAX) {
        secure_free(request);
        return tool_err("execution snapshot exceeds frame limit or could not be encoded");
    }
    tny_exec_host host = {.fd = -1, .pid = -1};
    int rc = tny_exec_host_start(&host);
    int64_t deadline = monotonic_ms() + timeout + 1000; /* bounded startup/transport slack */
    if (!rc) rc = tny_exec_host_send(host.fd, request, deadline, client_cancelled, env);
    secure_free(request);
    char *output = NULL;
    uint64_t expected = 2;
    bool completed = false;
    bool settling = false;
    bool finished = false;
    while (!rc && !completed) {
        char *line =
            tny_exec_host_receive(host.fd, deadline, settling ? NULL : client_cancelled, env);
        if (!line) {
            rc = errno ? errno : EPROTO;
            break;
        }
        yyjson_doc *d = jparse(line, strlen(line));
        secure_free(line);
        uint64_t id = 0;
        yyjson_val *body = NULL;
        tny_exec_kind kind = tny_exec_envelope(d ? yyjson_doc_get_root(d) : NULL, &id, &body);
        if (!tny_exec_protocol_admit(TNY_EXEC_WAIT_RESULT, kind, id, expected)) rc = EPROTO;
        else if (kind == TNY_EXEC_RESULT) {
            const char *text = jget_str(body, "output");
            if (!text || yyjson_obj_size(body) != 1 || strlen(text) > TNY_CODE_OUTPUT_BYTES + 1024)
                rc = EPROTO;
            else {
                output = xstrdup(text);
                completed = output != NULL;
                if (!output) rc = ENOMEM;
            }
        } else {
            if (finished) {
                yyjson_doc_free(d);
                rc = EPROTO;
                break;
            }
            if (settling) {
                tny_openai_control_request settled_request = {0};
                bool allowed = kind == TNY_EXEC_STATE || kind == TNY_EXEC_EVENT ||
                               (kind == TNY_EXEC_CONTROL &&
                                tny_execution_control_decode_request(body, &settled_request) &&
                                (settled_request.kind == TNY_OPENAI_CONTROL_POST_TOOL ||
                                 settled_request.kind == TNY_OPENAI_CONTROL_SUBAGENT_END));
                if (!allowed) {
                    yyjson_doc_free(d);
                    rc = EPROTO;
                    break;
                }
            }
            if (kind == TNY_EXEC_STATE && jget_bool(body, "finished", false)) {
                /* Lua has returned: only its final ACK/result may follow.
                 * Authority is over; this is a separate, non-executing phase. */
                finished = true;
                deadline = monotonic_ms() + SETTLEMENT_MS;
            }
            int64_t callback_started = monotonic_ms();
            yyjson_mut_doc *reply = client_callback(env, kind, body);
            if (kind == TNY_EXEC_CONTROL && reply &&
                yyjson_mut_get_bool(yyjson_mut_obj_get(yyjson_mut_doc_get_root(reply), "stop"))) {
                settling = true;
                if (deadline > monotonic_ms() + SETTLEMENT_MS)
                    deadline = monotonic_ms() + SETTLEMENT_MS;
            }
            if (!settling && !finished) {
                /* Owner callback time must not consume the client's transport
                 * watchdog. Only human waits extend the server's code budget. */
                int64_t waited = monotonic_ms() - callback_started;
                if (waited > 300000) waited = 300000;
                deadline += waited;
            }
            char *message = reply ? message_from_doc(id, TNY_EXEC_RESULT, reply) : NULL;
            yyjson_mut_doc_free(reply);
            if (!message) rc = EPROTO;
            else if (tny_exec_host_send(host.fd, message, deadline,
                                        settling ? NULL : client_cancelled, env))
                rc = errno ? errno : EPROTO;
            secure_free(message);
            expected++;
        }
        yyjson_doc_free(d);
    }
    if (completed && !rc &&
        tny_exec_host_expect_eof(host.fd, monotonic_ms() + TEARDOWN_MS,
                                 settling ? NULL : client_cancelled, env))
        rc = errno ? errno : EPROTO;
    int cleanup = tny_exec_host_close(&host, completed && !rc);
    if (rc || !completed || cleanup) {
        free(output);
        if (rc == ENOTSUP)
            return tool_err("execution server unavailable on this platform; no direct fallback");
        if (rc == ETIMEDOUT)
            return tool_err("execution server timeout; outcome unknown, not replayed");
        if (rc == ECANCELED) return tool_err("execution cancelled; outcome unknown, not replayed");
        return tool_err("execution server disconnected or failed protocol/cleanup; outcome "
                        "unknown, not replayed");
    }
    return output;
}

int tny_execution_server_main(void) {
    execution_server s = {.fd = tny_exec_host_accept(), .next_id = 2};
    if (s.fd < 0) return 2;
    s.deadline = monotonic_ms() + 10000;
    char *line = tny_exec_host_receive(s.fd, s.deadline, NULL, NULL);
    yyjson_doc *d = line ? jparse(line, strlen(line)) : NULL;
    secure_free(line);
    uint64_t id = 0;
    yyjson_val *body = NULL;
    tny_exec_kind kind = tny_exec_envelope(d ? yyjson_doc_get_root(d) : NULL, &id, &body);
    if (!tny_exec_protocol_admit(TNY_EXEC_WAIT_START, kind, id, 1) || yyjson_obj_size(body) != 12) {
        yyjson_doc_free(d);
        close(s.fd);
        return 2;
    }
    yyjson_val *args = jget(body, "arguments");
    const char *code = jget_str(args, "code");
    int64_t timeout = jget_int(args, "timeout_ms", TNY_CODE_DEFAULT_TIMEOUT_MS);
    if (!code || timeout < 1 || timeout > TNY_CODE_MAX_TIMEOUT_MS ||
        strlen(code) > TNY_CODE_SOURCE_BYTES || !jget_str(body, "cell_id") ||
        strlen(jget_str(body, "cell_id")) != 16 || !yyjson_is_bool(jget(body, "prompt")) ||
        !yyjson_is_bool(jget(body, "ask")) || !yyjson_is_bool(jget(body, "control")) ||
        !yyjson_is_bool(jget(body, "preview_ready")) ||
        !yyjson_is_uint(jget(body, "pending_bytes")) ||
        (jget(args, "timeout_ms") && !yyjson_is_int(jget(args, "timeout_ms")))) {
        yyjson_doc_free(d);
        close(s.fd);
        return 2;
    }
    s.env.ctx = tny_checkpoint_context_restore(jget(body, "context"));
    if (!s.env.ctx || (s.env.ctx->library_mode && !s.env.ctx->prompt_optimisation)) {
        tny_ctx_free(s.env.ctx);
        yyjson_doc_free(d);
        close(s.fd);
        return 2;
    }
    s.env.perm = perm_new(s.env.ctx);
    if (!s.env.perm || !tny_execution_state_apply(&s.env, jget(body, "state"), true)) {
        perm_free(s.env.perm);
        tny_ctx_free(s.env.ctx);
        yyjson_doc_free(d);
        close(s.fd);
        return 2;
    }
    s.deadline = monotonic_ms() + timeout;
    s.cell_id = jget_str(body, "cell_id");
    s.can_prompt = jget_bool(body, "prompt", false);
    s.can_ask = jget_bool(body, "ask", false);
    s.can_control = jget_bool(body, "control", false);
    s.env.execution_server = true;
    s.env.reserved_image_count = (int)jget_int(jget(body, "state"), "pending_count", 0);
    s.env.reserved_image_bytes = (size_t)jget_int(body, "pending_bytes", 0);
    s.env.cancelled = server_cancelled;
    s.env.cancelled_ud = &s;
    s.env.ev_cb = server_event;
    s.env.ev_ud = &s;
    s.env.prompt = s.can_prompt ? server_prompt : NULL;
    s.env.prompt_ud = &s;
    s.env.ask_user = s.can_ask ? server_ask : NULL;
    s.env.ask_user_ud = &s;
    s.preview_ready = jget_bool(body, "preview_ready", false);
    s.env.preview_admit = jget_bool(body, "preview_available", false) ? server_preview : NULL;
    s.env.preview_ud = &s;
    s.env.session_sock = jget_str(body, "session_sock");
    s.env.session_id = jget_str(body, "session_id");
    s.baseline = s.env.session ? yyjson_mut_doc_imut_copy(s.env.session->doc, NULL) : NULL;
    if (s.env.session && !s.baseline) s.failed = true;
    if (s.env.session) {
        s.env.session->execution_save = server_save;
        s.env.session->execution_save_ud = &s;
    }
    char *catalog = !s.failed ? tools_catalog_json(&s.env) : NULL;
    char *output =
        catalog ? tny_code_run_with_deadline(code, &s.deadline, catalog, server_tool, &s) : NULL;
    free(catalog);
    if (!output) output = tool_err("execution runtime allocation failure");
    if (s.stopped || monotonic_ms() >= s.deadline) {
        free(output);
        output = tool_err("execution %s; completed effects retained, uncertain calls not replayed",
                          s.stopped ? "cancelled by owner policy" : "timeout");
    }
    s.finished = true;
    s.settlement_deadline = monotonic_ms() + SETTLEMENT_MS;
    if (!s.failed) (void)server_state(&s);
    yyjson_mut_doc *result = object_doc();
    yyjson_mut_val *r = result ? yyjson_mut_doc_get_root(result) : NULL;
    bool ok = !s.failed && r && yyjson_mut_obj_add_strcpy(result, r, "output", output);
    char *reply = ok ? message_from_doc(1, TNY_EXEC_RESULT, result) : NULL;
    int rc = reply ? tny_exec_host_send(s.fd, reply, s.settlement_deadline, NULL, NULL) : -1;
    /* The client holds the result until EOF and successful reap. Teardown has
     * its own bounded wait and cannot grant more Lua/tool execution. */
    mcp_shutdown_all();
    secure_free(reply);
    yyjson_mut_doc_free(result);
    free(output);
    yyjson_doc_free(s.baseline);
    tools_discard_pending_images(&s.env);
    if (s.env.session) session_close(s.env.session);
    perm_free(s.env.perm);
    tny_ctx_free(s.env.ctx);
    yyjson_doc_free(d);
    close(s.fd);
    return rc ? 1 : 0;
}
