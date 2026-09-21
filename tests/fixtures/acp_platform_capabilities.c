/* Host-linked ABI probe; the Makefile selects the native or wasm process seam. */
#include "tny/tny.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3) return 1;
    tny_runtime_options_v0 options;
    if (tny_runtime_options_init(&options, sizeof options) != TNY_STATUS_OK) return 2;
    options.workspace = (tny_bytes){argv[1], strlen(argv[1])};
    options.provider = (tny_bytes){argv[2], strlen(argv[2])};
    options.persistence = 0;
    tny_runtime *runtime = NULL;
    if (tny_runtime_create(&options, sizeof options, &runtime, NULL) != TNY_STATUS_OK) return 3;
    tny_capabilities_v0 capabilities;
    int status = tny_capabilities_init(&capabilities, sizeof capabilities);
    if (status == TNY_STATUS_OK)
        status = tny_runtime_get_capabilities(runtime, &capabilities, sizeof capabilities);
    if (status == TNY_STATUS_OK)
        printf("{\"available\":%llu,\"selected\":%u,\"initialized\":%u}\n",
               (unsigned long long)capabilities.provider_available_mask,
               capabilities.provider_selected, capabilities.provider_initialized);
    if (tny_runtime_destroy(&runtime) != TNY_STATUS_OK) return 4;
    return status == TNY_STATUS_OK ? 0 : 5;
}
