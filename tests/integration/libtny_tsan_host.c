#define _POSIX_C_SOURCE 200809L

#include "tny/tny.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CANCEL_THREADS 4
#define CANCEL_ROUNDS  100
#define DEFAULT_CYCLES 12
#define PATH_CAP       1024
#define ID_CAP         128

typedef struct shared_state shared_state;

typedef struct {
    shared_state *shared;
    int index;
    const char *root;
    const char *base_url;
    char session_id[ID_CAP];
    int result;
} owner_arg;

struct shared_state {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    tny_session **sessions;
    int workers;
    int ready;
    int startup_failed;
    int cancelers_done;
    int cancel_failure;
};

typedef struct {
    shared_state *shared;
    int index;
} cancel_arg;

static void publish_startup(shared_state *shared, int index, tny_session *session, int failed) {
    pthread_mutex_lock(&shared->mutex);
    shared->sessions[index] = session;
    shared->startup_failed |= failed;
    shared->ready++;
    pthread_cond_broadcast(&shared->condition);
    pthread_mutex_unlock(&shared->mutex);
}

static tny_bytes bytes(const char *value) {
    tny_bytes out = {value, value == NULL ? 0 : (uint64_t)strlen(value)};
    return out;
}

static int report_error(const char *operation, int32_t status, tny_error *error) {
    fprintf(stderr, "tsan-host: %s failed with status %d", operation, (int)status);
    if (error != NULL) {
        tny_bytes message = tny_error_message(error);
        fprintf(stderr, ": %.*s", (int)message.len, message.ptr);
        tny_error_free(error);
    }
    fputc('\n', stderr);
    return 1;
}

static int copy_session_id(tny_session *session, char out[ID_CAP]) {
    tny_bytes id = tny_session_id(session);
    if (id.ptr == NULL || id.len == 0 || id.len >= ID_CAP) return 1;
    memcpy(out, id.ptr, (size_t)id.len);
    out[id.len] = '\0';
    return 0;
}

