/* test_cursor_callbacks.c — sdk.v1 callback execution and persistence. */
#include "backends/cursor/callbacks.h"
#include "greatest.h"
#include "lib/custom_tools.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef enum { TOOL_SYNC, TOOL_ASYNC } tool_mode;
typedef struct {
    tool_mode mode;
    int calls;
    tny_tool_call *call;
    uint64_t generation;
} tool_state;

static tny_bytes bytes(const char *value) { return (tny_bytes){value, strlen(value)}; }

static int32_t invoke_tool(void *ud, tny_tool_call *call, uint64_t generation, tny_bytes args,
                           tny_tool_result_v1 *result) {
    tool_state *state = ud;
    state->calls++;
    state->call = call;
    state->generation = generation;
    if (!args.ptr || !args.len) return TNY_STATUS_INVALID_ARGUMENT;
    if (state->mode == TOOL_ASYNC) return TNY_TOOL_INVOKE_ASYNC;
    memset(result, 0, sizeof *result);
    result->abi_version = TNY_TOOL_RESULT_ABI_VERSION;
    result->struct_size = sizeof *result;
    result->data = bytes("plain text result");
    return TNY_TOOL_INVOKE_SYNC;
}

static tny_tool_registration *register_test_tool(custom_tool_registry *registry, const char *name,
                                                 tool_state *state, bool sensitive) {
    tny_tool_spec_v1 spec = {0};
    spec.abi_version = TNY_TOOL_SPEC_ABI_VERSION;
    spec.struct_size = sizeof spec;
    spec.name = bytes(name);
    spec.description = bytes("Callback fixture tool");
    spec.input_schema_json =
        bytes("{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}");
    spec.sensitivity = sensitive ? TNY_TOOL_SENSITIVITY_SENSITIVE : TNY_TOOL_SENSITIVITY_SAFE;
    spec.invoke = invoke_tool;
    spec.user_data = state;
    tny_tool_registration *registration = NULL;
    return custom_tools_register(registry, NULL, &spec, &registration) == TNY_STATUS_OK
               ? registration
               : NULL;
}

static int reject_thread_create(pthread_t *thread, const pthread_attr_t *attr,
                                void *(*start)(void *), void *arg) {
    (void)thread;
    (void)attr;
    (void)start;
    (void)arg;
    return EAGAIN;
}

static int callback_step(cursor_callbacks *callbacks, int timeout_ms) {
    struct pollfd fds[CURSOR_CALLBACK_POLLFD_CAPACITY];
    int count = cursor_callbacks_pollfds(callbacks, fds, CURSOR_CALLBACK_POLLFD_CAPACITY);
    int rc = tny_poll(fds, (nfds_t)count, timeout_ms);
    if (rc < 0 && errno == EINTR) return 0;
    return rc < 0 ? -1 : cursor_callbacks_dispatch(callbacks, fds, count);
}

static int open_client(cursor_callbacks *callbacks) {
    const char *url = cursor_callbacks_url(callbacks);
    const char *colon = url ? strrchr(url, ':') : NULL;
    if (!colon) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)atoi(colon + 1));
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_request(cursor_callbacks *callbacks, int fd, const char *path, const char *token,
                        const char *body) {
    buf_t request;
    buf_init(&request);
    buf_appendf(&request,
                "POST %s HTTP/1.1\r\nAuthorization: Bearer %s\r\n"
                "Connect-Protocol-Version: 1\r\nContent-Type: application/json\r\n"
                "Content-Length: %zu\r\n\r\n%s",
                path, token, strlen(body), body);
    size_t offset = 0;
    for (int spin = 0; offset < request.len && spin < 2000; spin++) {
        ssize_t n = send(fd, request.data + offset, request.len - offset, 0);
        if (n > 0) offset += (size_t)n;
        else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) break;
        if (callback_step(callbacks, 0) != 0) break;
    }
    buf_free(&request);
    return offset > 0 ? 0 : -1;
}

