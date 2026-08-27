#define _POSIX_C_SOURCE 200809L
#include "addon_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t registry_cond = PTHREAD_COND_INITIALIZER;
static runtime_state *registry;
static uint32_t next_runtime_id = 1u;
typedef struct env_registration {
    napi_env env;
    struct env_registration *next;
} env_registration;
static env_registration *registered_envs;

int sdk_register_env_cleanup(napi_env env) {
    env_registration *entry;
    pthread_mutex_lock(&registry_mutex);
    for (entry = registered_envs; entry; entry = entry->next) {
        if (entry->env == env) { pthread_mutex_unlock(&registry_mutex); return 0; }
    }
    entry = (env_registration *)calloc(1u, sizeof(*entry));
    if (!entry) { pthread_mutex_unlock(&registry_mutex); return -1; }
    entry->env = env;
    entry->next = registered_envs;
    registered_envs = entry;
    pthread_mutex_unlock(&registry_mutex);
    return 1;
}

void sdk_unregister_env_cleanup(napi_env env) {
    env_registration **cursor;
    pthread_mutex_lock(&registry_mutex);
    for (cursor = &registered_envs; *cursor; cursor = &(*cursor)->next) {
        if ((*cursor)->env == env) {
            env_registration *removed = *cursor;
            *cursor = removed->next;
            free(removed);
            break;
        }
    }
    pthread_mutex_unlock(&registry_mutex);
}

void sdk_runtime_add(runtime_state *state) {
    pthread_mutex_lock(&registry_mutex);
    state->id = next_runtime_id++;
    if (state->id == 0u) state->id = next_runtime_id++;
    state->refs = 1u;
    state->next = registry;
    registry = state;
    pthread_mutex_unlock(&registry_mutex);
}

runtime_state *sdk_runtime_acquire(uint32_t id, napi_env env) {
    runtime_state *it;
    pthread_mutex_lock(&registry_mutex);
    for (it = registry; it && (it->id != id || it->env != env); it = it->next) {}
    if (it) it->refs++;
    pthread_mutex_unlock(&registry_mutex);
    return it;
}

void sdk_runtime_retain(runtime_state *state) {
    pthread_mutex_lock(&registry_mutex);
    state->refs++;
    pthread_mutex_unlock(&registry_mutex);
}

void sdk_runtime_release(runtime_state *state) {
    pthread_mutex_lock(&registry_mutex);
    if (state->refs > 0u) state->refs--;
    pthread_cond_broadcast(&registry_cond);
    pthread_mutex_unlock(&registry_mutex);
}

static void registry_remove(runtime_state *state) {
    runtime_state **it;
    for (it = &registry; *it; it = &(*it)->next) {
        if (*it == state) {
            *it = state->next;
            state->refs--;
            break;
        }
    }
}

void sdk_runtime_abandon(runtime_state *state) {
    if (state->token) atomic_store(&state->token->state, NULL);
    pthread_mutex_lock(&registry_mutex);
    registry_remove(state);
    while (state->refs != 0u)
        pthread_cond_wait(&registry_cond, &registry_mutex);
    pthread_mutex_unlock(&registry_mutex);
    pthread_mutex_destroy(&state->mutex);
    pthread_mutex_destroy(&state->session_mutex);
    pthread_cond_destroy(&state->cond);
    free(state);
}

void sdk_runtime_destroy(runtime_state *state) {
    if (state->token) atomic_store(&state->token->state, NULL);
    pthread_mutex_lock(&registry_mutex);
    registry_remove(state);
    pthread_mutex_unlock(&registry_mutex);
    (void)pthread_join(state->thread, NULL);
    pthread_mutex_lock(&registry_mutex);
    if (state->refs > 0u) state->refs--;
    pthread_cond_broadcast(&registry_cond);
    while (state->refs != 0u)
        pthread_cond_wait(&registry_cond, &registry_mutex);
    pthread_mutex_unlock(&registry_mutex);
    pthread_mutex_destroy(&state->mutex);
    pthread_mutex_destroy(&state->session_mutex);
    pthread_cond_destroy(&state->cond);
    free(state);
}

