/* Deterministic libtny public-capacity and custom-validator fuzz harness. */
#include "tny/tny.h"
#include "core/runtime.h"
#include "lib/custom_tools.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define TNY_FUZZ_ASAN 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#  define TNY_FUZZ_ASAN 1
#endif
#ifdef TNY_FUZZ_ASAN
#include <sanitizer/asan_interface.h>
#endif

#define REDZONE_SIZE 32u
#define REDZONE_BYTE UINT8_C(0xd7)

typedef struct {
    uint8_t *allocation;
    uint8_t *value;
    size_t capacity;
    size_t suffix_size;
} guarded_buffer;

static guarded_buffer guarded_new(size_t capacity, uint8_t fill) {
    size_t payload = capacity ? capacity : 1;
    guarded_buffer guard = {0};
    guard.allocation = malloc(REDZONE_SIZE + payload + REDZONE_SIZE);
    if (!guard.allocation) return guard;
    guard.value = guard.allocation + REDZONE_SIZE;
    guard.capacity = capacity;
    guard.suffix_size = REDZONE_SIZE + (capacity ? 0 : 1);
    memset(guard.allocation, REDZONE_BYTE, REDZONE_SIZE);
    if (capacity) memset(guard.value, fill, capacity);
    memset(guard.value + capacity, REDZONE_BYTE, guard.suffix_size);
#ifdef TNY_FUZZ_ASAN
    __asan_poison_memory_region(guard.allocation, REDZONE_SIZE);
    __asan_poison_memory_region(guard.value + capacity, guard.suffix_size);
#endif
    return guard;
}

static void guarded_free(guarded_buffer *guard) {
    if (!guard || !guard->allocation) return;
#ifdef TNY_FUZZ_ASAN
    __asan_unpoison_memory_region(guard->allocation, REDZONE_SIZE);
    __asan_unpoison_memory_region(
        guard->value + guard->capacity, guard->suffix_size);
#endif
    for (size_t index = 0; index < REDZONE_SIZE; index++)
        if (guard->allocation[index] != REDZONE_BYTE) abort();
    for (size_t index = 0; index < guard->suffix_size; index++)
        if (guard->value[guard->capacity + index] != REDZONE_BYTE) abort();
    free(guard->allocation);
    memset(guard, 0, sizeof *guard);
}

enum {
    CLASS_CAPACITY_OK = 1u << 0,
    CLASS_CAPACITY_REJECT = 1u << 1,
    CLASS_CREATE_OK = 1u << 2,
    CLASS_CREATE_REJECT = 1u << 3,
    CLASS_SCHEMA_OK = 1u << 4,
    CLASS_SCHEMA_REJECT = 1u << 5,
    CLASS_RESULT_OK = 1u << 6,
    CLASS_RESULT_REJECT = 1u << 7,
    CLASS_GENERATION_REJECT = 1u << 8,
    CLASS_REQUIRED = (1u << 9) - 1u,
};

static uint32_t observed_classes;

static void invariant(bool condition) {
    if (!condition) abort();
}

static uint8_t byte_at(const uint8_t *data, size_t size, size_t index) {
    return index < size ? data[index] : 0;
}

static uint64_t capacity_at(const uint8_t *data, size_t size, size_t index) {
    uint64_t value = byte_at(data, size, index);
    value |= (uint64_t)byte_at(data, size, index + 1) << 8;
    return value % 385u;
}

static uint64_t selected_capacity(const uint8_t *data, size_t size,
                                  size_t index) {
    switch (byte_at(data, size, index)) {
    case 0xfc: return UINT32_MAX;
    case 0xfd: return (uint64_t)UINT32_MAX + 1u;
    case 0xfe:
    case 0xff: return UINT64_MAX;
    default: return capacity_at(data, size, index);
    }
}

static void capacity_status(int32_t status) {
    invariant(status == TNY_STATUS_OK ||
              status == TNY_STATUS_INVALID_ARGUMENT);
    observed_classes |= status == TNY_STATUS_OK
        ? CLASS_CAPACITY_OK : CLASS_CAPACITY_REJECT;
}

