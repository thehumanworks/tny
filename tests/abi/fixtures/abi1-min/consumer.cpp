#include "tny/tny.h"

#include <cstdint>

int main() {
    tny_runtime_options_v0 options{};
    return tny_runtime_options_init(&options, sizeof options) == TNY_STATUS_OK &&
           tny_abi_version() == UINT32_C(65536) ? 0 : 1;
}