static int collect(cursor_callbacks *callbacks, int fd, buf_t *response) {
    for (int spin = 0; spin < 3000; spin++) {
        if (callback_step(callbacks, 1) != 0) return -1;
        char chunk[2048];
        ssize_t n = recv(fd, chunk, sizeof chunk, 0);
        if (n > 0) buf_append(response, chunk, (size_t)n);
        else if (n == 0) return 0;
        else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    }
    return -1;
}

static int exchange(cursor_callbacks *callbacks, const char *path, const char *token,
                    const char *body, buf_t *response) {
    int fd = open_client(callbacks);
    if (fd < 0) return -1;
    int rc = send_request(callbacks, fd, path, token, body);
    if (rc == 0) rc = collect(callbacks, fd, response);
    close(fd);
    return rc;
}

static int status_of(const buf_t *response) {
    int status = -1;
    if (response->data) (void)sscanf(response->data, "HTTP/1.1 %d", &status);
    return status;
}

static const char *body_of(const buf_t *response) {
    const char *body = response->data ? strstr(response->data, "\r\n\r\n") : NULL;
    return body ? body + 4 : "";
}

static cursor_callbacks *start_callbacks(custom_tool_registry *registry, const char *state,
                                         bool allow_sensitive) {
    cursor_callbacks_options options = {registry, state, true, true, allow_sensitive, NULL};
    char error[256];
    return cursor_callbacks_start(&options, error, sizeof error);
}