int sdk_queue_push(runtime_state *state, command *cmd) {
    int ok = 0;
    pthread_mutex_lock(&state->mutex);
    if (!state->closing &&
        (cmd->kind == CMD_RUNTIME_CLOSE || cmd->kind == CMD_ENV_CLEANUP) &&
        !state->priority_close) {
        state->priority_close = cmd;
        pthread_cond_signal(&state->cond);
        ok = 1;
    } else if (!state->closing && cmd->kind == CMD_ABORT && !state->priority_abort) {
        state->priority_abort = cmd;
        pthread_cond_signal(&state->cond);
        ok = 1;
    } else if (!state->closing && cmd->kind == CMD_SESSION_CLOSE &&
               !state->priority_session_close) {
        state->priority_session_close = cmd;
        pthread_cond_signal(&state->cond);
        ok = 1;
    } else if (!state->closing && state->count < COMMAND_CAPACITY) {
        size_t tail = (state->head + state->count) % COMMAND_CAPACITY;
        state->queue[tail] = cmd;
        state->count++;
        pthread_cond_signal(&state->cond);
        ok = 1;
    }
    pthread_mutex_unlock(&state->mutex);
    return ok;
}

static void queue_repush(runtime_state *state, command *cmd) {
    pthread_mutex_lock(&state->mutex);
    if (!state->pending_next) {
        state->pending_next = cmd;
    } else {
        cmd->status = TNY_STATUS_BACKPRESSURE;
        cmd->error_message = sdk_copy_cstr("another event demand is already pending");
        (void)napi_call_threadsafe_function(state->tsfn, cmd, napi_tsfn_blocking);
    }
    pthread_mutex_unlock(&state->mutex);
}

static command *queue_pop(runtime_state *state) {
    command *cmd;
    pthread_mutex_lock(&state->mutex);
    while (state->count == 0u && !state->priority_close &&
           !state->priority_session_close && !state->priority_abort &&
           !state->pending_next && !state->closing)
        pthread_cond_wait(&state->cond, &state->mutex);
    if (state->priority_close) {
        cmd = state->priority_close;
        state->priority_close = NULL;
        pthread_mutex_unlock(&state->mutex);
        return cmd;
    }
    if (state->priority_abort) {
        cmd = state->priority_abort;
        state->priority_abort = NULL;
        pthread_mutex_unlock(&state->mutex);
        return cmd;
    }
    if (state->priority_session_close) {
        cmd = state->priority_session_close;
        state->priority_session_close = NULL;
        pthread_mutex_unlock(&state->mutex);
        return cmd;
    }
    if (state->count == 0u && state->pending_next) {
        cmd = state->pending_next;
        state->pending_next = NULL;
        pthread_mutex_unlock(&state->mutex);
        return cmd;
    }
    if (state->count == 0u) {
        pthread_mutex_unlock(&state->mutex);
        return NULL;
    }
    cmd = state->queue[state->head];
    state->head = (state->head + 1u) % COMMAND_CAPACITY;
    state->count--;
    pthread_mutex_unlock(&state->mutex);
    return cmd;
}

static void complete(runtime_state *state, command *cmd) {
    napi_status status = napi_call_threadsafe_function(
        state->tsfn, cmd, napi_tsfn_blocking);
    if (status != napi_ok && atomic_load(&state->env_closing)) {
        sdk_runtime_release(state);
        sdk_free_command(cmd);
    }
}

static void fail(command *cmd, int32_t status, tny_error *error) {
    cmd->status = status;
    cmd->error_message = sdk_take_error(status, error);
}

static void fail_text(command *cmd, int32_t status, const char *message) {
    cmd->status = status;
    cmd->error_message = sdk_copy_cstr(message);
}

static void reject_queued(runtime_state *state) {
    command *cmd;
    for (;;) {
        pthread_mutex_lock(&state->mutex);
        if (state->priority_abort) {
            cmd = state->priority_abort;
            state->priority_abort = NULL;
        } else if (state->priority_session_close) {
            cmd = state->priority_session_close;
            state->priority_session_close = NULL;
        } else if (state->pending_next) {
            cmd = state->pending_next;
            state->pending_next = NULL;
        } else if (state->count != 0u) {
            cmd = state->queue[state->head];
            state->head = (state->head + 1u) % COMMAND_CAPACITY;
            state->count--;
        } else {
            pthread_mutex_unlock(&state->mutex);
            break;
        }
        pthread_mutex_unlock(&state->mutex);
        if (atomic_load(&state->env_closing)) {
            sdk_runtime_release(state);
            sdk_free_command(cmd);
        } else {
            fail_text(cmd, TNY_STATUS_BAD_STATE, "runtime is closed");
            complete(state, cmd);
        }
    }
}

