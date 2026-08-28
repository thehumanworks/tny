#define _POSIX_C_SOURCE 200809L
#include "tny/tny.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

_Static_assert(sizeof(tny_runtime_options_v0) == 200, "frozen runtime options v0 changed");
_Static_assert(sizeof(tny_host_services_v1) == 136, "host services v1 layout changed");
_Static_assert(sizeof(tny_runtime_options_v1) == 280, "runtime options v1 layout changed");

typedef struct {
    tny_runtime *runtime;
    int diagnostic_calls;
    int monotonic_calls;
    int random_calls;
    int notify_calls;
    int reentrant_status;
    int fail_open;
    int clock_status;
    int64_t clock_override;
    int omit_revision;
    int alive;
    uint64_t revision;
    unsigned char stored[64];
    uint64_t stored_size;
    char order[256];
    size_t order_len;
} host_state;

static int contains(const char *haystack, size_t haystack_len, const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len > haystack_len) return 0;
    for (size_t i = 0; i <= haystack_len - needle_len; i++)
        if (memcmp(haystack + i, needle, needle_len) == 0) return 1;
    return 0;
}

static void ordered(host_state *state, char marker) {
    if (state->order_len < sizeof state->order) state->order[state->order_len++] = marker;
}

static int32_t diagnostic(void *opaque, uint32_t level, tny_bytes component, tny_bytes message) {
    host_state *state = opaque;
    if (!state->alive || level > TNY_DIAGNOSTIC_ERROR || component.len != 6 ||
        memcmp(component.ptr, "libtny", 6) != 0 || !message.ptr || !message.len)
        return TNY_STATUS_INTERNAL;
    if (contains(message.ptr, (size_t)message.len, "secret")) return TNY_STATUS_INTERNAL;
    ordered(state, contains(message.ptr, (size_t)message.len, "destroying") ? 'D'
                   : contains(message.ptr, (size_t)message.len, "created")  ? 'C'
                                                                            : 'W');
    state->diagnostic_calls++;
    return TNY_STATUS_OK;
}

static void check_reentrant(host_state *state) {
    if (!state->runtime) return;
    tny_capabilities_v0 capabilities;
    if (tny_capabilities_init(&capabilities, sizeof capabilities) != TNY_STATUS_OK) return;
    state->reentrant_status =
        tny_runtime_get_capabilities(state->runtime, &capabilities, sizeof capabilities);
}

static int32_t clock_ms(void *opaque, int64_t *out_ms) {
    host_state *state = opaque;
    ordered(state, 'M');
    check_reentrant(state);
    state->monotonic_calls++;
    if (state->clock_status) return state->clock_status;
    *out_ms = state->clock_override ? state->clock_override : 1000 + state->monotonic_calls;
    return TNY_STATUS_OK;
}

static int32_t random_bytes(void *opaque, void *buffer, uint64_t size) {
    host_state *state = opaque;
    ordered(state, 'R');
    check_reentrant(state);
    memset(buffer, 0xA5, (size_t)size);
    state->random_calls++;
    return TNY_STATUS_OK;
}

static int32_t storage_store(void *opaque, tny_bytes key, uint64_t expected_revision,
                             const void *data, uint64_t size, uint64_t *out_revision) {
    host_state *state = opaque;
    ordered(state, 'S');
    check_reentrant(state);
    if (key.len != 7 || memcmp(key.ptr, "session", 7) != 0 ||
        expected_revision != state->revision || size > sizeof state->stored)
        return TNY_STATUS_BAD_STATE;
    memcpy(state->stored, data, (size_t)size);
    state->stored_size = size;
    state->revision++;
    if (!state->omit_revision) *out_revision = state->revision;
    return TNY_STATUS_OK;
}

static int32_t storage_load(void *opaque, tny_bytes key, uint64_t *out_revision, void *buffer,
                            uint64_t capacity, uint64_t *out_size) {
    host_state *state = opaque;
    ordered(state, 'L');
    check_reentrant(state);
    if (key.len != 7 || memcmp(key.ptr, "session", 7) != 0) return TNY_STATUS_BAD_STATE;
    *out_size = state->stored_size;
    *out_revision = state->revision;
    if (capacity < state->stored_size) return TNY_STATUS_BACKPRESSURE;
    memcpy(buffer, state->stored, (size_t)state->stored_size);
    return TNY_STATUS_OK;
}

static int32_t open_url(void *opaque, tny_bytes url) {
    host_state *state = opaque;
    ordered(state, 'O');
    check_reentrant(state);
    if (!url.ptr || !url.len) return TNY_STATUS_INVALID_ARGUMENT;
    if (state->fail_open == 2) return TNY_TOOL_INVOKE_ASYNC;
    return state->fail_open ? TNY_STATUS_IO : TNY_STATUS_OK;
}