#define EXERCISE_INIT(TYPE, FUNCTION, CAPACITY) do { \
    uint64_t selected_ = (CAPACITY); \
    if (selected_ == UINT32_MAX) { \
        guarded_buffer storage_ = guarded_new(sizeof(TYPE), 0xa5); \
        invariant(storage_.allocation != NULL); \
        int32_t status_ = FUNCTION((TYPE *)storage_.value, selected_); \
        invariant(status_ == TNY_STATUS_OK); \
        capacity_status(status_); \
        guarded_free(&storage_); \
    } else if (selected_ > UINT32_MAX) { \
        guarded_buffer storage_ = guarded_new(0, 0xa5); \
        invariant(storage_.allocation != NULL); \
        int32_t status_ = FUNCTION((TYPE *)storage_.value, selected_); \
        invariant(status_ == TNY_STATUS_INVALID_ARGUMENT); \
        capacity_status(status_); \
        guarded_free(&storage_); \
    } else { \
        guarded_buffer storage_ = guarded_new((size_t)selected_, 0xa5); \
        invariant(storage_.allocation != NULL); \
        capacity_status(FUNCTION((TYPE *)storage_.value, selected_)); \
        guarded_free(&storage_); \
    } \
} while (0)

static void exercise_initializers(const uint8_t *data, size_t size) {
    EXERCISE_INIT(tny_runtime_options_v0, tny_runtime_options_init,
                  selected_capacity(data, size, 0));
    EXERCISE_INIT(tny_runtime_options_v1, tny_runtime_options_v1_init,
                  selected_capacity(data, size, 2));
    EXERCISE_INIT(tny_host_services_v1, tny_host_services_v1_init,
                  selected_capacity(data, size, 4));
    EXERCISE_INIT(tny_tool_spec_v1, tny_tool_spec_v1_init,
                  selected_capacity(data, size, 6));
    EXERCISE_INIT(tny_tool_result_v1, tny_tool_result_v1_init,
                  selected_capacity(data, size, 8));
    EXERCISE_INIT(tny_capabilities_v0, tny_capabilities_init,
                  selected_capacity(data, size, 10));
    EXERCISE_INIT(tny_capabilities_v1, tny_capabilities_v1_init,
                  selected_capacity(data, size, 12));
    EXERCISE_INIT(tny_event_view_v0, tny_event_view_init,
                  selected_capacity(data, size, 14));
}

static tny_bytes bytes_of(const char *value) {
    return (tny_bytes){value, value ? (uint64_t)strlen(value) : 0};
}

static void valid_runtime_options(tny_runtime_options_v0 *options) {
    invariant(tny_runtime_options_init(options, sizeof *options) ==
              TNY_STATUS_OK);
    options->workspace = bytes_of(".");
    options->base_url = bytes_of("http://127.0.0.1:1/v1");
    options->api_key = bytes_of("fuzz-owned-key");
}

static guarded_buffer guarded_record(uint64_t capacity, size_t frozen_size,
                                     uint8_t fill) {
    invariant(capacity >= UINT32_MAX);
    return guarded_new(capacity == UINT32_MAX ? frozen_size : 0, fill);
}

static void expect_extreme_status(int32_t status, uint64_t capacity) {
    invariant(status == (capacity == UINT32_MAX
                         ? TNY_STATUS_OK : TNY_STATUS_INVALID_ARGUMENT));
    capacity_status(status);
}

/* Special selector coverage for every public record-capacity entry point.
 * UINT32_MAX receives exactly the frozen record size. Larger values receive a
 * non-NULL, wholly poisoned/canary-protected zero-capacity pointer, so an
 * implementation which inspects the record before rejecting is observable. */
