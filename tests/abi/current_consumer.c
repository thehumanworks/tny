#include "tny/tny.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static tny_bytes bytes(const char *value) {
    tny_bytes result = {value, value ? (uint64_t)strlen(value) : 0};
    return result;
}

static int boundary(uint64_t size, uint32_t frozen, const uint32_t *values, size_t count) {
    if (size >= frozen) return 1;
    for (size_t index = 0; index < count; index++)
        if (size == values[index]) return 1;
    return 0;
}

static int create_boundaries(const char *workspace) {
    static const uint32_t values[] = {40, 56, 72, 88, 104, 120, 136, 200};
    tny_runtime_options_v0 full;
    if (tny_runtime_options_init(&full, sizeof full) != TNY_STATUS_OK) return 1;
    full.workspace = bytes(workspace);
    full.base_url = bytes("http://127.0.0.1:1/v1");
    full.api_key = bytes("boundary-key");
    for (uint32_t capacity = 40; capacity <= 200; capacity++) {
        union {
            max_align_t align;
            unsigned char bytes[200];
        } storage;
        memset(storage.bytes, 0xa5, sizeof storage.bytes);
        memcpy(storage.bytes, &full, capacity);
        memcpy(storage.bytes, &capacity, sizeof capacity);
        tny_runtime *runtime = NULL;
        int32_t status =
            tny_runtime_create((tny_runtime_options_v0 *)storage.bytes, capacity, &runtime, NULL);
        int valid = boundary(capacity, 200, values, sizeof values / sizeof values[0]);
        if ((status == TNY_STATUS_OK) != valid || (!!runtime) != valid) return 2;
        if (runtime && tny_runtime_destroy(&runtime) != TNY_STATUS_OK) return 3;
    }
    return 0;
}

static int query_boundaries(tny_runtime *runtime) {
    static const uint32_t values[] = {32, 40,  48,  56,  60,  64,  72, 80,
                                      96, 112, 128, 144, 160, 176, 240};
    for (uint32_t capacity = 32; capacity <= 240; capacity++) {
        union {
            max_align_t align;
            unsigned char bytes[240];
        } storage;
        memset(storage.bytes, 0xa5, sizeof storage.bytes);
        memcpy(storage.bytes, &capacity, sizeof capacity);
        int valid = boundary(capacity, 240, values, sizeof values / sizeof values[0]);
        if (valid &&
            tny_capabilities_init((tny_capabilities_v0 *)storage.bytes, capacity) != TNY_STATUS_OK)
            return 1;
        int32_t status =
            tny_runtime_get_capabilities(runtime, (tny_capabilities_v0 *)storage.bytes, capacity);
        if ((status == TNY_STATUS_OK) != valid) return 2;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3 || tny_abi_version() != UINT32_C(65537)) return 1;
    if (create_boundaries(argv[1]) != 0) return 2;

    union {
        max_align_t alignment;
        unsigned char bytes[sizeof(tny_runtime_options_v0) + 16];
    } prefix;
    memset(prefix.bytes, 0xa5, sizeof prefix.bytes);
    if (tny_runtime_options_init((tny_runtime_options_v0 *)prefix.bytes, 40) != TNY_STATUS_OK)
        return 3;
    for (size_t i = 40; i < sizeof prefix.bytes; i++)
        if (prefix.bytes[i] != 0xa5) return 4;

    tny_runtime_options_v0 options;
    if (tny_runtime_options_init(&options, sizeof options) != TNY_STATUS_OK) return 5;
    options.workspace = bytes(argv[1]);
    options.base_url = bytes(argv[2]);
    options.api_key = bytes("abi1-fixture-key");
    options.reserved[0] = UINT64_C(0xfeed);
    tny_runtime *runtime = NULL;
    if (tny_runtime_create(&options, sizeof options, &runtime, NULL) != TNY_STATUS_OK || !runtime)
        return 6;
    if (query_boundaries(runtime) != 0) return 7;

    union {
        max_align_t alignment;
        unsigned char bytes[32];
    } cap_prefix;
    if (tny_capabilities_init((tny_capabilities_v0 *)cap_prefix.bytes, sizeof cap_prefix.bytes) !=
        TNY_STATUS_OK)
        return 8;
    tny_capabilities_v0 *capabilities = (tny_capabilities_v0 *)cap_prefix.bytes;
    if (tny_runtime_get_capabilities(runtime, capabilities, sizeof cap_prefix.bytes) !=
            TNY_STATUS_OK ||
        capabilities->abi_version != UINT32_C(65537))
        return 9;
    tny_capabilities_v0 full;
    if (tny_capabilities_init(&full, sizeof full) != TNY_STATUS_OK ||
        tny_runtime_get_capabilities(runtime, &full, sizeof full) != TNY_STATUS_OK ||
        !(full.feature_available_mask & TNY_CAP_FEATURE_SHARED_LIBRARY) ||
        !(full.feature_enabled_mask & TNY_CAP_FEATURE_SHARED_LIBRARY) ||
        (full.feature_available_mask & TNY_CAP_FEATURE_STATIC_LIBRARY) || full.linkage.len != 6 ||
        memcmp(full.linkage.ptr, "shared", 6) != 0)
        return 10;
    if (tny_runtime_destroy(&runtime) != TNY_STATUS_OK || runtime ||
        tny_runtime_destroy(&runtime) != TNY_STATUS_OK)
        return 11;
    return 0;
}