static void execute_create(runtime_state *state, command *cmd) {
    tny_runtime_options_v0 options;
    tny_error *error = NULL;
    tny_bytes version;
    uint32_t abi = tny_abi_version();
    uint32_t major = abi >> 16u;
    uint32_t minor = abi & 0xffffu;
    if (major != SDK_ABI_MAJOR || minor < SDK_ABI_MINOR) {
        char message[192];
        sdk_wipe_owned_bytes(&cmd->create.api_key);
        (void)snprintf(message, sizeof(message),
                       "incompatible libtny ABI %u.%u; this SDK requires ABI %u.%u or newer within major %u",
                       major, minor, SDK_ABI_MAJOR, SDK_ABI_MINOR, SDK_ABI_MAJOR);
        fail_text(cmd, TNY_STATUS_UNSUPPORTED, message);
        cmd->destroy_owner = 1;
        state->closing = 1;
        return;
    }
    if (tny_runtime_options_init(&options, sizeof options) != TNY_STATUS_OK) {
        fail_text(cmd, TNY_STATUS_INTERNAL, "failed to initialize runtime options");
        cmd->destroy_owner = 1;
        state->closing = 1;
        return;
    }
    options.permission_mode = cmd->create.permission_mode;
    options.persistence = cmd->create.persistence;
    options.max_steps = cmd->create.max_steps;
    options.max_tool_result_bytes = cmd->create.max_tool_result_bytes;
    options.workspace = sdk_view_of(cmd->create.workspace);
    options.state_dir = sdk_view_of(cmd->create.state_dir);
    options.provider = sdk_view_of(cmd->create.provider);
    options.model = sdk_view_of(cmd->create.model);
    options.base_url = sdk_view_of(cmd->create.base_url);
    options.api_key = sdk_view_of(cmd->create.api_key);
    options.wire_api = sdk_view_of(cmd->create.wire_api);
    cmd->status = tny_runtime_create(
        &options, sizeof options, &state->runtime, &error);
    sdk_wipe_owned_bytes(&cmd->create.api_key);
    if (cmd->status != TNY_STATUS_OK) {
        fail(cmd, cmd->status, error);
        cmd->destroy_owner = 1;
        state->closing = 1;
        return;
    }
    version = tny_library_version();
    cmd->abi_version = abi;
    cmd->string_result = sdk_copy_bytes(version);
    if (!cmd->string_result) {
        (void)tny_runtime_destroy(&state->runtime);
        fail_text(cmd, TNY_STATUS_OOM, "out of memory copying library version");
        cmd->destroy_owner = 1;
        state->closing = 1;
        return;
    }
    cmd->status = sdk_snapshot_capabilities(state->runtime, &cmd->capabilities);
    if (cmd->status != TNY_STATUS_OK) {
        (void)tny_runtime_destroy(&state->runtime);
        fail_text(cmd, cmd->status, "failed to copy libtny capabilities");
        cmd->destroy_owner = 1;
        state->closing = 1;
        return;
    }
    cmd->result = RESULT_RUNTIME;
}