TEST cursor_callbacks_route_auth_metadata_and_tools(void) {
    char state_template[] = "/tmp/tny-callback-test.XXXXXX";
    char *state_dir = mkdtemp(state_template);
    ASSERT(state_dir != NULL);
    custom_tool_registry *registry = custom_tools_new();
    ASSERT(registry != NULL);
    tool_state sync = {TOOL_SYNC, 0, NULL, 0};
    tool_state async = {TOOL_ASYNC, 0, NULL, 0};
    tool_state sensitive = {TOOL_SYNC, 0, NULL, 0};
    ASSERT(register_test_tool(registry, "sync_echo", &sync, false) != NULL);
    ASSERT(register_test_tool(registry, "async_echo", &async, false) != NULL);
    ASSERT(register_test_tool(registry, "secret_echo", &sensitive, true) != NULL);
    cursor_callbacks *callbacks = start_callbacks(registry, state_dir, false);
    ASSERT(callbacks != NULL);
    ASSERT(cursor_callbacks_tools_started(callbacks));
    ASSERT(cursor_callbacks_store_started(callbacks));
    ASSERT_EQ(64, (int)strlen(cursor_callbacks_token(callbacks)));

    char *definitions = cursor_callbacks_tool_definitions(
        callbacks, "{\"sync_echo\":{\"outputSchema\":{\"type\":\"object\"}}}");
    ASSERT(definitions != NULL);
    ASSERT(strstr(definitions, "\"description\":\"Callback fixture tool\"") != NULL);
    ASSERT(strstr(definitions, "\"inputSchema\":{") != NULL);
    ASSERT(strstr(definitions, "\"outputSchema\":{\"type\":\"object\"}") != NULL);
    free(definitions);

    const char *tool_path = "/sdk.v1.SdkCustomToolCallbackService/CallCustomTool";
    buf_t response;
    buf_init(&response);
    ASSERT_EQ(0, exchange(callbacks, tool_path, "wrong-token",
                          "{\"toolName\":\"sync_echo\",\"args\":{}}", &response));
    ASSERT_EQ(401, status_of(&response));
    buf_clear(&response);
    int no_version_fd = open_client(callbacks);
    ASSERT(no_version_fd >= 0);
    buf_t no_version;
    buf_init(&no_version);
    buf_appendf(&no_version,
                "POST %s HTTP/1.1\r\nAuthorization: Bearer %s\r\nContent-Length: 2\r\n\r\n{}",
                tool_path, cursor_callbacks_token(callbacks));
    ASSERT_EQ((int)no_version.len, (int)send(no_version_fd, no_version.data, no_version.len, 0));
    buf_free(&no_version);
    ASSERT_EQ(0, collect(callbacks, no_version_fd, &response));
    ASSERT_EQ(400, status_of(&response));
    close(no_version_fd);
    buf_clear(&response);
    ASSERT_EQ(0, exchange(callbacks, tool_path, cursor_callbacks_token(callbacks),
                          "{\"toolName\":\"missing\",\"args\":{}}", &response));
    ASSERT_EQ(404, status_of(&response));
    buf_clear(&response);
    ASSERT_EQ(0, exchange(callbacks, tool_path, cursor_callbacks_token(callbacks),
                          "{\"toolName\":\"sync_echo\",\"args\":\"not-an-object\"}", &response));
    ASSERT_EQ(400, status_of(&response));
    buf_clear(&response);
    ASSERT_EQ(0, exchange(callbacks, tool_path, cursor_callbacks_token(callbacks),
                          "{\"toolName\":\"sync_echo\",\"args\":{\"text\":\"hi\"}}", &response));
    ASSERT_EQ(200, status_of(&response));
    ASSERT(strstr(body_of(&response),
                  "\"content\":[{\"type\":\"text\",\"text\":\"plain text result\"}]") != NULL);
    ASSERT_EQ(1, sync.calls);
    buf_clear(&response);
    ASSERT_EQ(0, exchange(callbacks, tool_path, cursor_callbacks_token(callbacks),
                          "{\"toolName\":\"secret_echo\",\"args\":{}}", &response));
    ASSERT_EQ(403, status_of(&response));
    ASSERT_EQ(0, sensitive.calls);

    int fd = open_client(callbacks);
    ASSERT(fd >= 0);
    ASSERT_EQ(0, send_request(callbacks, fd, tool_path, cursor_callbacks_token(callbacks),
                              "{\"toolName\":\"async_echo\",\"args\":{}}"));
    for (int i = 0; i < 100 && async.calls == 0; i++) ASSERT_EQ(0, callback_step(callbacks, 1));
    ASSERT_EQ(1, async.calls);
    char probe;
    ASSERT_EQ(-1, (int)recv(fd, &probe, 1, 0));
    ASSERT(errno == EAGAIN || errno == EWOULDBLOCK);
    tny_tool_result_v1 result = {0};
    result.abi_version = TNY_TOOL_RESULT_ABI_VERSION;
    result.struct_size = sizeof result;
    result.data = bytes("{\"answer\":42}");
    ASSERT_EQ(TNY_STATUS_OK, custom_tool_complete(async.call, async.generation, &result));
    buf_clear(&response);
    ASSERT_EQ(0, collect(callbacks, fd, &response));
    ASSERT_EQ(200, status_of(&response));
    ASSERT(strstr(body_of(&response), "{\"result\":{\"answer\":42}}") != NULL);
    tny_tool_call_release(async.call); /* host's retained async reference */
    close(fd);

    /* A disconnected bridge cancels the deferred consumer. A host completion
     * racing after cancellation is rejected and can safely release its ref. */
    fd = open_client(callbacks);
    ASSERT(fd >= 0);
    ASSERT_EQ(0, send_request(callbacks, fd, tool_path, cursor_callbacks_token(callbacks),
                              "{\"toolName\":\"async_echo\",\"args\":{}}"));
    for (int i = 0; i < 100 && async.calls < 2; i++) ASSERT_EQ(0, callback_step(callbacks, 1));
    ASSERT_EQ(2, async.calls);
    close(fd);
    for (int i = 0; i < 20; i++) ASSERT_EQ(0, callback_step(callbacks, 1));
    ASSERT_EQ(TNY_STATUS_BAD_STATE, custom_tool_complete(async.call, async.generation, &result));
    tny_tool_call_release(async.call);

    cursor_callbacks_destroy(&callbacks);
    custom_tools_free(registry);
    buf_free(&response);
    PASS();
}

static void store_exchange(cursor_callbacks *callbacks, const char *body, buf_t *response) {
    buf_clear(response);
    (void)exchange(callbacks, "/sdk.v1.SdkStoreCallbackService/CallStore",
                   cursor_callbacks_token(callbacks), body, response);
}