static void exercise_extreme_capacity(unsigned endpoint, uint64_t capacity) {
    invariant(capacity >= UINT32_MAX);

    if (endpoint == 0) {
        tny_runtime_options_v0 full;
        valid_runtime_options(&full);
        guarded_buffer input = guarded_record(capacity, sizeof full, 0xa5);
        invariant(input.allocation != NULL);
        if (capacity == UINT32_MAX) memcpy(input.value, &full, sizeof full);
        tny_runtime *runtime = NULL;
        int32_t status = tny_runtime_create(
            (const tny_runtime_options_v0 *)input.value, capacity,
            &runtime, NULL);
        expect_extreme_status(status, capacity);
        guarded_free(&input);
        if (runtime) invariant(tny_runtime_destroy(&runtime) == TNY_STATUS_OK);
        return;
    }

    if (endpoint == 1) {
        tny_runtime_options_v1 full;
        invariant(tny_runtime_options_v1_init(&full, sizeof full) ==
                  TNY_STATUS_OK);
        valid_runtime_options(&full.runtime);
        guarded_buffer input = guarded_record(capacity, sizeof full, 0xa5);
        invariant(input.allocation != NULL);
        if (capacity == UINT32_MAX) memcpy(input.value, &full, sizeof full);
        tny_runtime *runtime = NULL;
        int32_t status = tny_runtime_create_v1(
            (const tny_runtime_options_v1 *)input.value, capacity,
            &runtime, NULL);
        expect_extreme_status(status, capacity);
        guarded_free(&input);
        if (runtime) invariant(tny_runtime_destroy(&runtime) == TNY_STATUS_OK);
        return;
    }

    if (endpoint == 4) {
        tny_event_view_v0 full;
        invariant(tny_event_view_init(&full, sizeof full) == TNY_STATUS_OK);
        guarded_buffer output = guarded_record(capacity, sizeof full, 0x5a);
        invariant(output.allocation != NULL);
        if (capacity == UINT32_MAX) memcpy(output.value, &full, sizeof full);
        tny_owned_event event;
        memset(&event, 0, sizeof event);
        int32_t status = tny_event_read(
            (const tny_event *)&event, (tny_event_view_v0 *)output.value,
            capacity);
        expect_extreme_status(status, capacity);
        guarded_free(&output);
        return;
    }

    tny_runtime_options_v0 options;
    valid_runtime_options(&options);
    tny_runtime *runtime = NULL;
    invariant(tny_runtime_create(&options, sizeof options, &runtime, NULL) ==
              TNY_STATUS_OK);
    invariant(runtime != NULL);
    if (endpoint == 2) {
        tny_capabilities_v0 full;
        invariant(tny_capabilities_init(&full, sizeof full) == TNY_STATUS_OK);
        guarded_buffer output = guarded_record(capacity, sizeof full, 0x5a);
        invariant(output.allocation != NULL);
        if (capacity == UINT32_MAX) memcpy(output.value, &full, sizeof full);
        int32_t status = tny_runtime_get_capabilities(
            runtime, (tny_capabilities_v0 *)output.value, capacity);
        expect_extreme_status(status, capacity);
        guarded_free(&output);
    } else {
        invariant(endpoint == 3);
        tny_capabilities_v1 full;
        invariant(tny_capabilities_v1_init(&full, sizeof full) ==
                  TNY_STATUS_OK);
        guarded_buffer output = guarded_record(capacity, sizeof full, 0x5a);
        invariant(output.allocation != NULL);
        if (capacity == UINT32_MAX) memcpy(output.value, &full, sizeof full);
        int32_t status = tny_runtime_get_capabilities_v1(
            runtime, (tny_capabilities_v1 *)output.value, capacity);
        expect_extreme_status(status, capacity);
        guarded_free(&output);
    }
    invariant(tny_runtime_destroy(&runtime) == TNY_STATUS_OK && !runtime);
}