static int execute_command(runtime_state *state, command *cmd) {
    tny_error *error = NULL;
    int32_t status = TNY_STATUS_OK;
    if (cmd->kind != CMD_RUNTIME_CREATE && !state->runtime) {
        fail_text(cmd, TNY_STATUS_BAD_STATE, "runtime is not open");
        complete(state, cmd);
        return 0;
    }
    switch (cmd->kind) {
    case CMD_RUNTIME_CREATE:
        execute_create(state, cmd);
        break;
    case CMD_SESSION_CREATE:
        pthread_mutex_lock(&state->session_mutex);
        if (state->session) {
            pthread_mutex_unlock(&state->session_mutex);
            fail_text(cmd, TNY_STATUS_BUSY, "libtny supports one open session per runtime");
            break;
        }
        status = tny_session_create(state->runtime, &state->session, &error);
        if (status != TNY_STATUS_OK) {
            pthread_mutex_unlock(&state->session_mutex);
            fail(cmd, status, error); break;
        }
        state->session_handle = 1u;
        cmd->session_handle = state->session_handle;
        cmd->string_result = sdk_copy_bytes(tny_session_id(state->session));
        if (!cmd->string_result) {
            (void)tny_session_destroy(&state->session);
            pthread_mutex_unlock(&state->session_mutex);
            fail_text(cmd, TNY_STATUS_OOM, "out of memory copying session id");
            break;
        }
        pthread_mutex_unlock(&state->session_mutex);
        cmd->result = RESULT_SESSION;
        break;
    case CMD_SESSION_OPEN:
        pthread_mutex_lock(&state->session_mutex);
        if (state->session) {
            pthread_mutex_unlock(&state->session_mutex);
            fail_text(cmd, TNY_STATUS_BUSY, "libtny supports one open session per runtime");
            break;
        }
        status = tny_session_open(state->runtime, sdk_view_of(cmd->text), &state->session, &error);
        if (status != TNY_STATUS_OK) {
            pthread_mutex_unlock(&state->session_mutex);
            fail(cmd, status, error); break;
        }
        state->session_handle = 1u;
        cmd->session_handle = state->session_handle;
        cmd->string_result = sdk_copy_bytes(tny_session_id(state->session));
        if (!cmd->string_result) {
            (void)tny_session_destroy(&state->session);
            pthread_mutex_unlock(&state->session_mutex);
            fail_text(cmd, TNY_STATUS_OOM, "out of memory copying session id");
            break;
        }
        pthread_mutex_unlock(&state->session_mutex);
        cmd->result = RESULT_SESSION;
        break;
    case CMD_SEND:
        if (!state->session || cmd->session_handle != state->session_handle) {
            fail_text(cmd, TNY_STATUS_BAD_STATE, "session is not open");
            break;
        }
        status = tny_session_send(state->session, sdk_view_of(cmd->text), &error);
        if (status != TNY_STATUS_OK) fail(cmd, status, error);
        break;
    case CMD_NEXT: {
        tny_event *event = NULL;
        if (!state->session || cmd->session_handle != state->session_handle) {
            fail_text(cmd, TNY_STATUS_BAD_STATE, "session is not open");
            break;
        }
        status = tny_session_next_event(state->session, OWNER_POLL_MS, &event, &error);
        if (status == TNY_STATUS_TIMEOUT) {
            queue_repush(state, cmd);
            return 0;
        }
        if (status == TNY_STATUS_DRAINED) {
            cmd->status = TNY_STATUS_DRAINED;
            cmd->result = RESULT_EVENT;
            break;
        }
        if (status != TNY_STATUS_EVENT) {
            fail(cmd, status, error);
            break;
        }
        status = sdk_snapshot_event(event, &cmd->event);
        tny_event_free(event);
        if (status != TNY_STATUS_OK) {
            fail_text(cmd, status, "failed to copy libtny event");
            break;
        }
        cmd->result = RESULT_EVENT;
        break;
    }
    case CMD_STEER:
        if (!state->session || cmd->session_handle != state->session_handle) {
            fail_text(cmd, TNY_STATUS_BAD_STATE, "session is not open");
            break;
        }
        status = tny_session_steer(state->session, sdk_view_of(cmd->text), &error);
        if (status != TNY_STATUS_OK) fail(cmd, status, error);
        break;
    case CMD_PERMISSION:
        if (!state->session || cmd->session_handle != state->session_handle) {
            fail_text(cmd, TNY_STATUS_BAD_STATE, "session is not open");
            break;
        }
        status = tny_session_respond_permission(state->session, sdk_view_of(cmd->text),
                                                cmd->decision, &error);
        if (status != TNY_STATUS_OK) fail(cmd, status, error);
        break;
    case CMD_CANCEL:
    case CMD_ABORT:
        if (!state->session || cmd->session_handle != state->session_handle) {
            fail_text(cmd, TNY_STATUS_BAD_STATE, "session is not open");
            break;
        }
        status = tny_session_cancel(state->session, &error);
        if (status != TNY_STATUS_OK) fail(cmd, status, error);
        break;
    case CMD_CAPABILITIES:
        cmd->status = sdk_snapshot_capabilities(state->runtime, &cmd->capabilities);
        if (cmd->status != TNY_STATUS_OK)
            fail_text(cmd, cmd->status, "failed to copy libtny capabilities");
        else
            cmd->result = RESULT_CAPABILITIES;
        break;
    case CMD_SESSION_CLOSE:
        pthread_mutex_lock(&state->session_mutex);
        if (state->session && cmd->session_handle == state->session_handle) {
            (void)tny_session_destroy(&state->session);
            state->session_handle = 0u;
        }
        pthread_mutex_unlock(&state->session_mutex);
        break;
    case CMD_RUNTIME_CLOSE:
    case CMD_ENV_CLEANUP:
        pthread_mutex_lock(&state->mutex);
        state->closing = 1;
        pthread_mutex_unlock(&state->mutex);
        pthread_mutex_lock(&state->session_mutex);
        if (state->session) {
            (void)tny_session_destroy(&state->session);
        }
        pthread_mutex_unlock(&state->session_mutex);
        if (state->runtime) {
            (void)tny_runtime_destroy(&state->runtime);
        }
        reject_queued(state);
        cmd->destroy_owner = 1;
        break;
    }
    {
        int destroy_owner = cmd->destroy_owner;
        if (cmd->silent && !cmd->embedded) sdk_free_command(cmd);
        else complete(state, cmd);
        return destroy_owner;
    }
}

