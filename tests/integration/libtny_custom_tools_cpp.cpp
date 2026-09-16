#include "tny/tny.h"

#include <cstdint>
#include <cstring>
#include <iostream>

struct State {
    int invoked = 0;
    bool asynchronous = false;
};

static tny_bytes view(const char *text) { return {text, static_cast<uint64_t>(std::strlen(text))}; }

static int32_t TNY_CALL invoke(void *opaque, tny_tool_call *call, uint64_t generation, tny_bytes,
                               tny_tool_result_v1 *result) noexcept {
    auto *state = static_cast<State *>(opaque);
    try {
        state->invoked++;
        if (tny_tool_result_v1_init(result, sizeof *result) != TNY_STATUS_OK)
            return TNY_STATUS_INTERNAL;
        result->data = view("cpp-result");
        if (state->asynchronous) {
            char copied[] = "cpp-result";
            result->data = view(copied);
            int32_t status = tny_tool_call_complete(call, generation, result, nullptr);
            std::memset(copied, 'x', sizeof copied - 1);
            tny_tool_call_release(call);
            return status == TNY_STATUS_OK ? TNY_TOOL_INVOKE_ASYNC : status;
        }
        return TNY_TOOL_INVOKE_SYNC;
    } catch (...) { return TNY_STATUS_INTERNAL; }
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    tny_runtime_options_v0 options;
    if (tny_runtime_options_init(&options, sizeof options) != TNY_STATUS_OK) return 2;
    options.workspace = view(argv[1]);
    options.base_url = view(argv[2]);
    options.api_key = view("cpp-custom-not-real");
    options.permission_mode = TNY_PERMISSION_YOLO;
    tny_runtime *runtime = nullptr;
    if (tny_runtime_create(&options, sizeof options, &runtime, nullptr) != TNY_STATUS_OK) return 3;
    State state;
    tny_tool_spec_v1 spec;
    if (tny_tool_spec_v1_init(&spec, sizeof spec) != TNY_STATUS_OK) return 4;
    spec.user_data = &state;
    spec.name = view("host_echo");
    spec.description = view("C++ custom tool fixture");
    spec.input_schema_json =
        view("{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"string\"}}}");
    spec.invoke = invoke;
    tny_tool_registration *registration = nullptr;
    if (tny_runtime_register_tool(runtime, &spec, &registration, nullptr) != 0) return 4;
    tny_session *session = nullptr;
    if (tny_session_create(runtime, &session, nullptr) != 0 ||
        tny_session_send(session, view("invoke C++ tool"), nullptr) != 0)
        return 5;
    int terminals = 0;
    for (int turn = 0; turn < 2; ++turn) {
        if (turn) {
            tny_session_free(session);
            if (tny_session_create(runtime, &session, nullptr) != 0) return 9;
            state.asynchronous = true;
            if (tny_session_send(session, view("async C++ tool"), nullptr) != 0) return 9;
        }
        for (;;) {
            tny_event *event = nullptr;
            int32_t status = tny_session_next_event(session, 5000, &event, nullptr);
            if (status == TNY_STATUS_DRAINED) break;
            if (status != TNY_STATUS_EVENT || !event) return 6;
            if (tny_event_get_kind(event) == TNY_EVENT_TURN_END) {
                if (tny_event_stop_reason(event) != TNY_STOP_REASON_DONE) return 7;
                terminals++;
            }
            tny_event_free(event);
        }
    }
    tny_session_free(session);
    if (tny_tool_registration_unregister(registration, nullptr) != 0 || state.invoked != 2 ||
        terminals != 2)
        return 8;
    tny_runtime_free(runtime);
    std::cout << "libtny-custom-tools: C++ callback passed\n";
    return 0;
}
