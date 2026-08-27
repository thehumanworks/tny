#define TNY_BUILDING_LIBRARY 1
#include "tny/tny.h"

#include "core/backend.h"
#include "core/config.h"
#include "core/perm.h"
#include "core/runtime.h"
#include "core/session.h"
#include "util/alloc.h"
#include "util/util.h"

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct tny_error {
    int32_t code;
    char *message;
};

struct tny_runtime {
    tny_ctx *ctx;
    struct tny_session *session;
    pthread_t owner;
    pid_t owner_pid;
    uint32_t endpoint_reachability;
};

struct tny_session {
    tny_runtime *runtime;
    tny_session_state *state;
    perm_engine *perm;
    tny_engine *engine;
    char *permission_id;
    uint32_t active;
    uint32_t terminal;
    uint32_t drained;
};

static tny_bytes bytes_of(const char *s, uint64_t len) {
    tny_bytes b = {s, s ? len : 0};
    return b;
}

static tny_bytes cstr_bytes(const char *s) {
    return bytes_of(s, s ? (uint64_t)strlen(s) : 0);
}

static int32_t failf(tny_error **out, int32_t code, const char *fmt, ...) {
    if (out) {
        *out = NULL;
        tny_error *e = calloc(1, sizeof *e);
        if (e) {
            char buf[512];
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(buf, sizeof buf, fmt, ap);
            va_end(ap);
            e->message = strdup(buf);
            if (e->message) { e->code = code; *out = e; }
            else free(e);
        }
    }
    return code;
}

static int32_t scoped_status(int32_t status, tny_error **error) {
    if (!tny_alloc_scope_failed()) return status;
    tny_alloc_scope_clear();
    if (error && *error) {
        (*error)->code = TNY_STATUS_OOM;
        free((*error)->message);
        (*error)->message = strdup("out of memory");
    } else {
        (void)failf(error, TNY_STATUS_OOM, "out of memory");
    }
    return TNY_STATUS_OOM;
}

static bool utf8_valid(const unsigned char *s, uint64_t n) {
    for (uint64_t i = 0; i < n;) {
        unsigned c = s[i++];
        if (c == 0) return false;
        if (c < 0x80) continue;
        unsigned need, min;
        if ((c & 0xe0) == 0xc0) { need = 1; min = 0x80; c &= 0x1f; }
        else if ((c & 0xf0) == 0xe0) { need = 2; min = 0x800; c &= 0x0f; }
        else if ((c & 0xf8) == 0xf0) { need = 3; min = 0x10000; c &= 0x07; }
        else return false;
        if (n - i < need) return false;
        for (unsigned j = 0; j < need; j++) {
            unsigned d = s[i++];
            if ((d & 0xc0) != 0x80) return false;
            c = (c << 6) | (d & 0x3f);
        }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
            return false;
    }
    return true;
}

static int32_t copy_bytes(tny_bytes value, bool required, const char *field,
                          char **out, tny_error **error) {
    *out = NULL;
    if (!value.ptr || value.len == 0) {
        if (required) return failf(error, TNY_STATUS_INVALID_ARGUMENT,
                                   "%s is required", field);
        return TNY_STATUS_OK;
    }
    if (value.len > SIZE_MAX - 1 ||
        !utf8_valid((const unsigned char *)value.ptr, value.len))
        return failf(error, TNY_STATUS_INVALID_ARGUMENT,
                     "%s must be valid UTF-8 without NUL bytes", field);
    char *copy = malloc((size_t)value.len + 1);
    if (!copy) return failf(error, TNY_STATUS_OOM, "out of memory");
    memcpy(copy, value.ptr, (size_t)value.len);
    copy[value.len] = 0;
    *out = copy;
    return TNY_STATUS_OK;
}

static bool on_owner(const tny_runtime *runtime) {
    return runtime && runtime->owner_pid == getpid() &&
           pthread_equal(runtime->owner, pthread_self());
}

static int32_t require_owner(const tny_runtime *runtime, tny_error **error) {
    if (!runtime) return failf(error, TNY_STATUS_INVALID_ARGUMENT,
                               "runtime is required");
    if (runtime->owner_pid != getpid())
        return TNY_STATUS_BAD_STATE;
    if (!on_owner(runtime)) return failf(error, TNY_STATUS_BAD_STATE,
                                         "libtny handle used from a non-owner thread");
    return TNY_STATUS_OK;
}

