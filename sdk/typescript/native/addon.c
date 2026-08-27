#define _POSIX_C_SOURCE 200809L
#include "addon_internal.h"

#include <stdlib.h>
#include <string.h>

static _Thread_local napi_status build_status = napi_ok;

static void note_status(napi_status status) {
    if (build_status == napi_ok && status != napi_ok) build_status = status;
}

static napi_value create_record(napi_env env) {
    napi_value source, object;
    napi_status status =
        napi_create_string_utf8(env, "({__proto__:null})", NAPI_AUTO_LENGTH, &source);
    if (status == napi_ok) status = napi_run_script(env, source, &object);
    note_status(status);
    return status == napi_ok ? object : NULL;
}

static napi_status set_named(napi_env env, napi_value object, const char *name, napi_value value) {
    napi_property_descriptor property = {
        name, NULL, NULL, NULL, NULL, value, napi_writable | napi_enumerable | napi_configurable,
        NULL};
    napi_status status =
        object && value ? napi_define_properties(env, object, 1u, &property) : napi_invalid_arg;
    note_status(status);
    return status;
}

static napi_value js_string(napi_env env, const char *value) {
    napi_value result;
    const char *text = value ? value : "";
    napi_status status = napi_create_string_utf8(env, text, strlen(text), &result);
    note_status(status);
    return status == napi_ok ? result : NULL;
}

static napi_value js_owned(napi_env env, sdk_owned_bytes value) {
    napi_value result;
    napi_status status =
        value.len > SIZE_MAX
            ? napi_invalid_arg
            : napi_create_string_utf8(env, value.ptr ? value.ptr : "", (size_t)value.len, &result);
    note_status(status);
    return status == napi_ok ? result : NULL;
}

static napi_value js_uint32(napi_env env, uint32_t value) {
    napi_value result;
    napi_status status = napi_create_uint32(env, value, &result);
    note_status(status);
    return status == napi_ok ? result : NULL;
}

static napi_value js_int32(napi_env env, int32_t value) {
    napi_value result;
    napi_status status = napi_create_int32(env, value, &result);
    note_status(status);
    return status == napi_ok ? result : NULL;
}

static napi_value js_bool(napi_env env, int value) {
    napi_value result;
    napi_status status = napi_get_boolean(env, value != 0, &result);
    note_status(status);
    return status == napi_ok ? result : NULL;
}

static napi_value js_big_uint(napi_env env, uint64_t value) {
    napi_value result;
    napi_status status = napi_create_bigint_uint64(env, value, &result);
    note_status(status);
    return status == napi_ok ? result : NULL;
}

static napi_value make_error(napi_env env, int32_t status, const char *message) {
    napi_value error, name;
    napi_value msg = js_string(env, message ? message : "libtny operation failed");
    napi_status result = msg ? napi_create_error(env, NULL, msg, &error) : napi_invalid_arg;
    note_status(result);
    if (result != napi_ok) return NULL;
    name = js_string(env, "TnyError");
    set_named(env, error, "name", name);
    set_named(env, error, "status", js_int32(env, status));
    return error;
}

static napi_value capabilities_to_js(napi_env env, const capability_copy *capabilities) {
    napi_value object;
    object = create_record(env);
    set_named(env, object, "schemaVersion", js_uint32(env, capabilities->schema_version));
    set_named(env, object, "abiVersion", js_uint32(env, capabilities->abi_version));
    set_named(env, object, "providerSelected", js_uint32(env, capabilities->provider_selected));
    set_named(env, object, "providerInitialized", js_bool(env, capabilities->provider_initialized));
    set_named(env, object, "endpointReachability",
              js_uint32(env, capabilities->endpoint_reachability));
    set_named(env, object, "threadingModel", js_uint32(env, capabilities->threading_model));
    set_named(env, object, "cancelModel", js_uint32(env, capabilities->cancel_model));
    set_named(env, object, "providerAvailableMask",
              js_big_uint(env, capabilities->provider_available_mask));
    set_named(env, object, "featureAvailableMask",
              js_big_uint(env, capabilities->feature_available_mask));
    set_named(env, object, "featureEnabledMask",
              js_big_uint(env, capabilities->feature_enabled_mask));
    set_named(env, object, "eventQueueMax", js_uint32(env, capabilities->event_queue_max));
    set_named(env, object, "eventReserved", js_uint32(env, capabilities->event_reserved));
    set_named(env, object, "eventPayloadBytesMax",
              js_big_uint(env, capabilities->event_payload_bytes_max));
    set_named(env, object, "eventReservedBytes",
              js_big_uint(env, capabilities->event_reserved_bytes));
    set_named(env, object, "libraryVersion", js_owned(env, capabilities->library_version));
    set_named(env, object, "platformFamily", js_owned(env, capabilities->platform_family));
    set_named(env, object, "architecture", js_owned(env, capabilities->architecture));
    set_named(env, object, "transport", js_owned(env, capabilities->transport));
    set_named(env, object, "tlsImplementation", js_owned(env, capabilities->tls_implementation));
    set_named(env, object, "linkage", js_owned(env, capabilities->linkage));
    return object;
}

