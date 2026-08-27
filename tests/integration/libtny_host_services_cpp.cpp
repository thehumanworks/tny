#include "tny/tny.h"

#include <cstdint>
#include <cstring>
#include <iostream>

struct State {
    int clock_calls = 0;
    int diagnostic_calls = 0;
};

static int32_t TNY_CALL diagnostic(void *opaque, uint32_t, tny_bytes,
                                   tny_bytes) noexcept {
    static_cast<State *>(opaque)->diagnostic_calls++;
    return TNY_STATUS_OK;
}

static int32_t TNY_CALL clock_ms(void *opaque, int64_t *out) noexcept {
    State *state = static_cast<State *>(opaque);
    try {
        *out = 4000 + ++state->clock_calls;
        return TNY_STATUS_OK;
    } catch (...) {
        return TNY_STATUS_INTERNAL;
    }
}

static tny_bytes view(const char *text) {
    return {text, static_cast<uint64_t>(std::strlen(text))};
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    State state;
    tny_host_services_v1 services;
    tny_host_services_v1_init(&services);
    services.user_data = &state;
    services.diagnostic = diagnostic;
    services.monotonic_ms = clock_ms;

    tny_runtime_options_v1 options;
    tny_runtime_options_v1_init(&options);
    options.runtime.workspace = view(argv[1]);
    options.runtime.base_url = view("http://127.0.0.1:1/v1");
    options.runtime.api_key = view("cpp-fixture-not-real");
    options.host_services = &services;

    tny_runtime *runtime = nullptr;
    tny_error *error = nullptr;
    if (tny_runtime_create_v1(&options, &runtime, &error) != TNY_STATUS_OK)
        return 3;
    int64_t now = 0;
    if (tny_runtime_host_monotonic_ms(runtime, &now, &error) != TNY_STATUS_OK ||
        now != 4001)
        return 4;
    tny_runtime_free(runtime);
    if (state.diagnostic_calls != 2) return 5;
    std::cout << "libtny-host-services: C++ noexcept callback passed\n";
    return 0;
}
