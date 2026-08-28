#define _POSIX_C_SOURCE 200809L
#include "tny/tny.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

_Static_assert(sizeof(tny_runtime_options_v0) == 200, "v0 changed");
_Static_assert(sizeof(tny_tool_result_v1) == 64, "tool result v1 changed");
_Static_assert(sizeof(tny_tool_spec_v1) == 160, "tool spec v1 changed");
_Static_assert(sizeof(tny_capabilities_v1) == 344, "capabilities v1 changed");

typedef enum { MODE_SYNC, MODE_ASYNC, MODE_ASYNC_VALIDATE, MODE_CANCEL, MODE_CLOSE } invoke_mode;

typedef struct {
    tny_runtime *runtime;
    invoke_mode mode;
    int invoked;
    int reentrant_status;
    int32_t first_completion;
    int32_t pending_stale_completion;
    int32_t invalid_utf8_completion;
    int32_t second_completion;
    int32_t stale_completion;
    pthread_t thread;
    int thread_started;
    tny_tool_call *call;
    uint64_t generation;
    pthread_mutex_t gate_mutex;
    pthread_cond_t gate_condition;
    int release_completion;
} callback_state;

static tny_bytes view(const char *text) { return (tny_bytes){text, (uint64_t)strlen(text)}; }

static void *complete_later(void *opaque) {
    callback_state *state = opaque;
    if (state->mode == MODE_CANCEL || state->mode == MODE_CLOSE) {
        pthread_mutex_lock(&state->gate_mutex);
        while (!state->release_completion)
            pthread_cond_wait(&state->gate_condition, &state->gate_mutex);
        pthread_mutex_unlock(&state->gate_mutex);
    } else {
        struct timespec delay = {0, 100 * 1000 * 1000};
        nanosleep(&delay, NULL);
    }
    tny_tool_result_v1 result;
    if (tny_tool_result_v1_init(&result, sizeof result) != TNY_STATUS_OK) {
        state->first_completion = TNY_STATUS_INTERNAL;
        return NULL;
    }
    result.data = view("async-result");
    if (state->mode == MODE_ASYNC) {
        state->first_completion =
            tny_tool_call_complete(state->call, state->generation, &result, NULL);
        tny_tool_call_release(state->call);
        return NULL;
    }
    state->pending_stale_completion =
        tny_tool_call_complete(state->call, state->generation + 1, &result, NULL);
    static const char invalid_utf8[] = {(char)0xFF};
    result.data = (tny_bytes){invalid_utf8, 1};
    state->invalid_utf8_completion =
        tny_tool_call_complete(state->call, state->generation, &result, NULL);
    result.data = view("async-result");
    result.reserved_scalar = 7;
    result.reserved[0] = 1;
    state->first_completion = tny_tool_call_complete(state->call, state->generation, &result, NULL);
    result.reserved[0] = 0;
    state->second_completion =
        tny_tool_call_complete(state->call, state->generation, &result, NULL);
    state->stale_completion =
        tny_tool_call_complete(state->call, state->generation + 1, &result, NULL);
    tny_tool_call_release(state->call);
    return NULL;
}

static int32_t invoke(void *opaque, tny_tool_call *call, uint64_t generation, tny_bytes arguments,
                      tny_tool_result_v1 *result) {
    callback_state *state = opaque;
    state->invoked++;
    state->call = call;
    state->generation = generation;
    if (!arguments.ptr || !arguments.len || generation == 0 ||
        tny_tool_call_generation(call) != generation)
        return TNY_STATUS_INTERNAL;
    state->reentrant_status = tny_runtime_register_tool(state->runtime, NULL, NULL, NULL);
    if (state->mode == MODE_SYNC) {
        if (tny_tool_result_v1_init(result, sizeof *result) != TNY_STATUS_OK)
            return TNY_STATUS_INTERNAL;
        result->data = view("sync-result");
        return TNY_TOOL_INVOKE_SYNC;
    }
    if (pthread_create(&state->thread, NULL, complete_later, state) != 0)
        return TNY_STATUS_INTERNAL;
    state->thread_started = 1;
    return TNY_TOOL_INVOKE_ASYNC;
}