static void complete_on_js(napi_env env, napi_value js_callback, void *context, void *data) {
    command *cmd = (command *)data;
    runtime_state *state = cmd->owner;
    napi_value value = NULL, ignored;
    bool pending = false;
    int reject = cmd->status < 0;
    (void)js_callback;
    (void)context;
    if (!env) {
        if (cmd->destroy_owner) sdk_runtime_destroy(state);
        else sdk_runtime_release(state);
        sdk_free_command(cmd);
        return;
    }
    build_status = napi_ok;
    if (reject) {
        value = make_error(env, cmd->status, cmd->error_message);
    } else {
        switch (cmd->result) {
        case RESULT_RUNTIME: {
            value = create_record(env);
            set_named(env, value, "runtimeId", js_uint32(env, state->id));
            set_named(env, value, "abiVersion", js_uint32(env, cmd->abi_version));
            set_named(env, value, "libraryVersion", js_string(env, cmd->string_result));
            set_named(env, value, "capabilities", capabilities_to_js(env, &cmd->capabilities));
            break;
        }
        case RESULT_CAPABILITIES: value = capabilities_to_js(env, &cmd->capabilities); break;
        case RESULT_SESSION:
            value = create_record(env);
            set_named(env, value, "sessionHandle", js_uint32(env, cmd->session_handle));
            set_named(env, value, "sessionId", js_string(env, cmd->string_result));
            break;
        case RESULT_EVENT:
            value = create_record(env);
            if (cmd->status == TNY_STATUS_DRAINED) {
                set_named(env, value, "done", js_bool(env, 1));
            } else {
                set_named(env, value, "done", js_bool(env, 0));
                set_named(env, value, "value", sdk_event_to_js(env, cmd->event));
            }
            break;
        case RESULT_VOID:
        default: note_status(napi_get_undefined(env, &value)); break;
        }
    }
    (void)napi_is_exception_pending(env, &pending);
    if (pending) (void)napi_get_and_clear_last_exception(env, &ignored);
    if (build_status != napi_ok || pending || !value) {
        build_status = napi_ok;
        value = make_error(env, TNY_STATUS_INTERNAL, "native result construction failed");
        if (value) (void)napi_reject_deferred(env, cmd->deferred, value);
    } else if (reject) {
        (void)napi_reject_deferred(env, cmd->deferred, value);
    } else {
        (void)napi_resolve_deferred(env, cmd->deferred, value);
    }
    (void)napi_unref_threadsafe_function(env, state->tsfn);
    if (cmd->destroy_owner) {
        sdk_runtime_destroy(state);
        sdk_free_command(cmd);
    } else {
        sdk_runtime_release(state);
        sdk_free_command(cmd);
    }
}

static void tsfn_finalize(napi_env env, void *data, void *hint) {
    tsfn_token *token = (tsfn_token *)data;
    runtime_state *state = atomic_exchange(&token->state, NULL);
    (void)env;
    (void)hint;
    if (state) sdk_finalize_env_state(state);
    free(token);
}

static void env_cleanup(void *data) { sdk_cleanup_env((napi_env)data); }

