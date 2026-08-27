#include "tny/tny.h"

#include <cstdint>

int main() {
    tny_runtime_options_v0 options{};
    tny_runtime_options_init(&options);
    return tny_abi_version() == UINT32_C(8) && options.struct_size == 200
        ? 0 : 1;
}