TEST cursor_callback_store_persists_bare_records_blobs_and_events(void) {
    char state_template[] = "/tmp/tny-store-test.XXXXXX";
    char *state_dir = mkdtemp(state_template);
    ASSERT(state_dir != NULL);
    custom_tool_registry *registry = custom_tools_new();
    cursor_callbacks *callbacks = start_callbacks(registry, state_dir, true);
    ASSERT(callbacks != NULL);
    buf_t response;
    buf_init(&response);
    store_exchange(
        callbacks,
        "{\"substore\":\"agents\",\"method\":\"create\",\"input\":{\"agent\":{\"agentId\":\"a/../"
        "1\",\"cwd\":\"/tmp/w\",\"status\":\"idle\",\"createdAt\":1,\"updatedAt\":1}}}",
        &response);
    ASSERT_EQ(200, status_of(&response));
    ASSERT(strstr(body_of(&response), "\"output\":{\"agentId\":\"a/../1\"") != NULL);
    store_exchange(
        callbacks,
        "{\"substore\":\"runs\",\"method\":\"create\",\"input\":{\"run\":{\"agentId\":\"a/../"
        "1\",\"runId\":\"run-1\",\"turnNumber\":1,\"status\":\"running\",\"createdAt\":1,"
        "\"updatedAt\":1}}}",
        &response);
    ASSERT_EQ(200, status_of(&response));
    store_exchange(callbacks,
                   "{\"substore\":\"checkpoints\",\"method\":\"create\",\"input\":{\"agentId\":\"a/"
                   "../1\",\"blobId\":\"blob/1\",\"data\":\"AAEC/w==\"}}",
                   &response);
    ASSERT_EQ(200, status_of(&response));
    ASSERT_STR_EQ("{}", body_of(&response));
    const char *append =
        "{\"substore\":\"runEvents\",\"method\":\"append\",\"input\":{\"runId\":\"run-1\","
        "\"eventType\":\"text\",\"payload\":{\"text\":\"hello\"},\"idempotencyKey\":\"same\"}}";
    store_exchange(callbacks, append, &response);
    ASSERT_EQ(200, status_of(&response));
    ASSERT(strstr(body_of(&response), "\"seq\":1") != NULL);
    store_exchange(callbacks, append, &response);
    ASSERT(strstr(body_of(&response), "\"seq\":1") != NULL);

    char *first_token = xstrdup(cursor_callbacks_token(callbacks));
    ASSERT(first_token != NULL);
    cursor_callbacks_destroy(&callbacks);
    callbacks = start_callbacks(registry, state_dir, true);
    ASSERT(callbacks != NULL);
    ASSERT(strcmp(first_token, cursor_callbacks_token(callbacks)) != 0);
    free(first_token);
    store_exchange(
        callbacks,
        "{\"substore\":\"agents\",\"method\":\"get\",\"input\":{\"agentId\":\"a/../1\"}}",
        &response);
    ASSERT_EQ(200, status_of(&response));
    ASSERT(strstr(body_of(&response), "\"agentId\":\"a/../1\"") != NULL);
    store_exchange(
        callbacks,
        "{\"substore\":\"agents\",\"method\":\"update\",\"input\":{\"agent\":{\"agentId\":\"a/../"
        "1\",\"cwd\":\"/tmp/w\",\"status\":\"archived\",\"createdAt\":1,\"updatedAt\":2}}}",
        &response);
    ASSERT_EQ(200, status_of(&response));
    ASSERT(strstr(body_of(&response), "\"status\":\"archived\"") != NULL);
    store_exchange(callbacks,
                   "{\"substore\":\"agents\",\"method\":\"list\",\"input\":{\"filter\":{"
                   "\"cwd\":\"/tmp/w\",\"limit\":10}}}",
                   &response);
    ASSERT(strstr(body_of(&response), "\"items\":[{\"agentId\":\"a/../1\"") != NULL);
    store_exchange(callbacks,
                   "{\"substore\":\"runs\",\"method\":\"get\",\"input\":{\"agentId\":\"a/../"
                   "1\",\"runId\":\"run-1\"}}",
                   &response);
    ASSERT(strstr(body_of(&response), "\"runId\":\"run-1\"") != NULL);
    store_exchange(
        callbacks,
        "{\"substore\":\"runs\",\"method\":\"update\",\"input\":{\"run\":{\"agentId\":\"a/../"
        "1\",\"runId\":\"run-1\",\"turnNumber\":1,\"status\":\"finished\",\"createdAt\":1,"
        "\"updatedAt\":2}}}",
        &response);
    ASSERT_EQ(200, status_of(&response));
    store_exchange(callbacks,
                   "{\"substore\":\"runs\",\"method\":\"list\",\"input\":{\"filter\":{"
                   "\"agentIds\":[\"a/../1\"]}}}",
                   &response);
    ASSERT(strstr(body_of(&response), "\"status\":\"finished\"") != NULL);
    store_exchange(callbacks,
                   "{\"substore\":\"checkpoints\",\"method\":\"get\",\"input\":{\"agentId\":\"a/../"
                   "1\",\"blobId\":\"blob/1\"}}",
                   &response);
    ASSERT(strstr(body_of(&response), "\"found\":true") != NULL);
    ASSERT(strstr(body_of(&response), "\"data\":\"AAEC/w==\"") != NULL);
    store_exchange(callbacks,
                   "{\"substore\":\"checkpoints\",\"method\":\"update\",\"input\":{"
                   "\"agentId\":\"a/../1\",\"blobId\":\"blob/1\",\"data\":\"AQID\"}}",
                   &response);
    ASSERT_EQ(200, status_of(&response));
    store_exchange(callbacks,
                   "{\"substore\":\"checkpoints\",\"method\":\"list\",\"input\":{\"filter\":{"
                   "\"agentIds\":[\"a/../1\"]}}}",
                   &response);
    ASSERT(strstr(body_of(&response), "\"items\":[\"blob/1\"]") != NULL);
    store_exchange(callbacks,
                   "{\"substore\":\"runEvents\",\"method\":\"list\",\"input\":{\"runId\":\"run-1\","
                   "\"afterOffset\":\"0\",\"limit\":10}}",
                   &response);
    ASSERT(strstr(body_of(&response), "\"items\":[{") != NULL);
    ASSERT(strstr(body_of(&response), "\"offset\":\"1\"") != NULL);
    store_exchange(callbacks,
                   "{\"substore\":\"runEvents\",\"method\":\"delete\",\"input\":{\"filter\":{"
                   "\"runIds\":[\"run-1\"]}}}",
                   &response);
    ASSERT_STR_EQ("{}", body_of(&response));
    store_exchange(callbacks,
                   "{\"substore\":\"runEvents\",\"method\":\"list\",\"input\":{\"runId\":"
                   "\"run-1\"}}",
                   &response);
    ASSERT(strstr(body_of(&response), "\"items\":[]") != NULL);
    store_exchange(callbacks,
                   "{\"substore\":\"checkpoints\",\"method\":\"delete\",\"input\":{\"filter\":{"
                   "\"blobIds\":[\"blob/1\"]}}}",
                   &response);
    ASSERT_STR_EQ("{}", body_of(&response));
    store_exchange(callbacks,
                   "{\"substore\":\"checkpoints\",\"method\":\"get\",\"input\":{\"agentId\":\"a/../"
                   "1\",\"blobId\":\"blob/1\"}}",
                   &response);
    ASSERT(strstr(body_of(&response), "\"output\":{\"found\":false}") != NULL);
    store_exchange(callbacks,
                   "{\"substore\":\"runs\",\"method\":\"delete\",\"input\":{\"filter\":{"
                   "\"runIds\":[\"run-1\"]}}}",
                   &response);
    ASSERT_STR_EQ("{}", body_of(&response));
    store_exchange(callbacks,
                   "{\"substore\":\"agents\",\"method\":\"delete\",\"input\":{\"filter\":{"
                   "\"agentIds\":[\"a/../1\"]}}}",
                   &response);
    ASSERT_STR_EQ("{}", body_of(&response));
    store_exchange(callbacks,
                   "{\"substore\":\"agents\",\"method\":\"get\",\"input\":{\"agentId\":\"a/../"
                   "1\"}}",
                   &response);
    ASSERT_STR_EQ("{}", body_of(&response));

    char *store_root = path_join(state_dir, "cursor-sdk-store/agents");
    DIR *scan = opendir(store_root);
    ASSERT(scan != NULL);
    struct dirent *entry;
    while ((entry = readdir(scan)))
        if (str_ends(entry->d_name, ".json")) ASSERT(strstr(entry->d_name, "a/../1") == NULL);
    closedir(scan);
    free(store_root);
    cursor_callbacks_destroy(&callbacks);
    custom_tools_free(registry);
    buf_free(&response);
    PASS();
}