static int create_runtime(const char *workspace, const char *url, uint32_t permission_mode,
                          tny_runtime **out) {
    tny_runtime_options_v0 options;
    if (tny_runtime_options_init(&options, sizeof options) != TNY_STATUS_OK)
        return TNY_STATUS_INTERNAL;
    options.workspace = view(workspace);
    options.base_url = view(url);
    options.api_key = view("custom-fixture-not-real");
    options.permission_mode = permission_mode;
    options.max_tool_result_bytes = 8;
    return tny_runtime_create(&options, sizeof options, out, NULL);
}

static int register_tool(tny_runtime *runtime, callback_state *state, uint32_t sensitivity,
                         tny_tool_registration **registration) {
    tny_tool_spec_v1 spec;
    if (tny_tool_spec_v1_init(&spec, sizeof spec) != TNY_STATUS_OK) return 1;
    spec.user_data = state;
    spec.name = view("host_echo");
    spec.description = view("Return a host-generated test value.");
    spec.input_schema_json =
        view("{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"string\"}},"
             "\"required\":[\"value\"]}");
    spec.sensitivity = sensitivity;
    spec.max_argument_bytes = 1024;
    spec.max_result_bytes = 1024;
    spec.invoke = invoke;
    return tny_runtime_register_tool(runtime, &spec, registration, NULL);
}

static int invalid_specs(tny_runtime *runtime, callback_state *state) {
    tny_tool_spec_v1 spec;
    if (tny_tool_spec_v1_init(&spec, sizeof spec) != TNY_STATUS_OK) return 1;
    spec.user_data = state;
    spec.description = view("invalid fixture");
    spec.input_schema_json = view("{\"type\":\"object\"}");
    spec.invoke = invoke;
    tny_tool_registration *registration = NULL;
    spec.name = view("list_files");
    if (tny_runtime_register_tool(runtime, &spec, &registration, NULL) !=
        TNY_STATUS_INVALID_ARGUMENT)
        return 1;
    spec.name = view("hostile_schema");
    spec.max_result_bytes = 1024;
    spec.reserved[0] = 0;
    spec.input_schema_json = view("{\"type\":\"object\",\"properties\":{\"value\":{"
                                  "\"type\":\"string\",\"pattern\":\".*\"}}}");
    if (tny_runtime_register_tool(runtime, &spec, &registration, NULL) !=
        TNY_STATUS_INVALID_ARGUMENT)
        return 1;
    spec.max_result_bytes = 1024;
    spec.reserved_scalar = 7;
    spec.reserved[0] = 1;
    spec.name = view("reserved_tail");
    spec.input_schema_json = view("{\"type\":\"object\"}");
    if (tny_runtime_register_tool(runtime, &spec, &registration, NULL) != TNY_STATUS_OK ||
        tny_tool_registration_unregister(registration, NULL) != TNY_STATUS_OK)
        return 1;
    registration = NULL;
    spec.name = view("too_large");
    spec.input_schema_json = view("{\"type\":\"object\"}");
    spec.max_result_bytes = TNY_CUSTOM_TOOL_RESULT_MAX + 1;
    if (tny_runtime_register_tool(runtime, &spec, &registration, NULL) !=
        TNY_STATUS_INVALID_ARGUMENT)
        return 1;
    spec.name = view("bad_schema");
    spec.input_schema_json = view("[]");
    if (tny_runtime_register_tool(runtime, &spec, &registration, NULL) !=
        TNY_STATUS_INVALID_ARGUMENT)
        return 1;
    static const char embedded[] = "bad\0name";
    spec.name = (tny_bytes){embedded, sizeof embedded - 1};
    spec.input_schema_json = view("{\"type\":\"object\"}");
    if (tny_runtime_register_tool(runtime, &spec, &registration, NULL) !=
        TNY_STATUS_INVALID_ARGUMENT)
        return 1;
    return 0;
}

