/* Fixed callback -> retained queue -> release workload through the real engine.
 * No provider, network, Python extension or persistence is invoked. Build and
 * compare unchanged C and migrated ownership using bench_events.py. */
#include "core/runtime.h"
#include "util/alloc.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#define EVENTS_PER_TURN 64
#define FNV_OFFSET      UINT64_C(14695981039346656037)
#define FNV_PRIME       UINT64_C(1099511628211)

static const char expected_text[] = "a copied\0payload with a binary length";
static const char expected_string[] = "retained callback payload";

typedef struct {
    tny_backend_event_cb callback;
    void *user_data;
    bool dispatched;
} benchmark_backend;

static int connect_backend(tny_backend *b, char *error, size_t capacity) {
    (void)b;
    (void)error;
    (void)capacity;
    return 0;
}
static void disconnect_backend(tny_backend *b) { (void)b; }
static int send_backend(tny_backend *b, const char *prompt, const char **images,
                        tny_backend_event_cb callback, void *user_data, char *error,
                        size_t capacity) {
    (void)prompt;
    (void)images;
    (void)error;
    (void)capacity;
    benchmark_backend *state = b->impl;
    state->callback = callback;
    state->user_data = user_data;
    state->dispatched = false;
    return 0;
}
static int poll_backend(tny_backend *b, struct pollfd *fds, int capacity) {
    (void)b;
    (void)fds;
    (void)capacity;
    return 0;
}
static int dispatch_backend(tny_backend *b, struct pollfd *fds, int count) {
    (void)fds;
    (void)count;
    benchmark_backend *state = b->impl;
    if (state->dispatched) return 0;
    state->dispatched = true;
    for (int i = 0; i < EVENTS_PER_TURN; i++) {
        char text[sizeof expected_text];
        char value[sizeof expected_string];
        memcpy(text, expected_text, sizeof text);
        memcpy(value, expected_string, sizeof value);
        tny_backend_event event = {0};
        event.kind = TNY_EV_STATUS;
        event.text = text;
        event.text_len = sizeof text - 1;
        event.message_id = value;
        event.tool_name = value;
        event.tool_id = value;
        event.tool_detail = value;
        event.perm_id = value;
        event.perm_summary = value;
        event.message_type = value;
        state->callback(&event, state->user_data);
        /* Mutate every source after the synchronous borrowed-view boundary. */
        memset(text, 'x', sizeof text);
        memset(value, 'y', sizeof value);
    }
    tny_backend_event terminal = {0};
    terminal.kind = TNY_EV_TURN_END;
    terminal.stop = TNY_STOP_DONE;
    state->callback(&terminal, state->user_data);
    state->callback(&terminal, state->user_data); /* exactly one survives */
    return 0;
}
static void destroy_backend(tny_backend *b) {
    free(b->impl);
    free(b);
}
static tny_backend *new_backend(void) {
    tny_backend *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->impl = calloc(1, sizeof(benchmark_backend));
    if (!b->impl) {
        free(b);
        return NULL;
    }
    b->id = TNY_BK_COUNT;
    b->connect = connect_backend;
    b->disconnect = disconnect_backend;
    b->send = send_backend;
    b->pollfds = poll_backend;
    b->dispatch = dispatch_backend;
    b->destroy = destroy_backend;
    return b;
}
static void hash_bytes(uint64_t *hash, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        *hash ^= (unsigned char)data[i];
        *hash *= FNV_PRIME;
    }
}
static bool observe(const tny_owned_event *event, uint64_t *hash) {
    const tny_backend_event *e = &event->ev;
    const char *strings[] = {e->message_id, e->tool_name,    e->tool_id,     e->tool_detail,
                             e->perm_id,    e->perm_summary, e->message_type};
    if (!e->text || e->text_len != sizeof expected_text - 1 ||
        memcmp(e->text, expected_text, e->text_len) != 0 || !event->provider ||
        !event->session_id || !event->turn_id)
        return false;
    hash_bytes(hash, e->text, e->text_len);
    for (size_t i = 0; i < sizeof strings / sizeof *strings; i++) {
        if (!strings[i] || strcmp(strings[i], expected_string) != 0) return false;
        hash_bytes(hash, strings[i], strlen(strings[i]));
    }
    return true;
}
static uint64_t nanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("usage: bench-events ITERATIONS WORKSPACE\n", stderr);
        return 2;
    }
    char *end = NULL;
    errno = 0;
    unsigned long iterations = strtoul(argv[1], &end, 10);
    if (errno || !end || *end || iterations < 1 || iterations > 1000000) return 2;
    tny_ctx *ctx = tny_ctx_load(argv[2]);
    if (!ctx) return 1;
    ctx->backend = TNY_BK_COUNT;
    ctx->no_save = true;
    ctx->extensions_enabled = false;
    ctx->max_extension_iterations = 0;
    tny_session_state *session = session_new(ctx);
    perm_engine *perm = perm_new(ctx);
    tny_engine *engine = tny_engine_new(ctx, session, perm, NULL, NULL);
    char error[256] = {0};
    bool ok = engine && tny_engine_prepare(engine, new_backend(), TNY_ENGINE_PREPARE_FRESH, error,
                                           sizeof error) == 0;
    uint64_t hash = FNV_OFFSET, events = 0, payload_bytes = 0, sequence = 0;
    tny_alloc_scope_begin("event_benchmark");
    uint64_t started = nanoseconds();
    for (unsigned long turn = 0; turn < iterations && ok; turn++) {
        ok = tny_engine_start(engine, "benchmark", NULL, error, sizeof error) == 0;
        unsigned int statuses = 0, terminals = 0;
        size_t logical_turn_bytes = 0;
        while (ok) {
            tny_owned_event *event = NULL;
            tny_engine_next next = tny_engine_next_event(engine, 0, &event, error, sizeof error);
            if (next == TNY_ENGINE_NEXT_DRAINED) break;
            if (next != TNY_ENGINE_NEXT_EVENT || !event) {
                ok = false;
                break;
            }
            ok = event->sequence > sequence;
            sequence = event->sequence;
            if (event->ev.kind == TNY_EV_STATUS) {
                ok = ok && !terminals && observe(event, &hash);
                statuses++;
                logical_turn_bytes += event->owned_bytes;
                events++;
            } else if (event->ev.kind == TNY_EV_TURN_END) {
                ok = ok && event->ev.stop == TNY_STOP_DONE;
                terminals++;
            }
            tny_owned_event_free(event);
        }
        ok = ok && statuses == EVENTS_PER_TURN && terminals == 1;
        if (logical_turn_bytes > payload_bytes) payload_bytes = logical_turn_bytes;
    }
    uint64_t finished = nanoseconds();
    size_t allocations = tny_alloc_test_scope_count();
    struct rusage usage;
    ok = ok && !tny_alloc_scope_failed() && getrusage(RUSAGE_SELF, &usage) == 0;
    uint64_t peak = ok ? (uint64_t)usage.ru_maxrss : 0;
#ifndef __APPLE__
    peak *= 1024;
#endif
    ok = ok && started && finished >= started && events == iterations * EVENTS_PER_TURN;
    if (ok)
        printf("{\"mode\":\"events\",\"fragmentation\":\"callback\",\"iterations\":%lu,"
               "\"nanoseconds\":%" PRIu64 ",\"input_bytes\":%" PRIu64 ",\"events\":%" PRIu64
               ",\"checksum\":\"%016" PRIx64 "\",\"allocations\":%zu,\"peak_rss_bytes\":%" PRIu64
               ",\"peak_logical_payload_bytes\":%" PRIu64 "}\n",
               iterations, finished - started,
               events * ((sizeof expected_text - 1) + 7 * (sizeof expected_string - 1)), events,
               hash, allocations, peak, payload_bytes);
    else fprintf(stderr, "event benchmark failed ownership/ordering/terminal oracle: %s\n", error);
    tny_engine_free(engine);
    perm_free(perm);
    session_close(session);
    tny_ctx_free(ctx);
    return ok ? 0 : 1;
}