static void exercise_runtime(const uint8_t *data, size_t size) {
    tny_runtime_options_v0 full;
    invariant(tny_runtime_options_init(&full, sizeof full) == TNY_STATUS_OK);
    full.workspace = bytes_of(".");
    full.base_url = bytes_of("http://127.0.0.1:1/v1");
    full.api_key = bytes_of("fuzz-owned-key");
    full.reserved[0] = UINT64_MAX;

    uint64_t capacity = (byte_at(data, size, 18) & 1u)
        ? capacity_at(data, size, 16) : sizeof full;
    guarded_buffer input = guarded_new((size_t)capacity, 0xa5);
    invariant(input.allocation != NULL);
    size_t copied = capacity < sizeof full ? (size_t)capacity : sizeof full;
    if (copied) memcpy(input.value, &full, copied);
    if (capacity >= sizeof(uint32_t)) {
        uint32_t declared = (uint32_t)capacity;
        memcpy(input.value, &declared, sizeof declared);
    }
    if ((byte_at(data, size, 19) & 3u) == 3u && capacity >= 8) {
        uint32_t unknown = UINT32_MAX;
        memcpy(input.value + 4, &unknown, sizeof unknown);
    }

    tny_runtime *runtime = NULL;
    int32_t status = tny_runtime_create(
        (tny_runtime_options_v0 *)input.value, capacity, &runtime, NULL);
    guarded_free(&input);
    invariant((status == TNY_STATUS_OK) == (runtime != NULL));
    if (status == TNY_STATUS_OK) {
        observed_classes |= CLASS_CREATE_OK;
        uint64_t query_capacity = (byte_at(data, size, 21) & 1u)
            ? capacity_at(data, size, 22) : sizeof(tny_capabilities_v0);
        guarded_buffer output = guarded_new((size_t)query_capacity, 0x5a);
        invariant(output.allocation != NULL);
        int32_t initialized = tny_capabilities_init(
            (tny_capabilities_v0 *)output.value, query_capacity);
        capacity_status(initialized);
        int32_t queried = tny_runtime_get_capabilities(
            runtime, (tny_capabilities_v0 *)output.value, query_capacity);
        invariant((queried == TNY_STATUS_OK) ==
                  (initialized == TNY_STATUS_OK));
        guarded_free(&output);
        uint64_t query_v1_capacity = (byte_at(data, size, 27) & 1u)
            ? capacity_at(data, size, 28) : sizeof(tny_capabilities_v1);
        guarded_buffer output_v1 = guarded_new(
            (size_t)query_v1_capacity, 0x6b);
        invariant(output_v1.allocation != NULL);
        int32_t initialized_v1 = tny_capabilities_v1_init(
            (tny_capabilities_v1 *)output_v1.value, query_v1_capacity);
        capacity_status(initialized_v1);
        int32_t queried_v1 = tny_runtime_get_capabilities_v1(
            runtime, (tny_capabilities_v1 *)output_v1.value,
            query_v1_capacity);
        invariant((queried_v1 == TNY_STATUS_OK) ==
                  (initialized_v1 == TNY_STATUS_OK));
        guarded_free(&output_v1);
        invariant(tny_runtime_destroy(&runtime) == TNY_STATUS_OK && !runtime);
    } else {
        invariant(status == TNY_STATUS_INVALID_ARGUMENT ||
                  status == TNY_STATUS_CONFIG || status == TNY_STATUS_OOM ||
                  status == TNY_STATUS_UNSUPPORTED);
        observed_classes |= CLASS_CREATE_REJECT;
    }

    tny_runtime_options_v1 full_v1;
    invariant(tny_runtime_options_v1_init(&full_v1, sizeof full_v1) ==
              TNY_STATUS_OK);
    full_v1.runtime = full;
    full_v1.reserved[0] = UINT64_MAX;
    uint64_t capacity_v1 = (byte_at(data, size, 30) & 1u)
        ? capacity_at(data, size, 31) : sizeof full_v1;
    guarded_buffer input_v1 = guarded_new((size_t)capacity_v1, 0xc3);
    invariant(input_v1.allocation != NULL);
    copied = capacity_v1 < sizeof full_v1
        ? (size_t)capacity_v1 : sizeof full_v1;
    if (copied) memcpy(input_v1.value, &full_v1, copied);
    if (capacity_v1 >= 2 * sizeof(uint32_t)) {
        uint32_t declared = (uint32_t)capacity_v1;
        memcpy(input_v1.value + sizeof(uint32_t), &declared, sizeof declared);
    }
    if ((byte_at(data, size, 33) & 3u) == 3u && capacity_v1 >= 4) {
        uint32_t unknown_abi = UINT32_MAX;
        memcpy(input_v1.value, &unknown_abi, sizeof unknown_abi);
    }
    runtime = NULL;
    status = tny_runtime_create_v1(
        (tny_runtime_options_v1 *)input_v1.value, capacity_v1, &runtime, NULL);
    guarded_free(&input_v1);
    invariant((status == TNY_STATUS_OK) == (runtime != NULL));
    if (status == TNY_STATUS_OK) {
        observed_classes |= CLASS_CREATE_OK;
        invariant(tny_runtime_destroy(&runtime) == TNY_STATUS_OK && !runtime);
    } else {
        invariant(status == TNY_STATUS_INVALID_ARGUMENT ||
                  status == TNY_STATUS_CONFIG || status == TNY_STATUS_OOM ||
                  status == TNY_STATUS_UNSUPPORTED);
        observed_classes |= CLASS_CREATE_REJECT;
    }

    uint64_t event_capacity = capacity_at(data, size, 24);
    guarded_buffer event_storage = guarded_new((size_t)event_capacity, 0x33);
    invariant(event_storage.allocation != NULL);
    int32_t event_init = tny_event_view_init(
        (tny_event_view_v0 *)event_storage.value, event_capacity);
    capacity_status(event_init);
    invariant(tny_event_read(NULL, (tny_event_view_v0 *)event_storage.value,
                             event_capacity) == TNY_STATUS_INVALID_ARGUMENT);
    guarded_free(&event_storage);
}

