#include "tny/tny.h"

#include <stdint.h>

int main(void) {
    tny_runtime_options_v0 options;
    tny_capabilities_v0 capabilities;
    if (tny_runtime_options_init(&options, sizeof options) != TNY_STATUS_OK)
        return 1;
    if (tny_capabilities_init(&capabilities, sizeof capabilities) !=
        TNY_STATUS_OK)
        return 2;
    return tny_abi_version() == UINT32_C(65536) ? 0 : 3;
}
