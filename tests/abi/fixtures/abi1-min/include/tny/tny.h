/* Immutable minimum ABI1.0 consumer header. Do not extend in place. */
#ifndef LIBTNY_ABI1_MIN_TNY_H
#define LIBTNY_ABI1_MIN_TNY_H

#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define TNY_API __attribute__((visibility("default")))
#else
#define TNY_API
#endif
#define TNY_CALL

#ifdef __cplusplus
extern "C" {
#endif

#define TNY_ABI_MAJOR 1u
#define TNY_ABI_MINOR 0u
#define TNY_ABI_VERSION UINT32_C(65536)
#define TNY_STATUS_OK 0
#define TNY_STATUS_INVALID_ARGUMENT (-1)
#define TNY_CAP_FEATURE_SHARED_LIBRARY (UINT64_C(1) << 2)
#define TNY_CAP_FEATURE_STATIC_LIBRARY (UINT64_C(1) << 3)

typedef struct tny_runtime tny_runtime;
typedef struct tny_error tny_error;

typedef struct {
    const char *ptr;
    uint64_t len;
} tny_bytes;

typedef struct {
    uint32_t struct_size;
    uint32_t permission_mode;
    uint32_t persistence;
    uint32_t max_steps;
    uint64_t max_tool_result_bytes;
    tny_bytes workspace;
    tny_bytes state_dir;
    tny_bytes provider;
    tny_bytes model;
    tny_bytes base_url;
    tny_bytes api_key;
    tny_bytes wire_api;
    uint64_t reserved[8];
} tny_runtime_options_v0;

typedef struct {
    uint32_t struct_size;
    uint32_t schema_version;
    uint32_t abi_version;
    uint32_t provider_selected;
    uint32_t provider_initialized;
    uint32_t endpoint_reachability;
    uint32_t threading_model;
    uint32_t cancel_model;
    uint64_t provider_available_mask;
    uint64_t feature_available_mask;
    uint64_t feature_enabled_mask;
    uint32_t event_queue_max;
    uint32_t event_reserved;
    uint64_t event_payload_bytes_max;
    uint64_t event_reserved_bytes;
    tny_bytes library_version;
    tny_bytes platform_family;
    tny_bytes architecture;
    tny_bytes transport;
    tny_bytes tls_implementation;
    tny_bytes linkage;
    uint64_t reserved[8];
} tny_capabilities_v0;

TNY_API uint32_t TNY_CALL tny_abi_version(void);
TNY_API int32_t TNY_CALL tny_runtime_options_init(
    tny_runtime_options_v0 *options, uint64_t capacity);
TNY_API int32_t TNY_CALL tny_capabilities_init(
    tny_capabilities_v0 *capabilities, uint64_t capacity);
TNY_API int32_t TNY_CALL tny_runtime_create(
    const tny_runtime_options_v0 *options, uint64_t capacity,
    tny_runtime **out_runtime, tny_error **out_error);
TNY_API int32_t TNY_CALL tny_runtime_get_capabilities(
    const tny_runtime *runtime, tny_capabilities_v0 *capabilities,
    uint64_t capacity);
TNY_API int32_t TNY_CALL tny_runtime_destroy(tny_runtime **runtime);

#ifdef __cplusplus
}
#endif

#endif