typedef struct {
    uint8_t mode;
    uint8_t result_bytes[64];
    uint64_t result_size;
    uint32_t result_abi;
    uint32_t result_struct_size;
    uint32_t result_is_error;
    bool null_result;
} callback_state;

static int32_t fuzz_invoke(void *opaque, tny_tool_call *call,
                           uint64_t generation, tny_bytes arguments,
                           tny_tool_result_v1 *result) {
    callback_state *state = opaque;
    invariant(call != NULL && generation != 0);
    invariant(arguments.ptr != NULL || arguments.len == 0);
    if (state->mode == 1) return TNY_TOOL_INVOKE_ASYNC;
    if (state->mode == 2) return 12345; /* invalid positive callback status */
    memset(result, 0, sizeof *result);
    result->abi_version = state->result_abi;
    result->struct_size = state->result_struct_size;
    result->data = (tny_bytes){
        state->null_result ? NULL : (const char *)state->result_bytes,
        state->result_size,
    };
    result->is_error = state->result_is_error;
    result->reserved_scalar = UINT32_MAX;
    result->reserved[0] = UINT64_MAX;
    return TNY_TOOL_INVOKE_SYNC;
}

static const char *schema_choice(uint8_t selector) {
    static const char *const schemas[] = {
        "{\"type\":\"object\"}",
        "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"string\"}},\"required\":[\"value\"]}",
        "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"string\",\"pattern\":\".*\"}}}",
        "{\"type\":",
        "{\"type\":\"object\",\"properties\":{\"nested\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"string\"}}}}}",
        "{\"type\":\"array\"}",
        "{\"type\":\"object\",\"properties\":{\"items\":{\"type\":\"array\"}},\"additionalProperties\":false}",
    };
    return schemas[selector % (sizeof schemas / sizeof schemas[0])];
}

static void note_result_status(int32_t status) {
    if (status == TNY_STATUS_OK) observed_classes |= CLASS_RESULT_OK;
    else if (status == TNY_STATUS_INVALID_ARGUMENT ||
             status == TNY_STATUS_BACKPRESSURE ||
             status == TNY_STATUS_INTERNAL)
        observed_classes |= CLASS_RESULT_REJECT;
    else invariant(status == TNY_STATUS_OOM || status == TNY_STATUS_BAD_STATE);
}