static int drain_turn(tny_session *session, int deny_permission, uint32_t expected_stop,
                      int *tool_ends) {
    int terminals = 0;
    for (;;) {
        tny_event *event = NULL;
        int32_t status = tny_session_next_event(session, 5000, &event, NULL);
        if (status == TNY_STATUS_DRAINED) break;
        if (status != TNY_STATUS_EVENT || !event) return 1;
        tny_event_view_v0 event_view;
        if (tny_event_view_init(&event_view, sizeof event_view) != TNY_STATUS_OK) return 1;
        if (tny_event_read(event, &event_view, sizeof event_view) != TNY_STATUS_OK) return 1;
        if (event_view.kind == TNY_EVENT_PERMISSION && deny_permission) {
            if (tny_session_respond_permission(session, event_view.permission_id,
                                               TNY_PERMISSION_DENY, NULL) != TNY_STATUS_OK)
                return 1;
        }
        if (event_view.kind == TNY_EVENT_TOOL_END) (*tool_ends)++;
        if (event_view.kind == TNY_EVENT_TURN_END) {
            terminals++;
            if (event_view.stop_reason != expected_stop) return 1;
        }
        tny_event_free(event);
    }
    return terminals != 1;
}

static int run_mode(const char *workspace, const char *url, invoke_mode mode) {
    callback_state state = {.mode = mode};
    if (pthread_mutex_init(&state.gate_mutex, NULL) != 0 ||
        pthread_cond_init(&state.gate_condition, NULL) != 0)
        return 27;
    tny_runtime *runtime = NULL;
    uint32_t permission = mode == MODE_SYNC ? TNY_PERMISSION_ASK : TNY_PERMISSION_YOLO;
    if (create_runtime(workspace, url, permission, &runtime) != 0) return 30;
    state.runtime = runtime;
    if (invalid_specs(runtime, &state)) return 29;
    tny_tool_registration *registration = NULL;
    uint32_t sensitivity = (mode == MODE_ASYNC || mode == MODE_ASYNC_VALIDATE)
                               ? TNY_TOOL_SENSITIVITY_SENSITIVE
                               : TNY_TOOL_SENSITIVITY_SAFE;
    if (register_tool(runtime, &state, sensitivity, &registration) != 0) return 31;

    tny_capabilities_v1 capabilities;
    if (tny_capabilities_v1_init(&capabilities, sizeof capabilities) != TNY_STATUS_OK) return 1;
    if (tny_runtime_get_capabilities_v1(runtime, &capabilities, sizeof capabilities) != 0 ||
        !(capabilities.base.feature_enabled_mask & TNY_CAP_FEATURE_CUSTOM_TOOLS) ||
        capabilities.custom_tool_max_count != TNY_CUSTOM_TOOL_MAX_COUNT ||
        capabilities.custom_tool_result_max != TNY_CUSTOM_TOOL_RESULT_MAX)
        return 28;

    tny_tool_registration *duplicate = NULL;
    if (register_tool(runtime, &state, TNY_TOOL_SENSITIVITY_SAFE, &duplicate) !=
        TNY_STATUS_BAD_STATE)
        return 32;
    tny_session *session = NULL;
    if (tny_session_create(runtime, &session, NULL) != 0) return 33;
    if (register_tool(runtime, &state, TNY_TOOL_SENSITIVITY_SAFE, &duplicate) !=
        TNY_STATUS_BAD_STATE)
        return 34;
    if (tny_session_send(session, view("call the host tool"), NULL) != 0) return 35;

    int tool_ends = 0;
    struct timespec started = {0};
    if (mode == MODE_ASYNC || mode == MODE_ASYNC_VALIDATE) clock_gettime(CLOCK_MONOTONIC, &started);
    if (mode == MODE_CANCEL || mode == MODE_CLOSE) {
        for (;;) {
            tny_event *event = NULL;
            int32_t status = tny_session_next_event(session, 5000, &event, NULL);
            if (status != TNY_STATUS_EVENT || !event) return 36;
            uint32_t kind = tny_event_get_kind(event);
            tny_event_free(event);
            if (kind == TNY_EVENT_TOOL_START) break;
        }
        if (mode == MODE_CLOSE) {
            tny_runtime_free(runtime);
            pthread_mutex_lock(&state.gate_mutex);
            state.release_completion = 1;
            pthread_cond_broadcast(&state.gate_condition);
            pthread_mutex_unlock(&state.gate_mutex);
            pthread_join(state.thread, NULL);
            if (state.first_completion != TNY_STATUS_BAD_STATE) return 45;
            pthread_cond_destroy(&state.gate_condition);
            pthread_mutex_destroy(&state.gate_mutex);
            return 0;
        }
        pthread_mutex_lock(&state.gate_mutex);
        state.release_completion = 1;
        pthread_cond_broadcast(&state.gate_condition);
        pthread_mutex_unlock(&state.gate_mutex);
        if (tny_session_cancel(session, NULL) != TNY_STATUS_OK) return 37;
        if (drain_turn(session, 0, TNY_STOP_REASON_INTERRUPTED, &tool_ends)) return 38;
    } else if (drain_turn(session, 0, TNY_STOP_REASON_DONE, &tool_ends)) {
        return 39;
    }
    if (mode == MODE_ASYNC || mode == MODE_ASYNC_VALIDATE) {
        struct timespec finished;
        clock_gettime(CLOCK_MONOTONIC, &finished);
        double elapsed = (double)(finished.tv_sec - started.tv_sec) +
                         (double)(finished.tv_nsec - started.tv_nsec) / 1000000000.0;
        if (elapsed >= 2.0) return 46;
    }
    if (state.thread_started) pthread_join(state.thread, NULL);
    if (state.invoked != 1 || state.reentrant_status != TNY_STATUS_BAD_STATE) return 40;
    if (mode == MODE_ASYNC_VALIDATE &&
        (state.pending_stale_completion != TNY_STATUS_BAD_STATE ||
         state.invalid_utf8_completion != TNY_STATUS_INVALID_ARGUMENT ||
         state.first_completion != TNY_STATUS_OK ||
         state.second_completion != TNY_STATUS_BAD_STATE ||
         state.stale_completion != TNY_STATUS_BAD_STATE))
        return 41;
    if (mode == MODE_ASYNC && state.first_completion != TNY_STATUS_OK) return 47;
    if (mode == MODE_CANCEL && state.first_completion != TNY_STATUS_OK &&
        state.first_completion != TNY_STATUS_BAD_STATE)
        return 42;
    if (mode != MODE_CANCEL && tool_ends != 1) return 43;
    tny_session_free(session);
    if (tny_tool_registration_unregister(registration, NULL) != TNY_STATUS_OK ||
        tny_tool_registration_unregister(registration, NULL) != TNY_STATUS_BAD_STATE)
        return 44;
    tny_runtime_free(runtime);
    pthread_cond_destroy(&state.gate_condition);
    pthread_mutex_destroy(&state.gate_mutex);
    return 0;
}