TEST blocking_unary_pump_serves_store_and_fails_tools_closed(void) {
    char state_template[] = "/tmp/tny-callback-pump-test.XXXXXX";
    char *state_dir = mkdtemp(state_template);
    ASSERT(state_dir != NULL);
    custom_tool_registry *registry = custom_tools_new();
    ASSERT(registry != NULL);
    tool_state sync = {TOOL_SYNC, 0, NULL, 0};
    ASSERT(register_test_tool(registry, "sync_echo", &sync, false) != NULL);
    cursor_callbacks *callbacks = start_callbacks(registry, state_dir, true);
    ASSERT(callbacks != NULL);
    char error[256];
    ASSERT_EQ(0, cursor_callbacks_blocking_begin(callbacks, error, sizeof error));

    /* This exchange models CreateAgent waiting for a bridge which, before it
     * can answer, synchronously calls the host store. pollfds() intentionally
     * returns no owner descriptors while the bounded pump owns the server. */
    struct pollfd owner_fds[CURSOR_CALLBACK_POLLFD_CAPACITY];
    ASSERT_EQ(0, cursor_callbacks_pollfds(callbacks, owner_fds, CURSOR_CALLBACK_POLLFD_CAPACITY));
    buf_t response;
    buf_init(&response);
    ASSERT_EQ(0, exchange(callbacks, "/sdk.v1.SdkStoreCallbackService/CallStore",
                          cursor_callbacks_token(callbacks),
                          "{\"substore\":\"agents\",\"method\":\"create\",\"input\":{\"agent\":{"
                          "\"agentId\":\"pump-agent\",\"cwd\":\"/tmp/w\",\"status\":\"idle\","
                          "\"createdAt\":1,\"updatedAt\":1}}}",
                          &response));
    ASSERT_EQ(200, status_of(&response));
    ASSERT(strstr(body_of(&response), "\"agentId\":\"pump-agent\"") != NULL);

    buf_clear(&response);
    ASSERT_EQ(0, exchange(callbacks, "/sdk.v1.SdkCustomToolCallbackService/CallCustomTool",
                          cursor_callbacks_token(callbacks),
                          "{\"toolName\":\"sync_echo\",\"args\":{}}", &response));
    ASSERT_EQ(503, status_of(&response));
    ASSERT_EQ(0, sync.calls);
    cursor_callbacks_blocking_end(callbacks);

    buf_clear(&response);
    ASSERT_EQ(0, exchange(callbacks, "/sdk.v1.SdkCustomToolCallbackService/CallCustomTool",
                          cursor_callbacks_token(callbacks),
                          "{\"toolName\":\"sync_echo\",\"args\":{}}", &response));
    ASSERT_EQ(200, status_of(&response));
    ASSERT_EQ(1, sync.calls);
    cursor_callbacks_destroy(&callbacks);
    custom_tools_free(registry);
    buf_free(&response);
    PASS();
}