static int32_t status_from_message(const char *message, int32_t fallback) {
    if (!message) return fallback;
    if (strstr(message, "API key") || strstr(message, "credential") ||
        strstr(message, "authentication"))
        return TNY_STATUS_AUTH;
    if (strstr(message, "connect") || strstr(message, "TLS") ||
        strstr(message, "request to") || strstr(message, "transport"))
        return TNY_STATUS_IO;
    return fallback;
}

uint32_t tny_abi_version(void) { return TNY_ABI_VERSION; }
tny_bytes tny_library_version(void) { return cstr_bytes(TNY_VERSION); }

static const char *cap_platform(void) {
#if defined(__APPLE__)
    return "macos";
#elif defined(__linux__) && defined(__GLIBC__)
    return "linux-glibc";
#else
    return "unsupported";
#endif
}

static const char *cap_architecture(void) {
#if defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

static const char *cap_tls(void) {
#if defined(__APPLE__)
    return "securetransport";
#elif defined(__linux__) && defined(__GLIBC__)
    return "openssl-dynamic";
#else
    return "none";
#endif
}

static bool cap_tls_available(void) {
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
    return true;
#else
    return false;
#endif
}

void tny_runtime_options_init(tny_runtime_options_v0 *o) {
    if (!o) return;
    memset(o, 0, sizeof *o);
    o->struct_size = sizeof *o;
    o->permission_mode = TNY_PERMISSION_ASK;
    o->max_steps = 0; /* unlimited; embedders opt into a cap */
    o->max_tool_result_bytes = 32768;
}

void tny_capabilities_init(tny_capabilities_v0 *capabilities) {
    if (!capabilities) return;
    memset(capabilities, 0, sizeof *capabilities);
    capabilities->struct_size = sizeof *capabilities;
}

static int32_t validate_options(const tny_runtime_options_v0 *o,
                                tny_error **error) {
    size_t required = offsetof(tny_runtime_options_v0, reserved);
    if (!o || o->struct_size < required)
        return failf(error, TNY_STATUS_INVALID_ARGUMENT,
                     "runtime options are missing or too small");
    if (o->permission_mode > TNY_PERMISSION_YOLO || o->persistence > 1)
        return failf(error, TNY_STATUS_INVALID_ARGUMENT,
                     "invalid permission or persistence option");
    if (o->max_steps > INT32_MAX)
        return failf(error, TNY_STATUS_INVALID_ARGUMENT,
                     "max_steps exceeds the supported signed runtime range");
    /* max_steps: 0 = unlimited, any positive value caps the loop */
    if (o->max_tool_result_bytes == 0 || o->max_tool_result_bytes > (16u << 20))
        return failf(error, TNY_STATUS_INVALID_ARGUMENT,
                     "runtime limits are outside the supported range");
    if (o->struct_size >= sizeof *o) {
        for (size_t i = 0; i < sizeof o->reserved / sizeof o->reserved[0]; i++)
            if (o->reserved[i] != 0)
                return failf(error, TNY_STATUS_INVALID_ARGUMENT,
                             "reserved runtime options must be zero");
    }
    return TNY_STATUS_OK;
}

int32_t tny_runtime_create(const tny_runtime_options_v0 *o,
                           tny_runtime **out, tny_error **error) {
    tny_alloc_scope_begin("runtime_create");
    if (out) *out = NULL;
    if (error) *error = NULL;
    if (!out) return scoped_status(failf(error, TNY_STATUS_INVALID_ARGUMENT,
                                         "out_runtime is required"), error);
    int32_t rc = validate_options(o, error);
    if (rc != TNY_STATUS_OK) return scoped_status(rc, error);
    char *workspace = NULL, *state_dir = NULL, *provider = NULL;
    char *model = NULL, *base_url = NULL, *api_key = NULL, *wire = NULL;
#define COPY(field, required_, slot) do { \
    rc = copy_bytes(o->field, required_, #field, &slot, error); \
    if (rc != TNY_STATUS_OK) goto fail; \
} while (0)
    COPY(workspace, true, workspace);
    COPY(state_dir, o->persistence != 0, state_dir);
    COPY(provider, false, provider);
    COPY(model, false, model);
    COPY(base_url, false, base_url);
    COPY(api_key, false, api_key);
    COPY(wire_api, false, wire);
#undef COPY

    const char *selected = provider ? provider : "openai";
    int backend = tny_backend_from_name(selected);
    if (backend != TNY_BK_OPENAI) {
        rc = failf(error, backend < 0 ? TNY_STATUS_INVALID_ARGUMENT
                                     : TNY_STATUS_UNSUPPORTED,
                   "provider '%s' is not supported by ABI 0", selected);
        goto fail;
    }
    /* An ephemeral context still needs an internal absolute path base for its
     * in-memory session representation. Reusing the workspace is safe because
     * no-save is set below before a session can be created, and no state path
     * is ever materialized. */
    tny_ctx *ctx = tny_ctx_new_explicit(workspace,
                                        state_dir ? state_dir : workspace);
    if (!ctx) { rc = failf(error, TNY_STATUS_CONFIG,
                           "workspace or state directory is invalid"); goto fail; }
    ctx->backend = backend;
    free(ctx->provider_name);
    ctx->provider_name = provider ? provider : strdup("openai");
    provider = NULL;
    if (!ctx->provider_name) {
        tny_ctx_free(ctx);
        rc = failf(error, TNY_STATUS_OOM, "out of memory");
        goto fail;
    }
    ctx->model = model; model = NULL;
    ctx->api_key = api_key; api_key = NULL;
    if (base_url) { free(ctx->base_url); ctx->base_url = base_url; base_url = NULL; }
    if (wire) { ctx->wire_api = wire; wire = NULL; }
    ctx->perm_mode = (tny_perm_mode)o->permission_mode;
    ctx->no_save = o->persistence == 0;
    ctx->max_steps = (int)o->max_steps;
    ctx->max_tool_result_bytes = (size_t)o->max_tool_result_bytes;
    ctx->mcp_disabled = true;

    tny_runtime *runtime = calloc(1, sizeof *runtime);
    if (!runtime) { tny_ctx_free(ctx); rc = failf(error, TNY_STATUS_OOM,
                                                  "out of memory"); goto fail; }
    runtime->ctx = ctx;
    runtime->owner = pthread_self();
    runtime->owner_pid = getpid();
    runtime->endpoint_reachability = TNY_ENDPOINT_REACHABILITY_UNKNOWN;
    if (tny_alloc_scope_failed()) {
        tny_ctx_free(ctx);
        free(runtime);
        rc = TNY_STATUS_OOM;
        goto fail;
    }
    *out = runtime;
    free(workspace); free(state_dir);
    return TNY_STATUS_OK;

fail:
    free(workspace); free(state_dir); free(provider); free(model);
    free(base_url); secure_free(api_key); free(wire);
    return scoped_status(rc, error);
}

static void session_release(tny_session *session);

void tny_runtime_free(tny_runtime *runtime) {
    if (!runtime) return;
    if (!on_owner(runtime)) return;
    if (runtime->session) session_release(runtime->session);
    tny_ctx_free(runtime->ctx);
    free(runtime);
}

int32_t tny_runtime_get_capabilities(const tny_runtime *runtime,
                                     tny_capabilities_v0 *capabilities) {
    if (!runtime || !capabilities) return TNY_STATUS_INVALID_ARGUMENT;
    if (!on_owner(runtime)) return TNY_STATUS_BAD_STATE;
    size_t required = offsetof(tny_capabilities_v0, library_version);
    if (capabilities->struct_size < required)
        return TNY_STATUS_INVALID_ARGUMENT;

    uint32_t caller_size = capabilities->struct_size;
    tny_capabilities_v0 full;
    tny_capabilities_init(&full);
    full.schema_version = TNY_CAPABILITY_SCHEMA_VERSION;
    full.abi_version = TNY_ABI_VERSION;
    full.provider_selected = TNY_PROVIDER_OPENAI;
    full.provider_initialized = runtime->session &&
        tny_engine_ready(runtime->session->engine) ? 1u : 0u;
    full.endpoint_reachability = runtime->endpoint_reachability;
    full.threading_model = TNY_THREADING_OWNER_THREAD;
    full.cancel_model = TNY_CANCEL_CROSS_THREAD_ASYNC_WAKE;
    full.provider_available_mask = TNY_PROVIDER_MASK_OPENAI;
    full.feature_available_mask = TNY_CAP_FEATURE_PERSISTENCE |
                                  TNY_CAP_FEATURE_SHARED_LIBRARY |
                                  TNY_CAP_FEATURE_CROSS_THREAD_CANCEL;
    full.feature_enabled_mask = TNY_CAP_FEATURE_SHARED_LIBRARY |
                                TNY_CAP_FEATURE_CROSS_THREAD_CANCEL;
    if (cap_tls_available()) {
        full.feature_available_mask |= TNY_CAP_FEATURE_TLS;
        if (str_starts(runtime->ctx->base_url, "https://"))
            full.feature_enabled_mask |= TNY_CAP_FEATURE_TLS;
    }
    if (!runtime->ctx->no_save)
        full.feature_enabled_mask |= TNY_CAP_FEATURE_PERSISTENCE;
    /* Keep synchronized with the private queue budgets in core/runtime.c.
     * They are reported values, not ABI layout or caller-configurable knobs. */
    full.event_queue_max = 256u;
    full.event_reserved = 2u;
    full.event_payload_bytes_max = UINT64_C(1024) * UINT64_C(1024);
    full.event_reserved_bytes = 1024u;
    full.library_version = tny_library_version();
    full.platform_family = cstr_bytes(cap_platform());
    full.architecture = cstr_bytes(cap_architecture());
    full.transport = cstr_bytes("native-http1");
    full.tls_implementation = cstr_bytes(cap_tls());
    full.linkage = cstr_bytes("shared");

    size_t n = caller_size < sizeof full ? caller_size : sizeof full;
    memcpy(capabilities, &full, n);
    capabilities->struct_size = caller_size;
    return TNY_STATUS_OK;
}

static int32_t make_session(tny_runtime *runtime, tny_session_state *state,
                            tny_session **out, tny_error **error) {
    if (!state) return failf(error, TNY_STATUS_CONFIG,
                             "could not create or open the session");
    tny_session *session = calloc(1, sizeof *session);
    if (!session) { session_close(state); return failf(error, TNY_STATUS_OOM,
                                                       "out of memory"); }
    session->runtime = runtime;
    session->state = state;
    session->perm = perm_new(runtime->ctx);
    session->engine = session->perm
        ? tny_engine_new(runtime->ctx, state, session->perm, NULL, NULL) : NULL;
    if (!session->perm || !session->engine) {
        session_release(session);
        return failf(error, TNY_STATUS_OOM, "out of memory");
    }
    if (tny_engine_enable_threadsafe_cancel(session->engine) != 0) {
        session_release(session);
        return failf(error, TNY_STATUS_IO,
                     "could not initialize the cancellation wake source");
    }
    runtime->session = session;
    *out = session;
    return TNY_STATUS_OK;
}

int32_t tny_session_create(tny_runtime *runtime, tny_session **out,
                           tny_error **error) {
    tny_alloc_scope_begin("session_create");
    if (out) *out = NULL;
    if (error) *error = NULL;
    int32_t rc = require_owner(runtime, error);
    if (rc != TNY_STATUS_OK) return scoped_status(rc, error);
    if (!out) return scoped_status(failf(error, TNY_STATUS_INVALID_ARGUMENT,
                                         "out_session is required"), error);
    if (runtime->session)
        return scoped_status(failf(error, TNY_STATUS_BAD_STATE,
                                   "runtime already has an open session"), error);
    rc = make_session(runtime, session_new(runtime->ctx), out, error);
    if (tny_alloc_scope_failed() && *out) {
        session_release(*out);
        *out = NULL;
    }
    return scoped_status(rc, error);
}

int32_t tny_session_open(tny_runtime *runtime, tny_bytes id,
                         tny_session **out, tny_error **error) {
    tny_alloc_scope_begin("session_open");
    if (out) *out = NULL;
    if (error) *error = NULL;
    int32_t rc = require_owner(runtime, error);
    if (rc != TNY_STATUS_OK) return scoped_status(rc, error);
    if (!out) return scoped_status(failf(error, TNY_STATUS_INVALID_ARGUMENT,
                                         "out_session is required"), error);
    if (runtime->session)
        return scoped_status(failf(error, TNY_STATUS_BAD_STATE,
                                   "runtime already has an open session"), error);
    char *session_id = NULL;
    rc = copy_bytes(id, true, "session id", &session_id, error);
    if (rc != TNY_STATUS_OK) return scoped_status(rc, error);
    tny_session_state *state = session_open(runtime->ctx, session_id);
    free(session_id);
    rc = make_session(runtime, state, out, error);
    if (tny_alloc_scope_failed() && *out) {
        session_release(*out);
        *out = NULL;
    }
    return scoped_status(rc, error);
}

tny_bytes tny_session_id(const tny_session *session) {
    return session && on_owner(session->runtime) && session->state
                                     ? cstr_bytes(session->state->id)
                                     : bytes_of(NULL, 0);
}

static void session_release(tny_session *session) {
    if (!session) return;
    tny_engine_free(session->engine);
    perm_free(session->perm);
    session_close(session->state);
    if (session->runtime && session->runtime->session == session)
        session->runtime->session = NULL;
    free(session->permission_id);
    free(session);
}

void tny_session_free(tny_session *session) {
    if (!session || !on_owner(session->runtime)) return;
    session_release(session);
}

int32_t tny_session_send(tny_session *session, tny_bytes prompt,
                         tny_error **error) {
    tny_alloc_scope_begin("session_send");
    if (error) *error = NULL;
    if (!session) return scoped_status(failf(error, TNY_STATUS_INVALID_ARGUMENT,
                                             "session is required"), error);
    int32_t rc = require_owner(session->runtime, error);
    if (rc != TNY_STATUS_OK) return scoped_status(rc, error);
    if (session->active)
        return scoped_status(failf(error, TNY_STATUS_BAD_STATE,
                                   "session already has an active turn"), error);
    char *text = NULL;
    rc = copy_bytes(prompt, true, "prompt", &text, error);
    if (rc != TNY_STATUS_OK) return scoped_status(rc, error);
    if (!tny_engine_ready(session->engine)) {
        char errbuf[512] = {0};
        tny_backend *backend = tny_backend_openai_new(session->runtime->ctx);
        if (!backend) {
            free(text);
            return scoped_status(
                failf(error, TNY_STATUS_OOM, "out of memory"), error);
        }
        if (tny_engine_prepare(session->engine, backend,
                               TNY_ENGINE_PREPARE_FRESH,
                               errbuf, sizeof errbuf) != 0) {
            free(text);
            int32_t status = status_from_message(errbuf, TNY_STATUS_CONFIG);
            if (status == TNY_STATUS_IO || status == TNY_STATUS_TIMEOUT_ERROR)
                session->runtime->endpoint_reachability =
                    TNY_ENDPOINT_REACHABILITY_UNREACHABLE;
            return scoped_status(failf(error, status, "%s", errbuf), error);
        }
    }
    char errbuf[512];
    if (tny_engine_start(session->engine, text, NULL,
                         errbuf, sizeof errbuf) != 0) {
        free(text);
        int32_t status = status_from_message(errbuf, TNY_STATUS_IO);
        if (status == TNY_STATUS_IO || status == TNY_STATUS_TIMEOUT_ERROR)
            session->runtime->endpoint_reachability =
                TNY_ENDPOINT_REACHABILITY_UNREACHABLE;
        return scoped_status(failf(error, status, "%s", errbuf), error);
    }
    free(text);
    free(session->permission_id);
    session->permission_id = NULL;
    session->active = 1;
    session->terminal = 0;
    session->drained = 0;
    if (tny_alloc_scope_failed()) tny_engine_fail_oom(session->engine);
    return TNY_STATUS_OK;
}

static uint32_t public_kind(tny_event_kind kind) { return (uint32_t)kind; }
static int32_t public_error_code(const tny_owned_event *owned);

int32_t tny_session_next_event(tny_session *session, uint32_t timeout_ms,
                               tny_event **out, tny_error **error) {
    tny_alloc_scope_begin("next_event");
    if (out) *out = NULL;
    if (error) *error = NULL;
    if (!session || !out)
        return scoped_status(failf(error, TNY_STATUS_INVALID_ARGUMENT,
                                   "session and out_event are required"), error);
    if (timeout_ms > 600000u)
        return scoped_status(failf(error, TNY_STATUS_INVALID_ARGUMENT,
                                   "next_event timeout exceeds 600000 ms"), error);
    int32_t rc = require_owner(session->runtime, error);
    if (rc != TNY_STATUS_OK) return scoped_status(rc, error);
    if (!session->active && !session->terminal)
        return scoped_status(failf(error, TNY_STATUS_BAD_STATE,
                                   "no turn has started"), error);
    if (session->drained) return TNY_STATUS_DRAINED;
    tny_owned_event *owned = NULL;
    char errbuf[512];
    tny_engine_next next = tny_engine_next_event(session->engine,
                                                  (int)timeout_ms, &owned,
                                                  errbuf, sizeof errbuf);
    if (next == TNY_ENGINE_NEXT_TIMEOUT) return TNY_STATUS_TIMEOUT;
    if (next == TNY_ENGINE_NEXT_DRAINED) {
        session->drained = 1;
        session->active = 0;
        return TNY_STATUS_DRAINED;
    }
    if (next == TNY_ENGINE_NEXT_ERROR)
        return scoped_status(failf(error, TNY_STATUS_IO, "%s", errbuf), error);
    if (owned->ev.kind == TNY_EV_PERMISSION) {
        free(session->permission_id);
        session->permission_id = strdup(owned->ev.perm_id ? owned->ev.perm_id : "");
        if (!session->permission_id) {
            tny_owned_event_free(owned);
            tny_engine_fail_oom(session->engine);
            owned = tny_engine_pop_event(session->engine);
            if (!owned)
                return scoped_status(TNY_STATUS_OOM, error);
        }
    } else if (owned->ev.kind == TNY_EV_TURN_END) {
        session->active = 0;
        session->terminal = 1;
        session->drained = 1; /* terminal is guaranteed to be final */
        free(session->permission_id);
        session->permission_id = NULL;
    }
    if (owned->ev.kind == TNY_EV_ERROR) {
        int32_t status = public_error_code(owned);
        if (status == TNY_STATUS_AUTH || status == TNY_STATUS_PROTOCOL)
            session->runtime->endpoint_reachability =
                TNY_ENDPOINT_REACHABILITY_REACHABLE;
        else if (status == TNY_STATUS_IO)
            session->runtime->endpoint_reachability =
                TNY_ENDPOINT_REACHABILITY_UNREACHABLE;
    } else if ((owned->ev.kind != TNY_EV_TURN_END ||
                (owned->ev.stop != TNY_STOP_INTERRUPTED &&
                 owned->ev.stop != TNY_STOP_ERROR)) &&
               owned->ev.kind != TNY_EV_USER_MESSAGE &&
               owned->ev.kind != TNY_EV_STEER_REJECTED) {
        /* Provider-derived data or a successful terminal proves a response
         * was observed. A query never initiates this transition itself. */
        session->runtime->endpoint_reachability =
            TNY_ENDPOINT_REACHABILITY_REACHABLE;
    }
    *out = (tny_event *)owned;
    return TNY_STATUS_EVENT;
}

int32_t tny_session_steer(tny_session *session, tny_bytes value,
                          tny_error **error) {
    tny_alloc_scope_begin("session_steer");
    if (error) *error = NULL;
    if (!session) return scoped_status(failf(error, TNY_STATUS_INVALID_ARGUMENT,
                                             "session is required"), error);
    int32_t rc = require_owner(session->runtime, error);
    if (rc != TNY_STATUS_OK) return scoped_status(rc, error);
    if (!session->active)
        return scoped_status(failf(error, TNY_STATUS_BAD_STATE,
                                   "no turn is active"), error);
    char *text = NULL;
    rc = copy_bytes(value, true, "steer text", &text, error);
    if (rc != TNY_STATUS_OK) return scoped_status(rc, error);
    char errbuf[512];
    int erc = tny_engine_steer(session->engine, text, errbuf, sizeof errbuf);
    free(text);
    if (tny_alloc_scope_failed()) {
        tny_engine_fail_oom(session->engine);
        return TNY_STATUS_OK;
    }
    return erc == 0 ? TNY_STATUS_OK
                    : scoped_status(failf(error, TNY_STATUS_BAD_STATE,
                                          "%s", errbuf), error);
}

int32_t tny_session_respond_permission(tny_session *session,
                                       tny_bytes request_id,
                                       uint32_t decision, tny_error **error) {
    tny_alloc_scope_begin("respond_permission");
    if (error) *error = NULL;
    if (!session || decision > TNY_PERMISSION_DENY)
        return scoped_status(failf(error, TNY_STATUS_INVALID_ARGUMENT,
                                   "session or permission decision is invalid"), error);
    int32_t rc = require_owner(session->runtime, error);
    if (rc != TNY_STATUS_OK) return scoped_status(rc, error);
    char *id = NULL;
    rc = copy_bytes(request_id, true, "permission id", &id, error);
    if (rc != TNY_STATUS_OK) return scoped_status(rc, error);
    if (!session->permission_id || strcmp(session->permission_id, id) != 0) {
        free(id);
        return scoped_status(failf(error, TNY_STATUS_BAD_STATE,
                                   "permission id is stale or belongs to another turn"), error);
    }
    tny_perm_decision internal = decision == TNY_PERMISSION_ALLOW
        ? TNY_PERM_DECISION_ALLOW
        : decision == TNY_PERMISSION_ALLOW_ALWAYS
            ? TNY_PERM_DECISION_ALLOW_ALWAYS : TNY_PERM_DECISION_DENY;
    tny_engine_respond_permission(session->engine, id, internal);
    free(session->permission_id); session->permission_id = NULL;
    free(id);
    if (tny_alloc_scope_failed()) tny_engine_fail_oom(session->engine);
    return TNY_STATUS_OK;
}

int32_t tny_session_cancel(tny_session *session, tny_error **error) {
    tny_alloc_scope_begin("session_cancel");
    if (error) *error = NULL;
    if (!session) return scoped_status(failf(error, TNY_STATUS_INVALID_ARGUMENT,
                                             "session is required"), error);
    if (session->runtime->owner_pid != getpid()) return TNY_STATUS_BAD_STATE;
    tny_engine_request_cancel(session->engine);
    return TNY_STATUS_OK;
}

static tny_owned_event *event_owned(const tny_event *event) {
    return (tny_owned_event *)(uintptr_t)event;
}

static int32_t public_error_code(const tny_owned_event *owned) {
    if (!owned || owned->ev.kind != TNY_EV_ERROR) return TNY_STATUS_OK;
    switch (owned->ev.error_code) {
    case TNY_EVENT_ERROR_IO: return TNY_STATUS_IO;
    case TNY_EVENT_ERROR_PROTOCOL: return TNY_STATUS_PROTOCOL;
    case TNY_EVENT_ERROR_BACKPRESSURE: return TNY_STATUS_BACKPRESSURE;
    case TNY_EVENT_ERROR_AUTH: return TNY_STATUS_AUTH;
    case TNY_EVENT_ERROR_OOM: return TNY_STATUS_OOM;
    default: return TNY_STATUS_INTERNAL;
    }
}

void tny_event_view_init(tny_event_view_v0 *view) {
    if (!view) return;
    memset(view, 0, sizeof *view);
    view->struct_size = sizeof *view;
}

int32_t tny_event_read(const tny_event *event, tny_event_view_v0 *view) {
    if (!event || !view) return TNY_STATUS_INVALID_ARGUMENT;
    size_t required = offsetof(tny_event_view_v0, provider);
    if (view->struct_size < required) return TNY_STATUS_INVALID_ARGUMENT;
    uint32_t caller_size = view->struct_size;
    tny_event_view_v0 full;
    tny_event_view_init(&full);
    tny_owned_event *owned = event_owned(event);
    full.kind = public_kind(owned->ev.kind);
    full.schema_version = TNY_EVENT_SCHEMA_VERSION;
    full.tool_ok = owned->ev.tool_ok ? 1u : 0u;
    full.permission_options = (uint32_t)owned->ev.perm_options;
    full.stop_reason = (uint32_t)owned->ev.stop;
    full.error_code = public_error_code(owned);
    full.has_cost = owned->ev.has_cost ? 1u : 0u;
    full.sequence = owned->sequence;
    full.timestamp_ms = owned->timestamp_ms;
    full.input_tokens = owned->ev.in_tokens;
    full.output_tokens = owned->ev.out_tokens;
    full.context_used = owned->ev.context_used;
    full.context_size = owned->ev.context_size;
    full.cost = owned->ev.cost;
    full.provider = cstr_bytes(owned->provider);
    full.session_id = cstr_bytes(owned->session_id);
    full.turn_id = cstr_bytes(owned->turn_id);
    full.text = bytes_of(owned->ev.text, owned->ev.text_len);
    full.message_id = cstr_bytes(owned->ev.message_id);
    full.tool_name = cstr_bytes(owned->ev.tool_name);
    full.tool_id = cstr_bytes(owned->ev.tool_id);
    full.tool_detail = cstr_bytes(owned->ev.tool_detail);
    full.permission_id = cstr_bytes(owned->ev.perm_id);
    full.permission_summary = cstr_bytes(owned->ev.perm_summary);
    full.message_type = cstr_bytes(owned->ev.message_type);
    size_t n = caller_size < sizeof full ? caller_size : sizeof full;
    memcpy(view, &full, n);
    view->struct_size = caller_size;
    return TNY_STATUS_OK;
}

uint32_t tny_event_get_kind(const tny_event *event) {
    tny_owned_event *owned = event_owned(event);
    return owned ? public_kind(owned->ev.kind) : TNY_EVENT_ERROR;
}
tny_bytes tny_event_text(const tny_event *e) {
    tny_owned_event *owned = event_owned(e);
    return owned ? bytes_of(owned->ev.text, owned->ev.text_len)
                 : bytes_of(NULL, 0);
}
#define EVENT_CSTR(name, field) \
tny_bytes name(const tny_event *e) { \
    tny_owned_event *owned = event_owned(e); \
    return owned ? cstr_bytes(owned->ev.field) : bytes_of(NULL, 0); \
}
EVENT_CSTR(tny_event_tool_name, tool_name)
EVENT_CSTR(tny_event_tool_id, tool_id)
EVENT_CSTR(tny_event_tool_detail, tool_detail)
EVENT_CSTR(tny_event_permission_id, perm_id)
EVENT_CSTR(tny_event_permission_summary, perm_summary)
EVENT_CSTR(tny_event_message_type, message_type)
#undef EVENT_CSTR
uint32_t tny_event_tool_ok(const tny_event *e) {
    tny_owned_event *owned = event_owned(e);
    return owned && owned->ev.tool_ok ? 1u : 0u;
}
uint32_t tny_event_permission_options(const tny_event *e) {
    tny_owned_event *owned = event_owned(e);
    return owned ? (uint32_t)owned->ev.perm_options : 0u;
}
int64_t tny_event_input_tokens(const tny_event *e) {
    tny_owned_event *owned = event_owned(e);
    return owned ? owned->ev.in_tokens : 0;
}
int64_t tny_event_output_tokens(const tny_event *e) {
    tny_owned_event *owned = event_owned(e);
    return owned ? owned->ev.out_tokens : 0;
}
uint32_t tny_event_stop_reason(const tny_event *e) {
    tny_owned_event *owned = event_owned(e);
    return owned ? (uint32_t)owned->ev.stop : TNY_STOP_REASON_ERROR;
}
int32_t tny_event_error_code(const tny_event *e) {
    return public_error_code(event_owned(e));
}
void tny_event_free(tny_event *event) {
    tny_owned_event_free(event_owned(event));
}

int32_t tny_error_code(const tny_error *error) {
    return error ? error->code : TNY_STATUS_OK;
}
tny_bytes tny_error_message(const tny_error *error) {
    return error ? cstr_bytes(error->message) : bytes_of(NULL, 0);
}
void tny_error_free(tny_error *error) {
    if (!error) return;
    free(error->message);
    free(error);
}