static void exercise_custom_tools(const uint8_t *data, size_t size) {
    custom_tool_registry *registry = custom_tools_new();
    if (!registry) return;

    callback_state state;
    memset(&state, 0, sizeof state);
    state.mode = byte_at(data, size, 1) % 3u;
    state.result_size = byte_at(data, size, 6) % 33u;
    state.result_abi = (byte_at(data, size, 8) & 1u)
        ? UINT32_MAX : TNY_TOOL_RESULT_ABI_VERSION;
    state.result_struct_size = (byte_at(data, size, 9) & 1u)
        ? byte_at(data, size, 10) : (uint32_t)sizeof(tny_tool_result_v1);
    state.result_is_error = byte_at(data, size, 11) % 3u;
    state.null_result = (byte_at(data, size, 7) & 1u) != 0;
    for (size_t index = 0; index < sizeof state.result_bytes; index++)
        state.result_bytes[index] = byte_at(data, size, 32 + index);

    tny_tool_spec_v1 spec;
    invariant(tny_tool_spec_v1_init(&spec, sizeof spec) == TNY_STATUS_OK);
    uint8_t schema_selector = byte_at(data, size, 0) % 7u;
    uint8_t name_selector = byte_at(data, size, 3) % 3u;
    uint32_t sensitivity = byte_at(data, size, 4) % 4u;
    bool raw_schema = size && data[0] == '{';
    bool schema_must_reject = !raw_schema && schema_selector >= 2u &&
                              schema_selector <= 5u;
    bool descriptor_otherwise_valid = name_selector == 0u && sensitivity <= 1u;
    const char *schema = schema_choice(schema_selector);
    const char *name = name_selector == 1u
        ? "list_files" : "fuzz_tool";
    spec.user_data = &state;
    spec.name = name_selector == 2u
        ? (tny_bytes){(const char *)data,
                      size < 65 ? (uint64_t)size : UINT64_C(65)}
        : bytes_of(name);
    spec.description = bytes_of("owned fuzz callback");
    spec.input_schema_json = raw_schema
        ? (tny_bytes){(const char *)data, (uint64_t)size}
        : bytes_of(schema);
    spec.sensitivity = sensitivity;
    spec.max_argument_bytes = 16;
    spec.max_result_bytes = 16;
    spec.invoke = fuzz_invoke;
    spec.reserved_scalar = UINT32_MAX;
    spec.reserved[0] = UINT64_MAX;

    tny_tool_registration *registration = NULL;
    int32_t registered = custom_tools_register(
        registry, &state, &spec, &registration);
    if (schema_must_reject && descriptor_otherwise_valid)
        invariant(registered == TNY_STATUS_INVALID_ARGUMENT);
    if (registered != TNY_STATUS_OK) {
        invariant(!registration);
        invariant(registered == TNY_STATUS_INVALID_ARGUMENT ||
                  registered == TNY_STATUS_OOM);
        if (schema_must_reject && descriptor_otherwise_valid)
            observed_classes |= CLASS_SCHEMA_REJECT;
        custom_tools_free(registry);
        return;
    }
    invariant(!schema_must_reject);
    observed_classes |= CLASS_SCHEMA_OK;

    char arguments[20];
    size_t arguments_size = size < sizeof arguments - 1 ? size : sizeof arguments - 1;
    for (size_t index = 0; index < arguments_size; index++)
        arguments[index] = (char)byte_at(data, size, index);
    arguments[arguments_size] = 0;
    tny_tool_call *call = NULL;
    char *result = NULL;
    bool is_error = false;
    int32_t invoked = custom_tool_invoke(
        registration, arguments, &call, &result, &is_error);
    if (invoked == TNY_TOOL_INVOKE_ASYNC) {
        invariant(call != NULL && result == NULL);
        uint64_t generation = tny_tool_call_generation(call);
        tny_tool_result_v1 completion;
        memset(&completion, 0, sizeof completion);
        completion.abi_version = state.result_abi;
        completion.struct_size = state.result_struct_size;
        completion.data = (tny_bytes){
            state.null_result ? NULL : (const char *)state.result_bytes,
            state.result_size,
        };
        completion.is_error = state.result_is_error;
        completion.reserved_scalar = UINT32_MAX;
        completion.reserved[0] = UINT64_MAX;
        invariant(custom_tool_complete(call, generation + 1, &completion) ==
                  TNY_STATUS_BAD_STATE);
        observed_classes |= CLASS_GENERATION_REJECT;
        int32_t completed = custom_tool_complete(call, generation, &completion);
        note_result_status(completed);
        if (completed != TNY_STATUS_OK) {
            memset(&completion, 0, sizeof completion);
            completion.abi_version = TNY_TOOL_RESULT_ABI_VERSION;
            completion.struct_size = sizeof completion;
            completion.data = bytes_of("ok");
            completed = custom_tool_complete(call, generation, &completion);
            invariant(completed == TNY_STATUS_OK);
            observed_classes |= CLASS_RESULT_OK;
        }
        invariant(custom_tool_complete(call, generation, &completion) ==
                  TNY_STATUS_BAD_STATE);
        observed_classes |= CLASS_GENERATION_REJECT;
        invariant(custom_tool_take(call, &result, &is_error) == 1);
        free(result);
        tny_tool_call_release(call);
    } else {
        invariant(call == NULL);
        note_result_status(invoked);
        free(result);
    }
    invariant(custom_tools_unregister(registration) == TNY_STATUS_OK);
    custom_tools_free(registry);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    exercise_initializers(data, size);
    uint64_t extreme = selected_capacity(data, size, 95);
    if (extreme >= UINT32_MAX)
        exercise_extreme_capacity(byte_at(data, size, 94) % 5u, extreme);
    exercise_runtime(data, size);
    exercise_custom_tools(data, size);
    return 0;
}