TEST pending_async_tool_coexists_with_blocking_store_pump(void) {
    char state_template[] = "/tmp/tny-callback-pending-pump-test.XXXXXX";
    char *state_dir = mkdtemp(state_template);
    ASSERT(state_dir != NULL);
    custom_tool_registry *registry = custom_tools_new();
    ASSERT(registry != NULL);
    tool_state async = {TOOL_ASYNC, 0, NULL, 0};
    ASSERT(register_test_tool(registry, "async_echo", &async, false) != NULL);
    cursor_callbacks *callbacks = start_callbacks(registry, state_dir, true);
    ASSERT(callbacks != NULL);

    const char *tool_path = "/sdk.v1.SdkCustomToolCallbackService/CallCustomTool";
    int deferred_fd = open_client(callbacks);
    ASSERT(deferred_fd >= 0);
    ASSERT_EQ(0, send_request(callbacks, deferred_fd, tool_path, cursor_callbacks_token(callbacks),
                              "{\"toolName\":\"async_echo\",\"args\":{}}"));
    for (int i = 0; i < 100 && async.calls == 0; i++) ASSERT_EQ(0, callback_step(callbacks, 1));
    ASSERT_EQ(1, async.calls);
    char probe;
    ASSERT_EQ(-1, (int)recv(deferred_fd, &probe, 1, 0));
    ASSERT(errno == EAGAIN || errno == EWOULDBLOCK);

    char error[256];
    ASSERT_EQ(0, cursor_callbacks_blocking_begin(callbacks, error, sizeof error));
    buf_t response;
    buf_init(&response);
    ASSERT_EQ(0, exchange(callbacks, "/sdk.v1.SdkStoreCallbackService/CallStore",
                          cursor_callbacks_token(callbacks),
                          "{\"substore\":\"agents\",\"method\":\"create\",\"input\":{\"agent\":{"
                          "\"agentId\":\"cancel-agent\",\"cwd\":\"/tmp/w\",\"status\":\"running\","
                          "\"createdAt\":1,\"updatedAt\":1}}}",
                          &response));
    ASSERT_EQ(200, status_of(&response));
    ASSERT(strstr(body_of(&response), "\"agentId\":\"cancel-agent\"") != NULL);

    buf_clear(&response);
    ASSERT_EQ(0, exchange(callbacks, tool_path, cursor_callbacks_token(callbacks),
                          "{\"toolName\":\"async_echo\",\"args\":{}}", &response));
    ASSERT_EQ(503, status_of(&response));
    ASSERT_EQ(1, async.calls);
    cursor_callbacks_blocking_end(callbacks);

    tny_tool_result_v1 result = {0};
    result.abi_version = TNY_TOOL_RESULT_ABI_VERSION;
    result.struct_size = sizeof result;
    result.data = bytes("{\"retired\":true}");
    ASSERT_EQ(TNY_STATUS_OK, custom_tool_complete(async.call, async.generation, &result));
    buf_clear(&response);
    ASSERT_EQ(0, collect(callbacks, deferred_fd, &response));
    ASSERT_EQ(200, status_of(&response));
    ASSERT(strstr(body_of(&response), "{\"result\":{\"retired\":true}}") != NULL);
    tny_tool_call_release(async.call);
    close(deferred_fd);
    cursor_callbacks_destroy(&callbacks);
    custom_tools_free(registry);
    buf_free(&response);
    PASS();
}

