#include "tny/tny.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int allowed(uint64_t size, uint32_t frozen, const uint32_t *boundaries, size_t count) {
    if (size >= frozen) return 1;
    for (size_t index = 0; index < count; index++)
        if (size == boundaries[index]) return 1;
    return 0;
}

#define CHECK(TYPE, FUNCTION, FROZEN, SIZE_OFFSET, BOUNDARIES)                                   \
    do {                                                                                         \
        for (uint64_t capacity = 0; capacity <= (FROZEN) + 8; capacity++) {                      \
            union {                                                                              \
                max_align_t align;                                                               \
                unsigned char bytes[(FROZEN) + 8];                                               \
            } storage;                                                                           \
            memset(storage.bytes, 0xa5, sizeof storage.bytes);                                   \
            int32_t status = FUNCTION((TYPE *)storage.bytes, capacity);                          \
            int expected =                                                                       \
                allowed(capacity, FROZEN, BOUNDARIES, sizeof BOUNDARIES / sizeof BOUNDARIES[0]); \
            if ((status == TNY_STATUS_OK) != expected) return __LINE__;                          \
            if (!expected) {                                                                     \
                for (size_t i = 0; i < sizeof storage.bytes; i++)                                \
                    if (storage.bytes[i] != 0xa5) return __LINE__;                               \
            } else {                                                                             \
                uint32_t declared = 0;                                                           \
                memcpy(&declared, storage.bytes + (SIZE_OFFSET), sizeof declared);               \
                if (declared != capacity) return __LINE__;                                       \
                size_t touched = capacity < (FROZEN) ? (size_t)capacity : (FROZEN);              \
                for (size_t i = touched; i < sizeof storage.bytes; i++)                          \
                    if (storage.bytes[i] != 0xa5) return __LINE__;                               \
            }                                                                                    \
        }                                                                                        \
    } while (0)

int main(void) {
    static const uint32_t options0[] = {40, 56, 72, 88, 104, 120, 136, 200};
    static const uint32_t options1[] = {208, 216, 280};
    static const uint32_t task1[] = {40, 72};
    static const uint32_t options2[] = {360, 424};
    static const uint32_t host1[] = {16, 24, 32, 40, 48, 56, 64, 72, 136};
    static const uint32_t spec1[] = {96, 160};
    static const uint32_t result1[] = {32, 64};
    static const uint32_t caps0[] = {32, 40,  48,  56,  60,  64,  72, 80,
                                     96, 112, 128, 144, 160, 176, 240};
    static const uint32_t caps1[] = {248, 252, 256, 264, 272, 280, 344};
    static const uint32_t event0[] = {32,  40,  48,  56,  64,  72,  80,  88,  104, 120,
                                      136, 152, 168, 184, 200, 216, 232, 248, 264, 328};
    CHECK(tny_runtime_options_v0, tny_runtime_options_init, 200, 0, options0);
    CHECK(tny_runtime_options_v1, tny_runtime_options_v1_init, 280, 4, options1);
    CHECK(tny_task_options_v1, tny_task_options_v1_init, 72, 4, task1);
    CHECK(tny_runtime_options_v2, tny_runtime_options_v2_init, 424, 4, options2);
    CHECK(tny_host_services_v1, tny_host_services_v1_init, 136, 4, host1);
    CHECK(tny_tool_spec_v1, tny_tool_spec_v1_init, 160, 4, spec1);
    CHECK(tny_tool_result_v1, tny_tool_result_v1_init, 64, 4, result1);
    CHECK(tny_capabilities_v0, tny_capabilities_init, 240, 0, caps0);
    CHECK(tny_capabilities_v1, tny_capabilities_v1_init, 344, 4, caps1);
    CHECK(tny_event_view_v0, tny_event_view_init, 328, 0, event0);
    /* The sentinel matrix above detects writes past capacity. These exact
     * allocations additionally turn any regression into an ASan failure in
     * sanitizer-enabled ABI runs. */
    for (size_t i = 0; i < sizeof options2 / sizeof options2[0]; i++) {
        void *exact = malloc(options2[i]);
        if (!exact || tny_runtime_options_v2_init(exact, options2[i]) != TNY_STATUS_OK) return 1;
        free(exact);
    }
    for (size_t i = 0; i < sizeof task1 / sizeof task1[0]; i++) {
        void *exact = malloc(task1[i]);
        if (!exact || tny_task_options_v1_init(exact, task1[i]) != TNY_STATUS_OK) return 1;
        free(exact);
    }
    return 0;
}