static napi_value rejected_promise(napi_env env, int32_t status, const char *message) {
    napi_deferred deferred;
    napi_value promise;
    (void)napi_create_promise(env, &deferred, &promise);
    (void)napi_reject_deferred(env, deferred, make_error(env, status, message));
    return promise;
}

static napi_value enqueue_existing(napi_env env, runtime_state *state, command *cmd) {
    napi_value promise;
    cmd->owner = state;
    sdk_runtime_retain(state);
    (void)napi_create_promise(env, &cmd->deferred, &promise);
    (void)napi_ref_threadsafe_function(env, state->tsfn);
    if (!sdk_queue_push(state, cmd)) {
        (void)napi_reject_deferred(
            env, cmd->deferred,
            make_error(env, TNY_STATUS_BACKPRESSURE, "native command queue is closed or full"));
        (void)napi_unref_threadsafe_function(env, state->tsfn);
        sdk_runtime_release(state);
        sdk_free_command(cmd);
    }
    return promise;
}

static int32_t cancel_state(runtime_state *state, uint32_t session_handle, tny_error **error) {
    int32_t status = TNY_STATUS_OK;
    pthread_mutex_lock(&state->session_mutex);
    if (!state->session || (session_handle && session_handle != state->session_handle))
        status = TNY_STATUS_BAD_STATE;
    else status = tny_session_cancel(state->session, error);
    pthread_mutex_unlock(&state->session_mutex);
    return status;
}

static napi_value cancel_direct(napi_env env, napi_callback_info info) {
    size_t argc = 2u;
    napi_value argv[2], promise, value;
    napi_deferred deferred;
    uint32_t runtime_id, session_handle;
    runtime_state *state;
    tny_error *error = NULL;
    int32_t status;
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc != 2u ||
        !sdk_arg_uint32(env, argv[0], &runtime_id) ||
        !sdk_arg_uint32(env, argv[1], &session_handle))
        return NULL;
    state = sdk_runtime_acquire(runtime_id, env);
    if (!state) return rejected_promise(env, TNY_STATUS_BAD_STATE, "runtime is closed");
    status = cancel_state(state, session_handle, &error);
    sdk_runtime_release(state);
    if (napi_create_promise(env, &deferred, &promise) != napi_ok) {
        if (error) tny_error_free(error);
        return NULL;
    }
    if (status == TNY_STATUS_OK) {
        if (error) tny_error_free(error);
        if (napi_get_undefined(env, &value) != napi_ok ||
            napi_resolve_deferred(env, deferred, value) != napi_ok)
            return NULL;
    } else {
        char *message = sdk_take_error(status, error);
        value = make_error(env, status, message);
        free(message);
        if (napi_reject_deferred(env, deferred, value) != napi_ok) return NULL;
    }
    return promise;
}