TEST blocking_begin_rejects_null_and_end_restores_owner_tool_dispatch(void) {
    char error[256] = "unchanged";
    ASSERT_EQ(-1, cursor_callbacks_blocking_begin(NULL, error, sizeof error));
    ASSERT_STR_EQ("callback server is not ready for a blocking bridge RPC", error);

    char state_template[] = "/tmp/tny-callback-begin-contract-test.XXXXXX";
    char *state_dir = mkdtemp(state_template);
    ASSERT(state_dir != NULL);
    custom_tool_registry *registry = custom_tools_new();
    ASSERT(registry != NULL);
    tool_state sync = {TOOL_SYNC, 0, NULL, 0};
    ASSERT(register_test_tool(registry, "sync_echo", &sync, false) != NULL);
    cursor_callbacks *callbacks = start_callbacks(registry, state_dir, true);
    ASSERT(callbacks != NULL);

    ASSERT_EQ(0, cursor_callbacks_blocking_begin(callbacks, error, sizeof error));
    cursor_callbacks_blocking_end(callbacks);
    cursor_callbacks_blocking_end(callbacks); /* idempotent after ownership returns */

    buf_t response;
    buf_init(&response);
    ASSERT_EQ(0, exchange(callbacks, "/sdk.v1.SdkCustomToolCallbackService/CallCustomTool",
                          cursor_callbacks_token(callbacks),
                          "{\"toolName\":\"sync_echo\",\"args\":{}}", &response));
    ASSERT_EQ(200, status_of(&response));
    ASSERT_EQ(1, sync.calls);

    /* A second lend proves both mode and thread-running state were reset. */
    ASSERT_EQ(0, cursor_callbacks_blocking_begin(callbacks, error, sizeof error));
    cursor_callbacks_blocking_end(callbacks);
    buf_clear(&response);
    ASSERT_EQ(0, exchange(callbacks, "/sdk.v1.SdkCustomToolCallbackService/CallCustomTool",
                          cursor_callbacks_token(callbacks),
                          "{\"toolName\":\"sync_echo\",\"args\":{}}", &response));
    ASSERT_EQ(200, status_of(&response));
    ASSERT_EQ(2, sync.calls);

    cursor_callbacks_destroy(&callbacks);
    custom_tools_free(registry);
    buf_free(&response);
    PASS();
}