static int denied_mode(const char *workspace, const char *url) {
    callback_state state = {.mode = MODE_SYNC};
    tny_runtime *runtime = NULL;
    if (create_runtime(workspace, url, TNY_PERMISSION_ASK, &runtime) != 0) return 50;
    state.runtime = runtime;
    tny_tool_registration *registration = NULL;
    if (register_tool(runtime, &state, TNY_TOOL_SENSITIVITY_SENSITIVE, &registration) != 0)
        return 51;
    tny_session *session = NULL;
    if (tny_session_create(runtime, &session, NULL) != 0 ||
        tny_session_send(session, view("deny the host tool"), NULL) != 0)
        return 52;
    int tool_ends = 0;
    if (drain_turn(session, 1, TNY_STOP_REASON_DENIED, &tool_ends)) return 53;
    if (state.invoked != 0) return 54;
    tny_session_free(session);
    tny_runtime_free(runtime);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    int status = run_mode(argv[1], argv[2], MODE_SYNC);
    if (!status) status = run_mode(argv[1], argv[2], MODE_ASYNC);
    if (!status) status = run_mode(argv[1], argv[2], MODE_ASYNC_VALIDATE);
    if (!status) status = run_mode(argv[1], argv[2], MODE_CANCEL);
    if (!status) status = run_mode(argv[1], argv[2], MODE_CLOSE);
    if (!status) status = denied_mode(argv[1], argv[2]);
    if (status) {
        fprintf(stderr, "libtny-custom-tools failed at %d\n", status);
        return status;
    }
    puts("libtny-custom-tools: sync/async/cancel/deny lifecycle passed");
    return 0;
}
