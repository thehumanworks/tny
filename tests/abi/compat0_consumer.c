#include "tny/tny.h"

#include <stdint.h>
#include <string.h>

static tny_bytes bytes(const char *value) {
    tny_bytes result = {value, value ? (uint64_t)strlen(value) : 0};
    return result;
}

int main(int argc, char **argv) {
    if (argc != 3 || tny_abi_version() != UINT32_C(8)) return 1;
    tny_runtime_options_v0 options;
    tny_runtime_options_init(&options);
    if (options.struct_size != UINT32_C(200)) return 2;
    options.workspace = bytes(argv[1]);
    options.base_url = bytes(argv[2]);
    options.api_key = bytes("abi0-fixture-key");
    tny_runtime *runtime = NULL;
    if (tny_runtime_create(&options, &runtime, NULL) != TNY_STATUS_OK || !runtime)
        return 3;
    tny_capabilities_v0 capabilities;
    tny_capabilities_init(&capabilities);
    if (tny_runtime_get_capabilities(runtime, &capabilities) != TNY_STATUS_OK ||
        capabilities.abi_version != UINT32_C(8))
        return 4;
    if (tny_runtime_destroy(&runtime) != TNY_STATUS_OK || runtime) return 5;
    return 0;
}
