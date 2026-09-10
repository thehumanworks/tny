/* Each call owns a native thread and a single-use libtny job. No JavaScript
 * callbacks or borrowed JS buffers cross into native provider code. Node's
 * async-work pool drains before environment cleanup, so it cannot host these
 * cancellable operations (ADR 0085). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif
#include "addon_internal.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#if TNY_ABI_MINOR < 2
typedef struct tny_toolkit_job tny_toolkit_job;
#endif

typedef struct {
    void *library;
    int32_t (*create)(tny_bytes, tny_toolkit_job **, tny_error **);
    int32_t (*run)(tny_toolkit_job *, tny_error **);
    int32_t (*cancel)(tny_toolkit_job *);
    tny_bytes (*result)(const tny_toolkit_job *);
    int32_t (*destroy)(tny_toolkit_job **);
} toolkit_api;

typedef struct {
    toolkit_api api;
    tny_toolkit_job *job;
    napi_threadsafe_function tsfn;
    pthread_t thread;
    napi_deferred deferred;
    int refs, closing, cleanup_registered, started, joined;
    int32_t status;
    char *error;
} toolkit_work;

static int load_api(toolkit_api *api) {
    if ((tny_abi_version() >> 16u) != 1u || (tny_abi_version() & 0xffffu) < 2u) return 0;
    Dl_info info;
    void *address = NULL;
    uint32_t (*version)(void) = tny_abi_version;
    if (sizeof address != sizeof version) return 0;
    memcpy(&address, &version, sizeof address);
    if (!address || !dladdr(address, &info) || !info.dli_fname) return 0;
    api->library = dlopen(info.dli_fname, RTLD_NOW | RTLD_LOCAL);
    if (!api->library) return 0;
#define LOAD(name)                                                     \
    do {                                                               \
        void *symbol = dlsym(api->library, "tny_toolkit_job_" #name);  \
        if (!symbol || sizeof symbol != sizeof api->name) goto failed; \
        memcpy(&api->name, &symbol, sizeof symbol);                    \
    } while (0)
    LOAD(create);
    LOAD(run);
    LOAD(cancel);
    LOAD(result);
    LOAD(destroy);
#undef LOAD
    return 1;
failed:
    dlclose(api->library);
    memset(api, 0, sizeof *api);
    return 0;
}

static napi_value make_error(napi_env env, int32_t status, const char *message) {
    napi_value text, error, name, code;
    if (napi_create_string_utf8(env, message ? message : "toolkit operation failed",
                                NAPI_AUTO_LENGTH, &text) != napi_ok ||
        napi_create_error(env, NULL, text, &error) != napi_ok ||
        napi_create_string_utf8(env, "TnyError", NAPI_AUTO_LENGTH, &name) != napi_ok ||
        napi_create_int32(env, status, &code) != napi_ok)
        return NULL;
    napi_property_descriptor props[] = {
        {"name", NULL, NULL, NULL, NULL, name, napi_default, NULL},
        {"status", NULL, NULL, NULL, NULL, code, napi_default, NULL},
    };
    if (napi_define_properties(env, error, 2, props) != napi_ok) return NULL;
    return error;
}

static void release(toolkit_work *w) {
    if (--w->refs) return;
    if (w->job) (void)w->api.destroy(&w->job);
    if (w->api.library) dlclose(w->api.library);
    free(w->error);
    free(w);
}

static void finalize(napi_env env, void *data, void *hint) {
    (void)env;
    (void)hint;
    release(data);
}

static void join_worker(toolkit_work *w) {
    if (w->started && !w->joined) {
        (void)pthread_join(w->thread, NULL);
        w->joined = 1;
    }
}

static void finalize_work(napi_env env, void *data, void *hint) {
    (void)env;
    (void)hint;
    toolkit_work *w = data;
    join_worker(w);
    release(w);
}

static napi_value cancel(napi_env env, napi_callback_info info) {
    toolkit_work *w = NULL;
    napi_value result;
    if (napi_get_cb_info(env, info, NULL, NULL, NULL, (void **)&w) != napi_ok || !w) return NULL;
    if (w->job) (void)w->api.cancel(w->job);
    if (napi_get_undefined(env, &result) != napi_ok) return NULL;
    return result;
}

static void cleanup(void *data) {
    toolkit_work *w = data;
    w->cleanup_registered = 0;
    w->closing = 1;
    if (w->job) (void)w->api.cancel(w->job);
    join_worker(w);
    (void)napi_release_threadsafe_function(w->tsfn, napi_tsfn_abort);
}

static void *execute(void *data) {
    toolkit_work *w = data;
    tny_error *error = NULL;
    w->status = w->api.run(w->job, &error);
    if (w->status) w->error = sdk_take_error(w->status, error);
    (void)napi_call_threadsafe_function(w->tsfn, w, napi_tsfn_nonblocking);
    (void)napi_release_threadsafe_function(w->tsfn, napi_tsfn_release);
    return NULL;
}

static void complete(napi_env env, napi_value callback, void *context, void *data) {
    (void)callback;
    (void)context;
    toolkit_work *w = data;
    join_worker(w);
    if (env && !w->closing) {
        napi_value value = NULL;
        if (!w->status) {
            tny_bytes result = w->api.result(w->job);
            if (napi_create_string_utf8(env, result.ptr ? result.ptr : "", (size_t)result.len,
                                        &value) != napi_ok)
                w->status = TNY_STATUS_OOM;
        }
        if (w->status) {
            value = make_error(env, w->status, w->error);
            if (value) (void)napi_reject_deferred(env, w->deferred, value);
        } else (void)napi_resolve_deferred(env, w->deferred, value);
    }
    (void)w->api.destroy(&w->job);
    if (w->cleanup_registered) {
        (void)napi_remove_env_cleanup_hook(env, cleanup, w);
        w->cleanup_registered = 0;
        (void)napi_release_threadsafe_function(w->tsfn, napi_tsfn_release);
    }
}

static napi_value start(napi_env env, napi_callback_info info) {
    size_t argc = 1, size = 0;
    napi_value argv[1], promise, fn, resource, object;
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc != 1 ||
        napi_get_value_string_utf8(env, argv[0], NULL, 0, &size) != napi_ok || !size ||
        size > 256u * 1024u) {
        napi_throw_type_error(env, NULL, "toolkit needs a JSON string of at most 256 KiB");
        return NULL;
    }
    toolkit_work *w = calloc(1, sizeof *w);
    if (!w) {
        napi_throw_error(env, NULL, "out of memory");
        return NULL;
    }
    w->refs = 1; /* TSFN, plus a separate cancel-function reference below */
    int32_t failure = TNY_STATUS_OOM;
    const char *message = "cannot schedule toolkit operation";
    if (!load_api(&w->api)) {
        failure = TNY_STATUS_UNSUPPORTED;
        message = "Toolkit requires libtny ABI 1.2 or newer";
        goto failed;
    }
    sdk_owned_bytes json = {malloc(size + 1), size};
    if (!json.ptr) goto failed;
    if (napi_get_value_string_utf8(env, argv[0], json.ptr, size + 1, &size) != napi_ok) {
        sdk_wipe_owned_bytes(&json);
        goto failed;
    }
    tny_error *error = NULL;
    failure = w->api.create(sdk_view_of(json), &w->job, &error);
    sdk_wipe_owned_bytes(&json);
    if (failure) {
        w->error = sdk_take_error(failure, error);
        message = w->error;
        goto failed;
    }
    failure = TNY_STATUS_OOM;
    if (napi_create_promise(env, &w->deferred, &promise) != napi_ok ||
        napi_create_function(env, "cancelToolkit", NAPI_AUTO_LENGTH, cancel, w, &fn) != napi_ok ||
        napi_create_string_utf8(env, "tny:toolkit", NAPI_AUTO_LENGTH, &resource) != napi_ok ||
        napi_create_object(env, &object) != napi_ok)
        goto failed;
    if (napi_add_finalizer(env, fn, w, finalize, NULL, NULL) != napi_ok) goto failed;
    w->refs++;
    napi_property_descriptor props[] = {
        {"promise", NULL, NULL, NULL, NULL, promise, napi_default, NULL},
        {"cancel", NULL, NULL, NULL, NULL, fn, napi_default, NULL},
    };
    if (napi_define_properties(env, object, 2, props) != napi_ok ||
        napi_create_threadsafe_function(env, NULL, NULL, resource, 1, 2, w, finalize_work, w,
                                        complete, &w->tsfn) != napi_ok)
        goto failed;
    if (napi_add_env_cleanup_hook(env, cleanup, w) != napi_ok) goto failed;
    w->cleanup_registered = 1;
    if (pthread_create(&w->thread, NULL, execute, w)) goto failed;
    w->started = 1;
    return object;
failed:
    {
        napi_value thrown = make_error(env, failure, message);
        if (w->cleanup_registered) {
            (void)napi_remove_env_cleanup_hook(env, cleanup, w);
            w->cleanup_registered = 0;
        }
        if (w->job) (void)w->api.destroy(&w->job);
        if (w->tsfn) {
            (void)napi_release_threadsafe_function(w->tsfn, napi_tsfn_abort);
            (void)napi_release_threadsafe_function(w->tsfn, napi_tsfn_release);
        } else release(w);
        if (thrown) (void)napi_throw(env, thrown);
        return NULL;
    }
}

void sdk_define_toolkit(napi_env env, napi_value exports) {
    napi_property_descriptor property = {"startToolkit", NULL, start,        NULL,
                                         NULL,           NULL, napi_default, NULL};
    (void)napi_define_properties(env, exports, 1, &property);
}