static void *owner_main(void *opaque) {
    owner_arg *arg = opaque;
    shared_state *shared = arg->shared;
    char workspace[PATH_CAP];
    char state_dir[PATH_CAP];
    char api_key[64];
    if (snprintf(workspace, sizeof workspace, "%s/workspace-%d", arg->root, arg->index) >=
            (int)sizeof workspace ||
        snprintf(state_dir, sizeof state_dir, "%s/state-%d", arg->root, arg->index) >=
            (int)sizeof state_dir ||
        snprintf(api_key, sizeof api_key, "tsan-owner-%d-not-real", arg->index) >=
            (int)sizeof api_key) {
        arg->result = 1;
        publish_startup(shared, arg->index, NULL, 1);
        return NULL;
    }

    tny_runtime_options_v0 options;
    if (tny_runtime_options_init(&options, sizeof options) != TNY_STATUS_OK) {
        arg->result = report_error("runtime_options_init", TNY_STATUS_INTERNAL, NULL);
        publish_startup(shared, arg->index, NULL, 1);
        return NULL;
    }
    options.workspace = bytes(workspace);
    options.state_dir = bytes(state_dir);
    options.provider = bytes("openai");
    options.base_url = bytes(arg->base_url);
    options.api_key = bytes(api_key);
    options.permission_mode = TNY_PERMISSION_YOLO;
    options.persistence = 1;

    tny_runtime *runtime = NULL;
    tny_session *session = NULL;
    tny_error *error = NULL;
    int32_t status = tny_runtime_create(&options, sizeof options, &runtime, &error);
    if (status != TNY_STATUS_OK) {
        arg->result = report_error("runtime_create", status, error);
        publish_startup(shared, arg->index, NULL, 1);
        return NULL;
    }

    tny_capabilities_v0 capabilities;
    if (tny_capabilities_init(&capabilities, sizeof capabilities) != TNY_STATUS_OK) {
        arg->result = report_error("capabilities_init", TNY_STATUS_INTERNAL, NULL);
        tny_runtime_free(runtime);
        publish_startup(shared, arg->index, NULL, 1);
        return NULL;
    }
    status = tny_runtime_get_capabilities(runtime, &capabilities, sizeof capabilities);
    if (status != TNY_STATUS_OK || capabilities.threading_model != TNY_THREADING_OWNER_THREAD ||
        capabilities.cancel_model != TNY_CANCEL_CROSS_THREAD_ASYNC_WAKE ||
        !(capabilities.feature_enabled_mask & TNY_CAP_FEATURE_CROSS_THREAD_CANCEL)) {
        arg->result = report_error("runtime_get_capabilities", status, error);
        tny_runtime_free(runtime);
        publish_startup(shared, arg->index, NULL, 1);
        return NULL;
    }

    status = tny_session_create(runtime, &session, &error);
    if (status != TNY_STATUS_OK) {
        arg->result = report_error("session_create", status, error);
        tny_runtime_free(runtime);
        publish_startup(shared, arg->index, NULL, 1);
        return NULL;
    }
    if (copy_session_id(session, arg->session_id)) {
        fprintf(stderr, "tsan-host: invalid session id\n");
        arg->result = 1;
        tny_session_free(session);
        tny_runtime_free(runtime);
        publish_startup(shared, arg->index, NULL, 1);
        return NULL;
    }

    status = tny_session_send(session, bytes("block, then cancel"), &error);
    if (status != TNY_STATUS_OK) {
        arg->result = report_error("session_send", status, error);
        tny_session_free(session);
        tny_runtime_free(runtime);
        publish_startup(shared, arg->index, NULL, 1);
        return NULL;
    }

    publish_startup(shared, arg->index, session, 0);

    int terminals = 0;
    int interrupted = 0;
    int drained = 0;
    for (int attempts = 0; attempts < 40; attempts++) {
        tny_event *event = NULL;
        status = tny_session_next_event(session, 500, &event, &error);
        if (status == TNY_STATUS_TIMEOUT) continue;
        if (status == TNY_STATUS_DRAINED) {
            drained = 1;
            break;
        }
        if (status != TNY_STATUS_EVENT || event == NULL) {
            arg->result = report_error("session_next_event", status, error);
            break;
        }
        if (tny_event_get_kind(event) == TNY_EVENT_TURN_END) {
            terminals++;
            if (tny_event_stop_reason(event) == TNY_STOP_REASON_INTERRUPTED) interrupted++;
        }
        tny_event_free(event);
    }
    if (terminals != 1 || interrupted != 1 || !drained) {
        fprintf(stderr, "tsan-host: owner %d got terminals=%d interrupted=%d drained=%d\n",
                arg->index, terminals, interrupted, drained);
        arg->result = 1;
    }

    pthread_mutex_lock(&shared->mutex);
    while (!shared->cancelers_done) pthread_cond_wait(&shared->condition, &shared->mutex);
    shared->sessions[arg->index] = NULL;
    pthread_mutex_unlock(&shared->mutex);

    if (!arg->result) {
        status = tny_session_send(session, bytes("post-cancel isolation turn"), &error);
        if (status != TNY_STATUS_OK) {
            arg->result = report_error("post-cancel session_send", status, error);
        } else {
            int done_terminals = 0;
            int second_drained = 0;
            for (int attempts = 0; attempts < 80; attempts++) {
                tny_event *event = NULL;
                status = tny_session_next_event(session, 500, &event, &error);
                if (status == TNY_STATUS_TIMEOUT) continue;
                if (status == TNY_STATUS_DRAINED) {
                    second_drained = 1;
                    break;
                }
                if (status != TNY_STATUS_EVENT || event == NULL) {
                    arg->result = report_error("post-cancel next_event", status, error);
                    break;
                }
                if (tny_event_get_kind(event) == TNY_EVENT_TURN_END &&
                    tny_event_stop_reason(event) == TNY_STOP_REASON_DONE)
                    done_terminals++;
                tny_event_free(event);
            }
            if (done_terminals != 1 || !second_drained) {
                fprintf(stderr, "tsan-host: owner %d post-cancel terminals=%d drained=%d\n",
                        arg->index, done_terminals, second_drained);
                arg->result = 1;
            }
        }
    }

    tny_session_free(session);
    tny_runtime_free(runtime);
    return NULL;
}

static void mark_cancel_failure(shared_state *shared, const char *operation, int32_t status,
                                tny_error *error) {
    report_error(operation, status, error);
    pthread_mutex_lock(&shared->mutex);
    shared->cancel_failure = 1;
    pthread_mutex_unlock(&shared->mutex);
}

static void *cancel_main(void *opaque) {
    cancel_arg *arg = opaque;
    shared_state *shared = arg->shared;
    for (int round = 0; round < CANCEL_ROUNDS; round++) {
        for (int index = 0; index < shared->workers; index++) {
            tny_session *session = shared->sessions[index];
            if (arg->index == 0 && round == 0) {
                tny_error *wrong_error = NULL;
                int32_t wrong =
                    tny_session_steer(session, bytes("wrong-thread-mutation"), &wrong_error);
                if (wrong != TNY_STATUS_BAD_STATE)
                    mark_cancel_failure(shared, "wrong-thread steer", wrong, wrong_error);
                else if (wrong_error != NULL) tny_error_free(wrong_error);
            }
            tny_error *error = NULL;
            int32_t status = tny_session_cancel(session, &error);
            if (status != TNY_STATUS_OK || error != NULL)
                mark_cancel_failure(shared, "session_cancel", status, error);
        }
    }
    return NULL;
}

