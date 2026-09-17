/* Native request admission: send -> provider-request control, before HTTP write.
 * A preflight opens the connection; each measured send gets a fresh session.
 * Run only through bench_requests.py, which supplies an isolated environment. */
#include "backends/openai/openai.h"
#include "core/config.h"
#include "util/alloc.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HISTORY_MESSAGES 16
#define MESSAGE_BYTES    4096

static void require(bool ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "request benchmark: %s\n", message);
        exit(1);
    }
}

static uint64_t nanoseconds(void) {
    struct timespec now;
    require(clock_gettime(CLOCK_MONOTONIC, &now) == 0, "monotonic clock failed");
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static uint64_t peak_rss(void) {
    struct rusage usage;
    require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
    uint64_t bytes = (uint64_t)usage.ru_maxrss;
#ifndef __APPLE__
    bytes *= 1024; /* Linux reports KiB; Darwin reports bytes. */
#endif
    return bytes;
}

static size_t fd_count(void) {
#ifdef __APPLE__
    const char *path = "/dev/fd";
#else
    const char *path = "/proc/self/fd";
#endif
    DIR *dir = opendir(path);
    require(dir != NULL, "cannot inspect open descriptors");
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] != '.') count++;
    }
    require(closedir(dir) == 0, "descriptor inspection cleanup failed");
    return count - 1; /* exclude the inspection directory itself */
}

typedef struct {
    const char *wire;
    bool measured;
    unsigned requests, ends, errors, other_controls;
    uint64_t start, elapsed;
    size_t allocations, owners;
} measurement;

static void control(const tny_openai_control_request *request,
                    tny_openai_control_response *response, void *ud) {
    measurement *m = ud;
    if (request->kind != TNY_OPENAI_CONTROL_PROVIDER_REQUEST) {
        m->other_controls++;
        return;
    }
    /* Read the clock first: callback validation is outside the measured scope. */
    uint64_t end = nanoseconds();
    m->allocations = tny_alloc_test_scope_count();
    m->owners = tny_alloc_test_owned_live();
    m->elapsed = end - m->start;
    m->requests++;
    response->stop = true;
    require(end >= m->start, "clock went backwards");
    require(!tny_alloc_scope_failed() && !tny_alloc_test_scope_injected(), "allocation failure");
    require(request->wire_api && strcmp(request->wire_api, m->wire) == 0, "wrong wire API");
    const char *endpoint = strcmp(m->wire, "chat") == 0 ? "/chat/completions" : "/responses";
    require(request->endpoint && strcmp(request->endpoint, endpoint) == 0, "wrong endpoint");
    require(request->method && strcmp(request->method, "POST") == 0 && request->stream,
            "wrong request shape");
    require(!m->measured || request->connection_reused, "connection opened inside timed scope");
}

static void event(const tny_backend_event *ev, void *ud) {
    measurement *m = ud;
    if (ev->kind == TNY_EV_ERROR) m->errors++;
    if (ev->kind == TNY_EV_TURN_END) {
        m->ends++;
        require(ev->stop == TNY_STOP_INTERRUPTED, "unexpected terminal reason");
    }
}

static void bind_session(tny_backend *backend, tny_session_state *session, perm_engine *perm,
                         measurement *m) {
    tny_backend_openai_bind(backend, session, perm, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                            control, m);
}

static void send_once(tny_backend *backend, measurement *m) {
    char error[256] = {0};
    tny_alloc_scope_begin("request-benchmark");
    m->start = nanoseconds();
    int rc = backend->send(backend, "Summarize the preceding fixed transcript.", NULL, event, m,
                           error, sizeof error);
    require(rc == 0, error[0] ? error : "send failed");
    require(m->requests == 1 && m->ends == 1 && m->errors == 0 && m->other_controls == 0,
            "request/terminal/error control oracle failed");
    require(!tny_alloc_scope_failed() && !tny_alloc_test_scope_injected(),
            "send allocation failure");
    require(m->elapsed > 0 && m->allocations > 0, "timer or instrumentation inactive");
}

static int listener(char *url, size_t capacity) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "socket failed");
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(fd, (struct sockaddr *)&addr, sizeof addr) == 0 && listen(fd, 1) == 0,
            "loopback listener failed");
    socklen_t length = sizeof addr;
    require(getsockname(fd, (struct sockaddr *)&addr, &length) == 0, "getsockname failed");
    require(fcntl(fd, F_SETFL, O_NONBLOCK) == 0, "listener nonblocking failed");
    snprintf(url, capacity, "http://127.0.0.1:%u/v1", (unsigned)ntohs(addr.sin_port));
    return fd;
}

