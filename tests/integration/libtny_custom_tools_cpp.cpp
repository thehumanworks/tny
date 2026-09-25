#include "tny/tny.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

struct State {
    int invoked = 0;
    bool asynchronous = false;
    bool completion_oom = false;
    int failed_completions = 0;
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
            if (state->completion_oom) {
                if (setenv("TNY_TEST_ALLOC_SCOPE", "tool_complete", 1) != 0 ||
                    setenv("TNY_TEST_ALLOC_FAIL_AT", "1", 1) != 0)
                    return TNY_STATUS_INTERNAL;
            }
            int32_t status = tny_tool_call_complete(call, generation, result, nullptr);
            if (state->completion_oom) {
                unsetenv("TNY_TEST_ALLOC_SCOPE");
                unsetenv("TNY_TEST_ALLOC_FAIL_AT");
                if (status == TNY_STATUS_OOM) state->failed_completions++;
            }
            std::memset(copied, 'x', sizeof copied - 1);
            /* A negative return leaves the handle owned by the invoker. */
            if (status != TNY_STATUS_OK) return status;
            tny_tool_call_release(call);
            return TNY_TOOL_INVOKE_ASYNC;
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
    bool inject_oom = std::getenv("TNY_CUSTOM_TOOL_COMPLETION_OOM") != nullptr;
    int turns = inject_oom ? 4 : 2;
    for (int turn = 0; turn < turns; ++turn) {
        state.completion_oom = inject_oom && (turn == 1 || turn == 2);
        int tool_ends = 0;
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
            if (tny_event_get_kind(event) == TNY_EVENT_TOOL_END) {
                tny_bytes name = tny_event_tool_name(event);
                tny_bytes detail = tny_event_tool_detail(event);
                std::string message(detail.ptr ? static_cast<const char *>(detail.ptr) : "",
                                    static_cast<size_t>(detail.len));
                if (tny_event_tool_ok(event) || name.len != 8 ||
                    std::memcmp(name.ptr, "run_code", 8) != 0 ||
                    message.find("execution server unavailable") == std::string::npos ||
                    message.find("no direct fallback") == std::string::npos)
                    return 10;
                tool_ends++;
            }
            if (tny_event_get_kind(event) == TNY_EVENT_TURN_END) {
                /* Explicit embedded refusal is a tool error; the mock then finishes. */
                if (tny_event_stop_reason(event) != TNY_STOP_REASON_DONE) return 7;
                terminals++;
            }
            tny_event_free(event);
        }
        if (tool_ends != 1) return 10;
    }
    tny_session_free(session);
    if (tny_tool_registration_unregister(registration, nullptr) != 0 || state.invoked != 0 ||
        terminals != turns || state.failed_completions != 0)
        return 8;
    tny_runtime_free(runtime);
    std::cout << "libtny-custom-tools: C++ registration/refusal passed; callbacks=" << state.invoked
              << '\n';
    return 0;
}