TEST blocking_begin_thread_failure_restores_mode_and_reports_error(void) {
    char state_template[] = "/tmp/tny-callback-thread-failure-test.XXXXXX";
    char *state_dir = mkdtemp(state_template);
    ASSERT(state_dir != NULL);
    custom_tool_registry *registry = custom_tools_new();
    ASSERT(registry != NULL);
    tool_state sync = {TOOL_SYNC, 0, NULL, 0};
    ASSERT(register_test_tool(registry, "sync_echo", &sync, false) != NULL);
    cursor_callbacks_options options = {
        registry, state_dir, true, true, true, reject_thread_create,
    };
    char error[256];
    cursor_callbacks *callbacks = cursor_callbacks_start(&options, error, sizeof error);
    ASSERT(callbacks != NULL);

    memset(error, 0, sizeof error);
    ASSERT_EQ(-1, cursor_callbacks_blocking_begin(callbacks, error, sizeof error));
    ASSERT_STR_EQ("could not start callback pump thread", error);

    /* Failure must restore owner mode. If blocking_mode stayed true, this
     * ordinary owner-thread tool request would fail closed with HTTP 503. */
    buf_t response;
    buf_init(&response);
    ASSERT_EQ(0, exchange(callbacks, "/sdk.v1.SdkCustomToolCallbackService/CallCustomTool",
                          cursor_callbacks_token(callbacks),
                          "{\"toolName\":\"sync_echo\",\"args\":{}}", &response));
    ASSERT_EQ(200, status_of(&response));
    ASSERT_EQ(1, sync.calls);

    /* The exact failure remains repeatable and never leaves pump_running set. */
    memset(error, 0, sizeof error);
    ASSERT_EQ(-1, cursor_callbacks_blocking_begin(callbacks, error, sizeof error));
    ASSERT_STR_EQ("could not start callback pump thread", error);
    cursor_callbacks_blocking_end(callbacks);

    cursor_callbacks_destroy(&callbacks);
    custom_tools_free(registry);
    buf_free(&response);
    PASS();
}

SUITE(cursor_callbacks_suite) {
    RUN_TEST(cursor_callbacks_route_auth_metadata_and_tools);
    RUN_TEST(cursor_callback_store_persists_bare_records_blobs_and_events);
    RUN_TEST(blocking_unary_pump_serves_store_and_fails_tools_closed);
    RUN_TEST(pending_async_tool_coexists_with_blocking_store_pump);
    RUN_TEST(blocking_begin_rejects_null_and_end_restores_owner_tool_dispatch);
    RUN_TEST(blocking_begin_thread_failure_restores_mode_and_reports_error);
}

#ifdef CURSOR_CALLBACKS_STANDALONE
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(cursor_callbacks_suite);
    GREATEST_MAIN_END();
}
#endif