void *sdk_owner_main(void *opaque) {
    runtime_state *state = (runtime_state *)opaque;
    for (;;) {
        command *cmd = queue_pop(state);
        if (!cmd) break;
        if (execute_command(state, cmd)) break;
    }
    if (atomic_load(&state->env_closing)) {
        pthread_mutex_lock(&state->session_mutex);
        if (state->session) {
            (void)tny_session_destroy(&state->session);
        }
        pthread_mutex_unlock(&state->session_mutex);
        if (state->runtime) {
            (void)tny_runtime_destroy(&state->runtime);
        }
    } else {
        (void)napi_release_threadsafe_function(state->tsfn, napi_tsfn_release);
    }
    return NULL;
}

static void discard_queued_after_join(runtime_state *state) {
    command *commands[COMMAND_CAPACITY + 5u];
    size_t count = 0u;
    pthread_mutex_lock(&state->mutex);
#define TAKE_SPECIAL(field) do { if (state->field) { commands[count++] = state->field; state->field = NULL; } } while (0)
    TAKE_SPECIAL(priority_close);
    TAKE_SPECIAL(priority_session_close);
    TAKE_SPECIAL(priority_abort);
    TAKE_SPECIAL(pending_next);
#undef TAKE_SPECIAL
    while (state->count && count < sizeof(commands) / sizeof(commands[0])) {
        commands[count++] = state->queue[state->head];
        state->head = (state->head + 1u) % COMMAND_CAPACITY;
        state->count--;
    }
    pthread_mutex_unlock(&state->mutex);
    for (size_t index = 0u; index < count; index++) {
        if (!commands[index]->silent) sdk_runtime_release(state);
        if (!commands[index]->embedded) sdk_free_command(commands[index]);
    }
}

void sdk_cleanup_env(napi_env env) {
    pthread_mutex_lock(&registry_mutex);
    for (runtime_state *state = registry; state; state = state->next) {
        if (state->env != env) continue;
        atomic_store(&state->env_closing, 1);
        pthread_mutex_lock(&state->session_mutex);
        if (state->session) {
            tny_error *error = NULL;
            (void)tny_session_cancel(state->session, &error);
            if (error) tny_error_free(error);
        }
        pthread_mutex_unlock(&state->session_mutex);
        pthread_mutex_lock(&state->mutex);
        state->closing = 1;
        pthread_cond_signal(&state->cond);
        pthread_mutex_unlock(&state->mutex);
    }
    pthread_mutex_unlock(&registry_mutex);
    sdk_unregister_env_cleanup(env);
}

void sdk_finalize_env_state(runtime_state *state) {
    (void)pthread_join(state->thread, NULL);
    discard_queued_after_join(state);
    sdk_runtime_abandon(state);
}