static int32_t notify_scheduler(void *opaque) {
    host_state *state = opaque;
    ordered(state, 'N');
    check_reentrant(state);
    state->notify_calls++;
    return TNY_STATUS_OK;
}

static tny_bytes view(const char *text) {
    tny_bytes value = {text, (uint64_t)strlen(text)};
    return value;
}

static int error_contains(tny_error *error, const char *needle) {
    tny_bytes message = tny_error_message(error);
    return message.ptr && contains(message.ptr, (size_t)message.len, needle);
}

typedef struct {
    tny_runtime *runtime;
    int32_t status;
} thread_call;

static void *wrong_thread(void *opaque) {
    thread_call *call = opaque;
    int64_t now = 0;
    call->status = tny_runtime_host_monotonic_ms(call->runtime, &now, NULL);
    return NULL;
}

static int create_options(const char *workspace, const char *base_url,
                          tny_runtime_options_v1 *options, tny_host_services_v1 *services) {
    if (tny_runtime_options_v1_init(options, sizeof *options) != TNY_STATUS_OK ||
        tny_host_services_v1_init(services, sizeof *services) != TNY_STATUS_OK)
        return 0;
    options->runtime.workspace = view(workspace);
    options->runtime.base_url = view(base_url);
    options->runtime.api_key = view("fixture-not-real");
    options->runtime.permission_mode = TNY_PERMISSION_YOLO;
    options->host_services = services;
    options->reserved[0] = UINT64_C(0x1234);
    services->reserved[0] = UINT64_C(0x5678);
    return options->abi_version == 1 && services->abi_version == 1;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    host_state state = {0};
    state.alive = 1;
    tny_runtime_options_v1 options;
    tny_host_services_v1 services;
    if (!create_options(argv[1], argv[2], &options, &services)) return 3;

    tny_host_services_v1 invalid_services;
    if (tny_host_services_v1_init(&invalid_services, sizeof invalid_services) != TNY_STATUS_OK)
        return 1;
    invalid_services.abi_version = 2;
    tny_runtime_options_v1 invalid_options = options;
    invalid_options.host_services = &invalid_services;
    tny_runtime *invalid_runtime = NULL;
    tny_error *invalid_error = NULL;
    if (tny_runtime_create_v1(&invalid_options, sizeof invalid_options, &invalid_runtime,
                              &invalid_error) != TNY_STATUS_INVALID_ARGUMENT ||
        invalid_runtime != NULL || invalid_error == NULL)
        return 23;
    tny_error_free(invalid_error);

    services.user_data = &state;
    services.diagnostic = diagnostic;
    services.monotonic_ms = clock_ms;
    services.secure_random = random_bytes;
    services.storage_load = storage_load;
    services.storage_store = storage_store;
    services.open_url = open_url;
    services.notify_scheduler = notify_scheduler;

    tny_runtime *runtime = NULL;
    tny_error *error = NULL;
    if (tny_runtime_create_v1(&options, sizeof options, &runtime, &error) != TNY_STATUS_OK)
        return 4;
    state.runtime = runtime;
    memset(&services, 0, sizeof services); /* creation copied the table */

    tny_capabilities_v0 capabilities;
    if (tny_capabilities_init(&capabilities, sizeof capabilities) != TNY_STATUS_OK) return 1;
    if (tny_runtime_get_capabilities(runtime, &capabilities, sizeof capabilities) !=
            TNY_STATUS_OK ||
        !(capabilities.feature_enabled_mask & TNY_CAP_FEATURE_HOST_SERVICES))
        return 5;

    int64_t first = 0, second = 0;
    if (tny_runtime_host_monotonic_ms(runtime, &first, &error) != TNY_STATUS_OK ||
        tny_runtime_host_monotonic_ms(runtime, &second, &error) != TNY_STATUS_OK || first != 1001 ||
        second != 1002 || state.reentrant_status != TNY_STATUS_BAD_STATE)
        return 6;
    state.clock_status = TNY_STATUS_IO;
    if (tny_runtime_host_monotonic_ms(runtime, &first, &error) != TNY_STATUS_IO) return 26;
    tny_error_free(error);
    error = NULL;
    state.clock_status = 0;
    state.clock_override = INT64_MAX;
    if (tny_runtime_host_monotonic_ms(runtime, &first, &error) != TNY_STATUS_PROTOCOL) return 27;
    tny_error_free(error);
    error = NULL;
    state.clock_override = 0;
    if (tny_runtime_host_monotonic_ms(runtime, &first, &error) != TNY_STATUS_OK || first < second)
        return 28;
    unsigned char random[16] = {0};
    if (tny_runtime_host_secure_random(runtime, random, sizeof random, &error) != TNY_STATUS_OK ||
        random[0] != 0xA5 || random[15] != 0xA5)
        return 7;

    unsigned char input[] = {0, 1, 2, 3};
    uint64_t revision = 0;
    if (tny_runtime_host_storage_store(runtime, view("session"), 0, input, sizeof input, &revision,
                                       &error) != TNY_STATUS_OK ||
        revision != 1)
        return 8;
    memset(input, 0xFF, sizeof input);
    unsigned char output[16] = {0};
    uint64_t size = 0;
    if (tny_runtime_host_storage_load(runtime, view("session"), &revision, output, sizeof output,
                                      &size, &error) != TNY_STATUS_OK ||
        revision != 1 || size != 4 || output[2] != 2)
        return 9;
    state.omit_revision = 1;
    if (tny_runtime_host_storage_store(runtime, view("session"), revision, input, sizeof input,
                                       &revision, &error) != TNY_STATUS_PROTOCOL)
        return 29;
    tny_error_free(error);
    error = NULL;
    state.omit_revision = 0;
    if (tny_runtime_host_open_url(runtime, view("https://example.invalid"), &error) !=
            TNY_STATUS_OK ||
        tny_runtime_host_notify_scheduler(runtime, &error) != TNY_STATUS_OK)
        return 10;
    if (state.order_len < 15 || memcmp(state.order, "CMMMWMWMRSLSWON", 15) != 0) return 24;

    int notify_before_turn = state.notify_calls;
    tny_session *session = NULL;
    if (tny_session_create(runtime, &session, &error) != TNY_STATUS_OK ||
        tny_session_send(session, view("strict host-services turn"), &error) != TNY_STATUS_OK)
        return 18;
    int terminals = 0;
    int64_t previous_timestamp = -1;
    for (;;) {
        tny_event *event = NULL;
        int32_t next = tny_session_next_event(session, 5000, &event, &error);
        if (next == TNY_STATUS_DRAINED) break;
        if (next != TNY_STATUS_EVENT || !event) return 19;
        tny_event_view_v0 event_view;
        if (tny_event_view_init(&event_view, sizeof event_view) != TNY_STATUS_OK) return 1;
        if (tny_event_read(event, &event_view, sizeof event_view) != TNY_STATUS_OK ||
            event_view.timestamp_ms < previous_timestamp)
            return 20;
        previous_timestamp = event_view.timestamp_ms;
        if (event_view.kind == TNY_EVENT_TURN_END) {
            terminals++;
            if (event_view.stop_reason != TNY_STOP_REASON_DONE) return 21;
        }
        tny_event_free(event);
    }
    if (terminals != 1 || state.notify_calls <= notify_before_turn) return 22;
    /* Leave the drained session attached: runtime_free must emit its final
     * callback before child teardown and no child cleanup may call back. */

    state.fail_open = 1;
    int32_t failed =
        tny_runtime_host_open_url(runtime, view("https://secret.invalid/token"), &error);
    if (failed != TNY_STATUS_IO || !error || tny_error_code(error) != TNY_STATUS_IO ||
        error_contains(error, "secret"))
        return 11;
    tny_error_free(error);
    error = NULL;
    state.fail_open = 2;
    if (tny_runtime_host_open_url(runtime, view("https://example.invalid"), &error) !=
        TNY_STATUS_INTERNAL)
        return 30;
    tny_error_free(error);
    error = NULL;

    thread_call call = {runtime, 0};
    pthread_t thread;
    if (pthread_create(&thread, NULL, wrong_thread, &call) != 0) return 12;
    pthread_join(thread, NULL);
    if (call.status != TNY_STATUS_BAD_STATE) return 13;

    int diagnostics_before_free = state.diagnostic_calls;
    tny_runtime_free(runtime);
    state.runtime = NULL;
    state.alive = 0;
    struct timespec settle = {0, 10 * 1000 * 1000};
    nanosleep(&settle, NULL);
    if (state.diagnostic_calls != diagnostics_before_free + 1) return 14;
    if (!state.order_len || state.order[state.order_len - 1] != 'D') return 25;

    tny_runtime_options_v1 empty_options;
    tny_host_services_v1 empty_services;
    if (!create_options(argv[1], argv[2], &empty_options, &empty_services)) return 15;
    runtime = NULL;
    if (tny_runtime_create_v1(&empty_options, sizeof empty_options, &runtime, &error) !=
        TNY_STATUS_OK)
        return 16;
    unsigned char native_random[8] = {0};
    if (tny_runtime_host_monotonic_ms(runtime, &first, &error) != TNY_STATUS_OK ||
        tny_runtime_host_secure_random(runtime, native_random, sizeof native_random, &error) !=
            TNY_STATUS_OK ||
        tny_runtime_host_open_url(runtime, view("https://example.invalid"), &error) !=
            TNY_STATUS_UNSUPPORTED)
        return 17;
    tny_error_free(error);
    tny_runtime_free(runtime);

    puts("libtny-host-services: C ownership, order, failure and teardown passed");
    return 0;
}
