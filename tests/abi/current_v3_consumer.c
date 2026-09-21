#include "tny/tny.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static tny_bytes bytes(const char *value) {
    tny_bytes result = {value, value ? (uint64_t)strlen(value) : 0};
    return result;
}

static uint64_t enabled(tny_runtime *runtime) {
    tny_capabilities_v0 capabilities;
    if (tny_capabilities_init(&capabilities, sizeof capabilities) != TNY_STATUS_OK ||
        tny_runtime_get_capabilities(runtime, &capabilities, sizeof capabilities) !=
            TNY_STATUS_OK ||
        !(capabilities.feature_available_mask & TNY_CAP_FEATURE_REASONING_EFFORT))
        return UINT64_MAX;
    return capabilities.feature_enabled_mask;
}

int main(int argc, char **argv) {
    if (argc != 3) return 1;
    tny_runtime_options_v3 options;
    if (tny_runtime_options_v3_init(&options, sizeof options) != TNY_STATUS_OK) return 2;
    if (options.inference.abi_version != TNY_INFERENCE_OPTIONS_ABI_VERSION ||
        options.inference.struct_size != sizeof options.inference ||
        options.base.struct_size != sizeof options.base)
        return 3;
    options.base.base.runtime.workspace = bytes(argv[1]);
    options.base.base.runtime.base_url = bytes(argv[2]);
    options.base.base.runtime.api_key = bytes("abi1-v3-fixture-key");

    /* Unlike v2, neither a task nor an effort is required. */
    tny_runtime *runtime = NULL;
    if (tny_runtime_create_v3(&options, sizeof options, &runtime, NULL) != TNY_STATUS_OK ||
        !runtime)
        return 4;
    uint64_t mask = enabled(runtime);
    if (mask == UINT64_MAX || (mask & TNY_CAP_FEATURE_REASONING_EFFORT) ||
        (mask & TNY_CAP_FEATURE_TASK_PRESETS))
        return 5;
    if (tny_runtime_destroy(&runtime) != TNY_STATUS_OK) return 6;

    /* Effort alone, then effort with a task; reserved words stay opaque. */
    options.inference.reasoning_effort = bytes("high");
    if (tny_runtime_create_v3(&options, sizeof options, &runtime, NULL) != TNY_STATUS_OK ||
        !runtime)
        return 7;
    mask = enabled(runtime);
    if (mask == UINT64_MAX || !(mask & TNY_CAP_FEATURE_REASONING_EFFORT) ||
        (mask & TNY_CAP_FEATURE_TASK_PRESETS))
        return 8;
    if (tny_runtime_destroy(&runtime) != TNY_STATUS_OK) return 9;
    options.base.task.name = bytes("review");
    options.inference.reasoning_effort = bytes("provider-token_1.5");
    memset(options.base.reserved, 0xa5, sizeof options.base.reserved);
    memset(options.inference.reserved, 0xa5, sizeof options.inference.reserved);
    memset(options.reserved, 0xa5, sizeof options.reserved);
    if (tny_runtime_create_v3(&options, sizeof options, &runtime, NULL) != TNY_STATUS_OK ||
        !runtime)
        return 10;
    mask = enabled(runtime);
    if (mask == UINT64_MAX || !(mask & TNY_CAP_FEATURE_REASONING_EFFORT) ||
        !(mask & TNY_CAP_FEATURE_TASK_PRESETS))
        return 11;
    if (tny_runtime_destroy(&runtime) != TNY_STATUS_OK) return 12;

    /* The documented minimum prefix ends after the inference record. */
    void *minimum = malloc(504u);
    if (!minimum || tny_runtime_options_v3_init(minimum, 504u) != TNY_STATUS_OK) return 13;
    tny_runtime_options_v3 minimum_fields = {0};
    memcpy(&minimum_fields, minimum, 504u);
    minimum_fields.base.base.runtime.workspace = bytes(argv[1]);
    minimum_fields.base.base.runtime.base_url = bytes(argv[2]);
    minimum_fields.base.base.runtime.api_key = bytes("abi1-v3-minimum-key");
    minimum_fields.inference.reasoning_effort = bytes("light");
    memcpy(minimum, &minimum_fields, 504u);
    if (tny_runtime_create_v3(minimum, 504u, &runtime, NULL) != TNY_STATUS_OK || !runtime ||
        tny_runtime_destroy(&runtime) != TNY_STATUS_OK) {
        free(minimum);
        return 14;
    }
    free(minimum);

    if (tny_runtime_create_v3(&options, sizeof options, NULL, NULL) != TNY_STATUS_INVALID_ARGUMENT)
        return 15;

    /* Malformed effort tokens fail before any runtime exists. */
    static const char *const invalid[] = {"hi gh", "high\n", "a/b", "\"high\"",
                                          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        options.inference.reasoning_effort = bytes(invalid[i]);
        if (tny_runtime_create_v3(&options, sizeof options, &runtime, NULL) !=
                TNY_STATUS_INVALID_ARGUMENT ||
            runtime)
            return 16;
    }
    options.inference.reasoning_effort = (tny_bytes){"high\0x", 6u};
    if (tny_runtime_create_v3(&options, sizeof options, &runtime, NULL) !=
            TNY_STATUS_INVALID_ARGUMENT ||
        runtime)
        return 17;
    options.inference.reasoning_effort = (tny_bytes){NULL, 4u};
    if (tny_runtime_create_v3(&options, sizeof options, &runtime, NULL) !=
            TNY_STATUS_INVALID_ARGUMENT ||
        runtime)
        return 18;
    options.inference.reasoning_effort = bytes("high");

    /* A task body without a name is a caller error, not an ignored field. */
    options.base.task.name = bytes(NULL);
    options.base.task.instructions = bytes("orphan body");
    if (tny_runtime_create_v3(&options, sizeof options, &runtime, NULL) !=
            TNY_STATUS_INVALID_ARGUMENT ||
        runtime)
        return 19;
    options.base.task.instructions = bytes(NULL);

    options.inference.abi_version++;
    if (tny_runtime_create_v3(&options, sizeof options, &runtime, NULL) != TNY_STATUS_UNSUPPORTED ||
        runtime)
        return 20;
    options.inference.abi_version--;
    options.abi_version++;
    if (tny_runtime_create_v3(&options, sizeof options, &runtime, NULL) != TNY_STATUS_UNSUPPORTED ||
        runtime)
        return 21;
    options.abi_version--;
    options.base.abi_version++;
    if (tny_runtime_create_v3(&options, sizeof options, &runtime, NULL) != TNY_STATUS_UNSUPPORTED ||
        runtime)
        return 22;
    options.base.abi_version--;
    options.inference.struct_size = 23u;
    if (tny_runtime_create_v3(&options, sizeof options, &runtime, NULL) !=
            TNY_STATUS_INVALID_ARGUMENT ||
        runtime)
        return 23;
    options.inference.struct_size = (uint32_t)sizeof options.inference;
    options.base.struct_size = 359u;
    if (tny_runtime_create_v3(&options, sizeof options, &runtime, NULL) !=
            TNY_STATUS_INVALID_ARGUMENT ||
        runtime)
        return 24;
    return 0;
}