static napi_value create_runtime(napi_env env, napi_callback_info info) {
    size_t argc = 1u;
    napi_value argv[1], name, promise;
    napi_valuetype type;
    runtime_state *state;
    command *cmd;
    tsfn_token *token;
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc != 1u)
        return rejected_promise(env, TNY_STATUS_INVALID_ARGUMENT,
                                "createRuntime requires an options object");
    (void)napi_typeof(env, argv[0], &type);
    if (type != napi_object) {
        (void)napi_throw_type_error(env, NULL, "createRuntime requires an options object");
        return NULL;
    }
    state = (runtime_state *)calloc(1u, sizeof(*state));
    cmd = (command *)calloc(1u, sizeof(*cmd));
    if (!state || !cmd) {
        free(state);
        free(cmd);
        return rejected_promise(env, TNY_STATUS_OOM, "out of memory creating native runtime");
    }
    cmd->kind = CMD_RUNTIME_CREATE;
    if (!sdk_parse_create_options(env, argv[0], &cmd->create)) {
        sdk_free_command(cmd);
        free(state);
        return NULL;
    }
    state->env = env;
    pthread_mutex_init(&state->mutex, NULL);
    pthread_mutex_init(&state->session_mutex, NULL);
    pthread_cond_init(&state->cond, NULL);
    atomic_init(&state->env_closing, 0);
    sdk_runtime_add(state);
    token = (tsfn_token *)calloc(1u, sizeof(*token));
    if (!token) {
        sdk_runtime_abandon(state);
        sdk_free_command(cmd);
        return rejected_promise(env, TNY_STATUS_OOM, "out of memory creating owner bridge");
    }
    atomic_init(&token->state, state);
    state->token = token;
    (void)napi_create_string_utf8(env, "tny native owner", NAPI_AUTO_LENGTH, &name);
    if (napi_create_threadsafe_function(env, NULL, NULL, name, COMMAND_CAPACITY, 1u, token,
                                        tsfn_finalize, NULL, complete_on_js,
                                        &state->tsfn) != napi_ok) {
        state->token = NULL;
        free(token);
        sdk_runtime_abandon(state);
        sdk_free_command(cmd);
        return rejected_promise(env, TNY_STATUS_INTERNAL, "failed to start native owner thread");
    }
    int register_cleanup = sdk_register_env_cleanup(env);
    if (register_cleanup < 0 ||
        (register_cleanup > 0 && napi_add_env_cleanup_hook(env, env_cleanup, env) != napi_ok)) {
        if (register_cleanup > 0) sdk_unregister_env_cleanup(env);
        (void)napi_release_threadsafe_function(state->tsfn, napi_tsfn_abort);
        sdk_runtime_abandon(state);
        sdk_free_command(cmd);
        return rejected_promise(env, TNY_STATUS_INTERNAL, "failed to register environment cleanup");
    }
    if (pthread_create(&state->thread, NULL, sdk_owner_main, state) != 0) {
        (void)napi_release_threadsafe_function(state->tsfn, napi_tsfn_abort);
        sdk_runtime_abandon(state);
        sdk_free_command(cmd);
        return rejected_promise(env, TNY_STATUS_INTERNAL, "failed to start native owner thread");
    }
    (void)napi_unref_threadsafe_function(env, state->tsfn);
    promise = enqueue_existing(env, state, cmd);
    return promise;
}

static napi_value dispatch(napi_env env, napi_callback_info info, command_kind kind) {
    size_t argc = 4u;
    napi_value argv[4];
    uint32_t runtime_id, session_id = 0u;
    runtime_state *state;
    command *cmd;
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc < 1u ||
        !sdk_arg_uint32(env, argv[0], &runtime_id))
        return NULL;
    state = sdk_runtime_acquire(runtime_id, env);
    if (!state) return rejected_promise(env, TNY_STATUS_BAD_STATE, "runtime is closed");
    cmd = (command *)calloc(1u, sizeof(*cmd));
    if (!cmd) {
        sdk_runtime_release(state);
        return rejected_promise(env, TNY_STATUS_OOM, "out of memory creating command");
    }
    cmd->kind = kind;
    if (kind == CMD_SESSION_CREATE || kind == CMD_SESSION_OPEN) {
        if (kind == CMD_SESSION_OPEN) {
            if (argc < 2u || !sdk_arg_string(env, argv[1], &cmd->text)) {
                sdk_free_command(cmd);
                sdk_runtime_release(state);
                return NULL;
            }
        }
    } else if (kind != CMD_RUNTIME_CLOSE && kind != CMD_CAPABILITIES) {
        if (argc < 2u || !sdk_arg_uint32(env, argv[1], &session_id)) {
            sdk_free_command(cmd);
            sdk_runtime_release(state);
            return NULL;
        }
        cmd->session_handle = session_id;
    }
    if (kind == CMD_SEND || kind == CMD_STEER || kind == CMD_PERMISSION) {
        if (argc < 3u || !sdk_arg_string(env, argv[2], &cmd->text)) {
            sdk_free_command(cmd);
            sdk_runtime_release(state);
            return NULL;
        }
    }
    if (kind == CMD_PERMISSION) {
        if (argc < 4u || !sdk_arg_uint32(env, argv[3], &cmd->decision)) {
            sdk_free_command(cmd);
            sdk_runtime_release(state);
            return NULL;
        }
    }
    if (kind == CMD_RUNTIME_CLOSE || kind == CMD_SESSION_CLOSE) {
        tny_error *cancel_error = NULL;
        (void)cancel_state(state, kind == CMD_SESSION_CLOSE ? cmd->session_handle : 0u,
                           &cancel_error);
        if (cancel_error) tny_error_free(cancel_error);
    }
    {
        napi_value promise = enqueue_existing(env, state, cmd);
        sdk_runtime_release(state);
        return promise;
    }
}

