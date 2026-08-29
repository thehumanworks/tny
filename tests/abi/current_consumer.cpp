#include "tny/tny.h"

#include <cstdint>

int main() {
    tny_runtime_options_v1 options{};
    tny_host_services_v1 services{};
    tny_tool_spec_v1 spec{};
    tny_tool_result_v1 result{};
    return tny_abi_version() == UINT32_C(65537) &&
                   tny_runtime_options_v1_init(&options, sizeof options) == TNY_STATUS_OK &&
                   tny_host_services_v1_init(&services, sizeof services) == TNY_STATUS_OK &&
                   tny_tool_spec_v1_init(&spec, sizeof spec) == TNY_STATUS_OK &&
                   tny_tool_result_v1_init(&result, sizeof result) == TNY_STATUS_OK
               ? 0
               : 1;
}
