#define _POSIX_C_SOURCE 200809L
#include "addon_internal.h"

#include <stdlib.h>

typedef struct {
    napi_env env;
    napi_async_work work;
    napi_deferred deferred;
    uint32_t abi_version;
    uint32_t capability_size;
    char *library_version;
} probe_work;

static void probe_execute(napi_env env, void *opaque) {
    probe_work *work = (probe_work *)opaque;
    tny_capabilities_v0 capabilities;
    tny_bytes version;
    (void)env;
    work->abi_version = tny_abi_version();
    if (tny_capabilities_init(&capabilities, sizeof capabilities) ==
        TNY_STATUS_OK)
        work->capability_size = capabilities.struct_size;
    version = tny_library_version();
    work->library_version = sdk_copy_n(version.ptr, (size_t)version.len);
}

static void set_named(napi_env env, napi_value object, const char *name, napi_value value) {
    (void)napi_set_named_property(env, object, name, value);
}

static void probe_complete(napi_env env, napi_status status, void *opaque) {
    probe_work *work = (probe_work *)opaque;
    napi_value result, value;
    if (status != napi_ok || !work->library_version) {
        (void)napi_create_string_utf8(env, "native ABI probe failed", NAPI_AUTO_LENGTH, &value);
        (void)napi_create_error(env, NULL, value, &result);
        (void)napi_reject_deferred(env, work->deferred, result);
    } else {
        (void)napi_create_object(env, &result);
        (void)napi_create_uint32(env, work->abi_version, &value);
        set_named(env, result, "abiVersion", value);
        (void)napi_create_uint32(env, work->capability_size, &value);
        set_named(env, result, "capabilitySize", value);
        (void)napi_create_string_utf8(env, work->library_version, NAPI_AUTO_LENGTH, &value);
        set_named(env, result, "libraryVersion", value);
        (void)napi_resolve_deferred(env, work->deferred, result);
    }
    free(work->library_version);
    (void)napi_delete_async_work(env, work->work);
    free(work);
}

static napi_value probe_abi(napi_env env, napi_callback_info info) {
    probe_work *work = (probe_work *)calloc(1u, sizeof(*work));
    napi_value promise, name;
    (void)info;
    if (!work) {
        (void)napi_throw_error(env, NULL, "out of memory");
        return NULL;
    }
    work->env = env;
    (void)napi_create_promise(env, &work->deferred, &promise);
    (void)napi_create_string_utf8(env, "tny ABI probe", NAPI_AUTO_LENGTH, &name);
    if (napi_create_async_work(env, NULL, name, probe_execute, probe_complete,
                               work, &work->work) != napi_ok ||
        napi_queue_async_work(env, work->work) != napi_ok) {
        free(work);
        (void)napi_throw_error(env, NULL, "failed to queue ABI probe");
        return NULL;
    }
    return promise;
}

void sdk_define_probe(napi_env env, napi_value exports) {
    napi_property_descriptor property = {
        "__probeAbi", NULL, probe_abi, NULL, NULL, NULL, napi_default, NULL
    };
    (void)napi_define_properties(env, exports, 1u, &property);
}
