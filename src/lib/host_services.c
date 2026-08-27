#include "lib/host_services.h"

#include "util/util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct tny_host_services_state {
    tny_host_services_v1 services;
    bool in_callback;
    bool closing;
    bool have_last_ms;
    int64_t last_ms;
};

static int32_t callback_status(int32_t status) {
    if (status == TNY_STATUS_OK) return status;
    if (status <= TNY_STATUS_INVALID_ARGUMENT && status >= TNY_STATUS_INTERNAL) return status;
    return TNY_STATUS_INTERNAL;
}

static bool has_field(const tny_host_services_v1 *source, size_t offset, size_t size) {
    return source->struct_size >= offset + size;
}

int32_t tny_host_services_copy(const tny_host_services_v1 *source, tny_host_services_state **out) {
    if (!out) return TNY_STATUS_INVALID_ARGUMENT;
    *out = NULL;
    if (!source) return TNY_STATUS_OK;
    if (source->abi_version != TNY_HOST_SERVICES_ABI_VERSION ||
        source->struct_size < offsetof(tny_host_services_v1, diagnostic))
        return TNY_STATUS_INVALID_ARGUMENT;
    tny_host_services_state *state = calloc(1, sizeof *state);
    if (!state) return TNY_STATUS_OOM;
    state->services.abi_version = TNY_HOST_SERVICES_ABI_VERSION;
    state->services.struct_size = sizeof state->services;
    state->services.user_data = source->user_data;
#define COPY_CALLBACK(field)                                                                \
    do {                                                                                    \
        if (has_field(source, offsetof(tny_host_services_v1, field), sizeof source->field)) \
            state->services.field = source->field;                                          \
    } while (0)
    COPY_CALLBACK(diagnostic);
    COPY_CALLBACK(monotonic_ms);
    COPY_CALLBACK(secure_random);
    COPY_CALLBACK(storage_load);
    COPY_CALLBACK(storage_store);
    COPY_CALLBACK(open_url);
    COPY_CALLBACK(notify_scheduler);
#undef COPY_CALLBACK
    *out = state;
    return TNY_STATUS_OK;
}

void tny_host_services_free(tny_host_services_state *state) {
    if (!state) return;
    state->closing = true;
    memset(&state->services, 0, sizeof state->services);
    free(state);
}

void tny_host_services_close(tny_host_services_state *state) {
    if (state) state->closing = true;
}

bool tny_host_services_in_callback(const tny_host_services_state *state) {
    return state && state->in_callback;
}

static bool enter(tny_host_services_state *state) {
    if (!state || state->closing || state->in_callback) return false;
    state->in_callback = true;
    return true;
}

static void leave(tny_host_services_state *state) { state->in_callback = false; }

void tny_host_services_diagnostic(tny_host_services_state *state, uint32_t level,
                                  const char *message) {
    if (!state || !state->services.diagnostic || !enter(state)) return;
    static const char component[] = "libtny";
    tny_bytes component_view = {component, sizeof component - 1};
    tny_bytes message_view = {message, (uint64_t)strlen(message)};
    (void)state->services.diagnostic(state->services.user_data, level, component_view,
                                     message_view);
    leave(state);
}

int32_t tny_host_services_monotonic(tny_host_services_state *state, int64_t *out_ms) {
    if (!out_ms) return TNY_STATUS_INVALID_ARGUMENT;
    if (!state || !state->services.monotonic_ms) {
        int64_t value = monotonic_ms();
        if (state && state->have_last_ms && value < state->last_ms) value = state->last_ms;
        if (state) {
            state->last_ms = value;
            state->have_last_ms = true;
        }
        *out_ms = value;
        return TNY_STATUS_OK;
    }
    if (!enter(state)) return TNY_STATUS_BAD_STATE;
    int64_t value = 0;
    int32_t status =
        callback_status(state->services.monotonic_ms(state->services.user_data, &value));
    leave(state);
    if (status != TNY_STATUS_OK) return status;
    if (value < 0 || value > INT64_MAX - INT32_MAX ||
        (state->have_last_ms && value < state->last_ms))
        return TNY_STATUS_PROTOCOL;
    state->last_ms = value;
    state->have_last_ms = true;
    *out_ms = value;
    return TNY_STATUS_OK;
}