static napi_value decode_test_event(napi_env env, napi_callback_info info) {
    static char nul_text[] = {'A', '\0', 'B'};
    size_t argc = 1u;
    napi_value argv[1];
    uint32_t kind;
    event_copy event;
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc != 1u ||
        !sdk_arg_uint32(env, argv[0], &kind))
        return NULL;
    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.schema_version = TNY_EVENT_SCHEMA_VERSION;
#define OWNED_LITERAL(value) (sdk_owned_bytes){(char *)(value), sizeof(value) - 1u}
    event.provider = OWNED_LITERAL("test-provider");
    event.session_id = OWNED_LITERAL("test-session");
    event.turn_id = OWNED_LITERAL("test-turn");
    event.text = (sdk_owned_bytes){nul_text, sizeof(nul_text)};
    event.message_id = OWNED_LITERAL("future-message");
    event.message_type = OWNED_LITERAL("future-type");
    event.tool_name = OWNED_LITERAL("future-tool");
    event.tool_id = OWNED_LITERAL("future-tool-id");
    event.tool_detail = OWNED_LITERAL("future-detail");
    event.permission_id = OWNED_LITERAL("future-permission");
    event.permission_summary = OWNED_LITERAL("future-summary");
#undef OWNED_LITERAL
    event.permission_options = 7u;
    event.error_code = -123;
    return sdk_event_to_js(env, &event);
}

#define DISPATCH_FN(name, kind)                                     \
    static napi_value name(napi_env env, napi_callback_info info) { \
        return dispatch(env, info, kind);                           \
    }
DISPATCH_FN(create_session, CMD_SESSION_CREATE)
DISPATCH_FN(open_session, CMD_SESSION_OPEN)
DISPATCH_FN(send_prompt, CMD_SEND)
DISPATCH_FN(next_event, CMD_NEXT)
DISPATCH_FN(steer, CMD_STEER)
DISPATCH_FN(respond_permission, CMD_PERMISSION)
DISPATCH_FN(get_capabilities, CMD_CAPABILITIES)
DISPATCH_FN(close_session, CMD_SESSION_CLOSE)
DISPATCH_FN(close_runtime, CMD_RUNTIME_CLOSE)
#undef DISPATCH_FN

static napi_value module_init(napi_env env, napi_value exports) {
    napi_property_descriptor properties[] = {
        {"createRuntime", NULL, create_runtime, NULL, NULL, NULL, napi_default, NULL},
        {"createSession", NULL, create_session, NULL, NULL, NULL, napi_default, NULL},
        {"openSession", NULL, open_session, NULL, NULL, NULL, napi_default, NULL},
        {"send", NULL, send_prompt, NULL, NULL, NULL, napi_default, NULL},
        {"nextEvent", NULL, next_event, NULL, NULL, NULL, napi_default, NULL},
        {"steer", NULL, steer, NULL, NULL, NULL, napi_default, NULL},
        {"respondPermission", NULL, respond_permission, NULL, NULL, NULL, napi_default, NULL},
        {"cancel", NULL, cancel_direct, NULL, NULL, NULL, napi_default, NULL},
        {"abort", NULL, cancel_direct, NULL, NULL, NULL, napi_default, NULL},
        {"getCapabilities", NULL, get_capabilities, NULL, NULL, NULL, napi_default, NULL},
        {"closeSession", NULL, close_session, NULL, NULL, NULL, napi_default, NULL},
        {"closeRuntime", NULL, close_runtime, NULL, NULL, NULL, napi_default, NULL},
        {"__testDecodeEvent", NULL, decode_test_event, NULL, NULL, NULL, napi_default, NULL},
    };
    (void)napi_define_properties(env, exports, sizeof(properties) / sizeof(properties[0]),
                                 properties);
    sdk_define_probe(env, exports);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, module_init)