static int parse_cycles(void) {
    const char *value = getenv("TNY_TSAN_CYCLES");
    if (value == NULL || *value == '\0') return DEFAULT_CYCLES;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (*end != '\0' || parsed < 1 || parsed > 1000) return -1;
    return (int)parsed;
}

static int run_cycle(int workers, const char *root, char **urls, int cycle) {
    shared_state shared;
    memset(&shared, 0, sizeof shared);
    shared.workers = workers;
    shared.sessions = calloc((size_t)workers, sizeof(*shared.sessions));
    owner_arg *owners = calloc((size_t)workers, sizeof(*owners));
    pthread_t *owner_threads = calloc((size_t)workers, sizeof(*owner_threads));
    if (shared.sessions == NULL || owners == NULL || owner_threads == NULL) return 1;
    if (pthread_mutex_init(&shared.mutex, NULL) != 0 ||
        pthread_cond_init(&shared.condition, NULL) != 0)
        return 1;

    char cycle_root[PATH_CAP];
    if (snprintf(cycle_root, sizeof cycle_root, "%s/cycle-%d", root, cycle) >=
        (int)sizeof cycle_root)
        return 1;
    for (int index = 0; index < workers; index++) {
        owners[index].shared = &shared;
        owners[index].index = index;
        owners[index].root = cycle_root;
        owners[index].base_url = urls[index];
        if (pthread_create(&owner_threads[index], NULL, owner_main, &owners[index]) != 0) return 1;
    }

    pthread_mutex_lock(&shared.mutex);
    while (shared.ready != workers) pthread_cond_wait(&shared.condition, &shared.mutex);
    int startup_failed = shared.startup_failed;
    pthread_mutex_unlock(&shared.mutex);

    if (startup_failed) {
        for (int index = 0; index < workers; index++) {
            if (shared.sessions[index] != NULL) tny_session_cancel(shared.sessions[index], NULL);
        }
        pthread_mutex_lock(&shared.mutex);
        shared.cancelers_done = 1;
        pthread_cond_broadcast(&shared.condition);
        pthread_mutex_unlock(&shared.mutex);
        for (int index = 0; index < workers; index++) pthread_join(owner_threads[index], NULL);
        return 1;
    }

    struct timespec settle = {0, 100 * 1000 * 1000};
    nanosleep(&settle, NULL);
    pthread_t cancel_threads[CANCEL_THREADS];
    cancel_arg cancelers[CANCEL_THREADS];
    for (int index = 0; index < CANCEL_THREADS; index++) {
        cancelers[index].shared = &shared;
        cancelers[index].index = index;
        if (pthread_create(&cancel_threads[index], NULL, cancel_main, &cancelers[index]) != 0)
            return 1;
    }
    for (int index = 0; index < CANCEL_THREADS; index++) pthread_join(cancel_threads[index], NULL);

    pthread_mutex_lock(&shared.mutex);
    shared.cancelers_done = 1;
    pthread_cond_broadcast(&shared.condition);
    pthread_mutex_unlock(&shared.mutex);

    int failed = shared.cancel_failure;
    for (int index = 0; index < workers; index++) {
        pthread_join(owner_threads[index], NULL);
        failed |= owners[index].result;
        for (int other = 0; other < index; other++) {
            if (strcmp(owners[index].session_id, owners[other].session_id) == 0) {
                fprintf(stderr, "tsan-host: runtimes shared a session id\n");
                failed = 1;
            }
        }
    }
    pthread_cond_destroy(&shared.condition);
    pthread_mutex_destroy(&shared.mutex);
    free(owner_threads);
    free(owners);
    free(shared.sessions);
    return failed;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s ROOT URL URL [URL ...]\n", argv[0]);
        return 2;
    }
    int cycles = parse_cycles();
    if (cycles < 1) {
        fprintf(stderr, "TNY_TSAN_CYCLES must be an integer from 1 to 1000\n");
        return 2;
    }
    int workers = argc - 2;
    for (int cycle = 0; cycle < cycles; cycle++) {
        if (run_cycle(workers, argv[1], &argv[2], cycle)) return 1;
    }
    printf("libtny-tsan: %d cycles, %d isolated runtimes, %d cancels/session/cycle\n", cycles,
           workers, CANCEL_THREADS * CANCEL_ROUNDS);
    return 0;
}