#ifdef TNY_FUZZ_STANDALONE
static int read_file(const char *path, uint8_t **out, size_t *out_size) {
    *out = NULL;
    *out_size = 0;
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return -1; }
    long end = ftell(file);
    if (end < 0 || end > (1 << 20) || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    uint8_t *data = malloc((size_t)end + 1);
    if (!data) { fclose(file); return -1; }
    if (end && fread(data, 1, (size_t)end, file) != (size_t)end) {
        free(data); fclose(file); return -1;
    }
    fclose(file);
    *out = data;
    *out_size = (size_t)end;
    return 0;
}

static int self_test(bool negative) {
    uint8_t valid[96] = {0};
    observed_classes = 0;
    LLVMFuzzerTestOneInput(valid, sizeof valid);
    if (!negative) {
        for (uint64_t capacity = 0; capacity <= 384; capacity++) {
            EXERCISE_INIT(tny_runtime_options_v0, tny_runtime_options_init,
                          capacity);
        }
        static const uint64_t huge_capacities[] = {
            UINT32_MAX, (uint64_t)UINT32_MAX + 1u, UINT64_MAX,
        };
        for (size_t index = 0;
             index < sizeof huge_capacities / sizeof huge_capacities[0];
             index++) {
            EXERCISE_INIT(tny_runtime_options_v0, tny_runtime_options_init,
                          huge_capacities[index]);
            for (unsigned endpoint = 0; endpoint < 5; endpoint++)
                exercise_extreme_capacity(endpoint, huge_capacities[index]);
        }
        uint8_t invalid[96];
        memset(invalid, 0xff, sizeof invalid);
        LLVMFuzzerTestOneInput(invalid, sizeof invalid);
        uint8_t schema_invalid[96] = {0};
        schema_invalid[0] = 2;
        LLVMFuzzerTestOneInput(schema_invalid, sizeof schema_invalid);
        uint8_t async_invalid[96] = {0};
        async_invalid[0] = 1;
        async_invalid[1] = 1;
        async_invalid[6] = 20;
        LLVMFuzzerTestOneInput(async_invalid, sizeof async_invalid);
    }
    fprintf(stderr, "fuzz classes: 0x%03x required: 0x%03x\n",
            observed_classes, CLASS_REQUIRED);
    return observed_classes == CLASS_REQUIRED ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return self_test(false);
    if (argc == 2 && strcmp(argv[1], "--negative-self-test") == 0)
        return self_test(true);
    for (int index = 1; index < argc; index++) {
        uint8_t *data = NULL;
        size_t size = 0;
        if (read_file(argv[index], &data, &size) != 0) return 2;
        LLVMFuzzerTestOneInput(data, size);
        free(data);
    }
    return 0;
}
#endif