static void sample(const char *wire, const char *workspace, const char *state, unsigned index,
                   bool warmup) {
    size_t initial_owners = tny_alloc_test_owned_live(), initial_fds = fd_count();
    char url[128], error[256];
    int server = listener(url, sizeof url);
    tny_alloc_scope_begin("setup");
    tny_ctx *ctx = tny_ctx_new_explicit(workspace, state);
    require(ctx != NULL, "context creation failed");
    free(ctx->base_url);
    ctx->base_url = xstrdup(url);
    free(ctx->api_key);
    ctx->api_key = xstrdup("request-benchmark-not-a-secret");
    free(ctx->wire_api);
    ctx->wire_api = xstrdup(wire);
    free(ctx->model);
    ctx->model = xstrdup("request-benchmark-model");
    ctx->no_save = true;
    ctx->context_enabled = false;
    ctx->library_mode = false; /* advertise the ordinary native built-in tool schemas */
    ctx->perm_mode = TNY_MODE_YOLO;
    perm_engine *perm = perm_new(ctx);
    tny_session_state *session = session_new(ctx);
    tny_backend *backend = tny_backend_openai_new(ctx);
    require(perm && session && backend, "fixture construction failed");
    measurement preflight = {.wire = wire};
    bind_session(backend, session, perm, &preflight);
    require(backend->connect(backend, error, sizeof error) == 0, "connect failed");
    require(backend->create_or_resume(backend, NULL, error, sizeof error) == 0, "create failed");
    send_once(backend, &preflight); /* opens TCP; stop before any HTTP write */
    struct pollfd connected = {.fd = server, .events = POLLIN};
    require(tny_poll(&connected, 1, 1000) > 0, "preflight connection timed out");
    int peer = accept(server, NULL, NULL);
    require(peer >= 0 && fcntl(peer, F_SETFL, O_NONBLOCK) == 0, "preflight did not connect");

    /* Rebind before freeing the old session. No timed iteration grows history. */
    tny_session_state *fresh = session_new(ctx);
    require(fresh != NULL, "fresh session failed");
    measurement m = {.wire = wire, .measured = true};
    bind_session(backend, fresh, perm, &m);
    session_close(session);
    session = fresh;
    char content[MESSAGE_BYTES + 1];
    for (int i = 0; i < HISTORY_MESSAGES; i++) {
        for (int j = 0; j < MESSAGE_BYTES; j++) content[j] = (char)('a' + (i + j) % 26);
        content[MESSAGE_BYTES] = '\0';
        session_add_text(session, i % 2 ? "assistant" : "user", content);
    }
    require(yyjson_mut_arr_size(session_messages(session)) == HISTORY_MESSAGES,
            "wrong initial history size");
    require(!tny_alloc_scope_failed(), "setup allocation failure");
    size_t prepared_owners = tny_alloc_test_owned_live();
    send_once(backend, &m);
    require(yyjson_mut_arr_size(session_messages(session)) == HISTORY_MESSAGES + 1,
            "unexpected history growth");
    char byte;
    require(recv(peer, &byte, 1, 0) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
            "HTTP bytes written before stop");
    backend->destroy(backend);
    perm_free(perm);
    session_close(session);
    tny_ctx_free(ctx);
    /* Closed TCP stream must contain no buffered HTTP bytes either. */
    struct pollfd closed = {.fd = peer, .events = POLLIN};
    require(tny_poll(&closed, 1, 1000) > 0, "connection did not close within one second");
    require(recv(peer, &byte, 1, 0) == 0, "connection not closed cleanly or HTTP bytes queued");
    require(close(peer) == 0 && close(server) == 0, "socket cleanup failed");
    size_t final_owners = tny_alloc_test_owned_live(), final_fds = fd_count();
    require(initial_owners == final_owners, "persistent owner leak");
    require(initial_fds == final_fds, "descriptor leak");
    require(!tny_alloc_scope_failed(), "cleanup allocation failure");
    printf("{\"wire\":\"%s\",\"index\":%u,\"warmup\":%s,\"nanoseconds\":%" PRIu64
           ",\"allocations\":%zu,\"peak_rss_bytes\":%" PRIu64
           ",\"owners_before\":%zu,\"owners_prepared\":%zu,\"owners_boundary\":%zu,"
           "\"owners_after\":%zu,\"fds_before\":%zu,\"fds_after\":%zu,"
           "\"request_controls\":%u,\"turn_ends\":%u,\"errors\":%u,\"http_bytes\":0,"
           "\"history_messages\":%d,\"message_bytes\":%d}\n",
           wire, index, warmup ? "true" : "false", m.elapsed, m.allocations, peak_rss(),
           initial_owners, prepared_owners, m.owners, final_owners, initial_fds, final_fds,
           m.requests, m.ends, m.errors, HISTORY_MESSAGES, MESSAGE_BYTES);
}

static unsigned number(const char *value) {
    char *end = NULL;
    errno = 0;
    unsigned long n = strtoul(value, &end, 10);
    require(!errno && end != value && !*end && n <= 100000, "invalid iteration count");
    return (unsigned)n;
}

int main(int argc, char **argv) {
    require(argc == 6, "usage: bench-requests responses|chat ITERATIONS WARMUPS WORKSPACE STATE");
    require(strcmp(argv[1], "responses") == 0 || strcmp(argv[1], "chat") == 0, "invalid wire");
    unsigned iterations = number(argv[2]), warmups = number(argv[3]);
    require(iterations > 0, "iterations must be positive");
    require(!getenv("TNY_TEST_ALLOC_SCOPE") && !getenv("TNY_TEST_ALLOC_FAIL_AT"),
            "ambient allocator injection");
    for (unsigned i = 0; i < warmups + iterations; i++)
        sample(argv[1], argv[4], argv[5], i, i < warmups);
    return 0;
}
