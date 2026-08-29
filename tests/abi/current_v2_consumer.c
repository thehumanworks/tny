#include "tny/tny.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static tny_bytes bytes(const char *value) {
    tny_bytes result = {value, value ? (uint64_t)strlen(value) : 0};
    return result;
}

int main(int argc, char **argv) {
    if (argc != 3) return 1;
    tny_runtime_options_v2 options;
    if (tny_runtime_options_v2_init(&options, sizeof options) != TNY_STATUS_OK) return 2;
    options.base.runtime.workspace = bytes(argv[1]);
    options.base.runtime.base_url = bytes(argv[2]);
    options.base.runtime.api_key = bytes("abi1-v2-fixture-key");
    options.task.name = bytes("custom");
    options.task.instructions = bytes("custom task body");
    tny_runtime *runtime = NULL;
    if (tny_runtime_create_v2(&options, sizeof options, &runtime, NULL) != TNY_STATUS_OK ||
        !runtime)
        return 3;
    tny_capabilities_v0 capabilities;
    if (tny_capabilities_init(&capabilities, sizeof capabilities) != TNY_STATUS_OK ||
        tny_runtime_get_capabilities(runtime, &capabilities, sizeof capabilities) !=
            TNY_STATUS_OK ||
        !(capabilities.feature_available_mask & TNY_CAP_FEATURE_TASK_PRESETS) ||
        !(capabilities.feature_enabled_mask & TNY_CAP_FEATURE_TASK_PRESETS))
        return 8;
    if (tny_runtime_destroy(&runtime) != TNY_STATUS_OK) return 4;

    /* Empty instructions are the built-in shorthand. Reserved words are
     * opaque and cannot change the meaning of the known prefix. */
    options.task.name = bytes("review");
    options.task.instructions = bytes(NULL);
    memset(options.base.reserved, 0xa5, sizeof options.base.reserved);
    memset(options.task.reserved, 0xa5, sizeof options.task.reserved);
    memset(options.reserved, 0xa5, sizeof options.reserved);
    if (tny_runtime_create_v2(&options, sizeof options, &runtime, NULL) != TNY_STATUS_OK ||
        !runtime || tny_runtime_destroy(&runtime) != TNY_STATUS_OK)
        return 9;

    /* The documented minimum outer prefix ends after the complete task
     * record and must be safe for an exact-size allocation. */
    tny_runtime_options_v2 *minimum = malloc(360u);
    if (!minimum || tny_runtime_options_v2_init(minimum, 360u) != TNY_STATUS_OK) return 10;
    minimum->base.runtime.workspace = bytes(argv[1]);
    minimum->base.runtime.base_url = bytes(argv[2]);
    minimum->base.runtime.api_key = bytes("abi1-v2-minimum-key");
    minimum->task.name = bytes("review");
    if (tny_runtime_create_v2(minimum, 360u, &runtime, NULL) != TNY_STATUS_OK || !runtime ||
        tny_runtime_destroy(&runtime) != TNY_STATUS_OK) {
        free(minimum);
        return 11;
    }
    free(minimum);

    if (tny_runtime_create_v2(&options, sizeof options, NULL, NULL) != TNY_STATUS_INVALID_ARGUMENT)
        return 12;

    options.task.name = bytes("bad/name");
    if (tny_runtime_create_v2(&options, sizeof options, &runtime, NULL) !=
            TNY_STATUS_INVALID_ARGUMENT ||
        runtime)
        return 5;
    options.task.name = bytes("custom");
    options.task.instructions = (tny_bytes){"x", UINT64_C(262145)};
    if (tny_runtime_create_v2(&options, sizeof options, &runtime, NULL) !=
            TNY_STATUS_INVALID_ARGUMENT ||
        runtime)
        return 6;
    options.task.instructions = bytes("custom task body");
    options.task.abi_version++;
    if (tny_runtime_create_v2(&options, sizeof options, &runtime, NULL) != TNY_STATUS_UNSUPPORTED ||
        runtime)
        return 7;
    options.task.abi_version--;
    options.abi_version++;
    if (tny_runtime_create_v2(&options, sizeof options, &runtime, NULL) != TNY_STATUS_UNSUPPORTED ||
        runtime)
        return 13;
    options.abi_version--;
    options.base.abi_version++;
    if (tny_runtime_create_v2(&options, sizeof options, &runtime, NULL) != TNY_STATUS_UNSUPPORTED ||
        runtime)
        return 14;
    options.base.abi_version--;
    options.task.struct_size = 39u;
    if (tny_runtime_create_v2(&options, sizeof options, &runtime, NULL) !=
            TNY_STATUS_INVALID_ARGUMENT ||
        runtime)
        return 15;
    options.task.struct_size = (uint32_t)sizeof options.task;
    options.task.name = (tny_bytes){"custom\0suffix", 13u};
    if (tny_runtime_create_v2(&options, sizeof options, &runtime, NULL) !=
            TNY_STATUS_INVALID_ARGUMENT ||
        runtime)
        return 16;
    options.task.name = bytes("custom");
    options.task.instructions = (tny_bytes){"\xff", 1u};
    if (tny_runtime_create_v2(&options, sizeof options, &runtime, NULL) !=
            TNY_STATUS_INVALID_ARGUMENT ||
        runtime)
        return 17;
    return 0;
}