int64_t tny_host_services_monotonic_or_native(tny_host_services_state *state) {
    int64_t value = 0;
    if (tny_host_services_monotonic(state, &value) == TNY_STATUS_OK) return value;
    tny_host_services_diagnostic(state, TNY_DIAGNOSTIC_WARN,
                                 "host monotonic callback failed; native fallback used");
    int64_t native = monotonic_ms();
    if (state && state->have_last_ms && native < state->last_ms) native = state->last_ms;
    if (state) {
        state->last_ms = native;
        state->have_last_ms = true;
    }
    return native;
}

static int32_t native_random(void *buffer, uint64_t size) {
    if (size == 0) return TNY_STATUS_OK;
    if (!buffer || size > SIZE_MAX) return TNY_STATUS_INVALID_ARGUMENT;
    int fd;
    do fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    while (fd < 0 && errno == EINTR);
    if (fd < 0) return TNY_STATUS_IO;
    uint8_t *cursor = buffer;
    uint64_t remaining = size;
    while (remaining) {
        size_t chunk =
            remaining > (UINT64_C(1) << 30) ? (size_t)(UINT64_C(1) << 30) : (size_t)remaining;
        ssize_t got = read(fd, cursor, chunk);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            close(fd);
            return TNY_STATUS_IO;
        }
        cursor += got;
        remaining -= (uint64_t)got;
    }
    close(fd);
    return TNY_STATUS_OK;
}

int32_t tny_host_services_random(tny_host_services_state *state, void *buffer, uint64_t size) {
    if (size && !buffer) return TNY_STATUS_INVALID_ARGUMENT;
    if (!state || !state->services.secure_random) return native_random(buffer, size);
    if (!enter(state)) return TNY_STATUS_BAD_STATE;
    int32_t status =
        callback_status(state->services.secure_random(state->services.user_data, buffer, size));
    leave(state);
    return status;
}

int32_t tny_host_services_storage_load(tny_host_services_state *state, tny_bytes key,
                                       uint64_t *out_revision, void *buffer, uint64_t capacity,
                                       uint64_t *out_size) {
    if (!state || !state->services.storage_load) return TNY_STATUS_UNSUPPORTED;
    if (!out_revision || !out_size || (capacity && !buffer)) return TNY_STATUS_INVALID_ARGUMENT;
    if (!enter(state)) return TNY_STATUS_BAD_STATE;
    *out_size = 0;
    int32_t status = callback_status(state->services.storage_load(
        state->services.user_data, key, out_revision, buffer, capacity, out_size));
    leave(state);
    if (status == TNY_STATUS_OK && *out_size > capacity) return TNY_STATUS_PROTOCOL;
    return status;
}

int32_t tny_host_services_storage_store(tny_host_services_state *state, tny_bytes key,
                                        uint64_t expected_revision, const void *data, uint64_t size,
                                        uint64_t *out_revision) {
    if (!state || !state->services.storage_store) return TNY_STATUS_UNSUPPORTED;
    if (!out_revision || expected_revision == UINT64_MAX || (size && !data))
        return TNY_STATUS_INVALID_ARGUMENT;
    *out_revision = expected_revision;
    if (!enter(state)) return TNY_STATUS_BAD_STATE;
    int32_t status = callback_status(state->services.storage_store(
        state->services.user_data, key, expected_revision, data, size, out_revision));
    leave(state);
    if (status == TNY_STATUS_OK && *out_revision <= expected_revision) return TNY_STATUS_PROTOCOL;
    return status;
}

int32_t tny_host_services_open_url(tny_host_services_state *state, tny_bytes url) {
    if (!state || !state->services.open_url) return TNY_STATUS_UNSUPPORTED;
    if (!enter(state)) return TNY_STATUS_BAD_STATE;
    int32_t status = callback_status(state->services.open_url(state->services.user_data, url));
    leave(state);
    return status;
}

int32_t tny_host_services_notify(tny_host_services_state *state) {
    if (!state || !state->services.notify_scheduler) return TNY_STATUS_UNSUPPORTED;
    if (!enter(state)) return TNY_STATUS_BAD_STATE;
    int32_t status = callback_status(state->services.notify_scheduler(state->services.user_data));
    leave(state);
    return status;
}

void tny_host_services_notify_advisory(tny_host_services_state *state) {
    int32_t status = tny_host_services_notify(state);
    if (status != TNY_STATUS_OK && status != TNY_STATUS_UNSUPPORTED)
        tny_host_services_diagnostic(
            state, TNY_DIAGNOSTIC_WARN,
            "host scheduler notification failed; pull delivery remains active");
}
