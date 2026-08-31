/* test_http_server.c — focused loopback callback-server tests. */
#include "greatest.h"
#include "net/http_server.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int calls;
    uint64_t deferred_id;
    char path[128];
    buf_t body;
} handler_state;

static int test_post(const http_server_request *request, http_server_response *response, void *ud) {
    handler_state *state = ud;
    state->calls++;
    const char *path = request->path;
    size_t path_len = request->path_len;
    const char *body = request->body;
    size_t body_len = request->body_len;
    size_t copy = path_len < sizeof state->path - 1 ? path_len : sizeof state->path - 1;
    memcpy(state->path, path, copy);
    state->path[copy] = 0;
    buf_clear(&state->body);
    buf_append(&state->body, body, body_len);
    if (strcmp(path, "/missing") == 0) return HTTP_SERVER_POST_NOT_FOUND;
    if (strcmp(path, "/defer") == 0) {
        state->deferred_id = response->request_id;
        return HTTP_SERVER_POST_DEFERRED;
    }
    if (strcmp(path, "/fail") == 0) return HTTP_SERVER_POST_ERROR;
    if (strcmp(path, "/invalid-response") == 0) {
        response->status = 0;
        return HTTP_SERVER_POST_HANDLED;
    }
    response->status = 201;
    response->content_type = "application/json; charset=utf-8";
    response->body = "{\"received\":true}";
    response->body_len = strlen(response->body);
    return HTTP_SERVER_POST_HANDLED;
}

static http_server *start_server(handler_state *state) {
    char error[256];
    return http_server_start("callback-secret", test_post, state, error, sizeof error);
}

static int server_step(http_server *server, int timeout_ms) {
    struct pollfd fds[HTTP_SERVER_MAX_CONNECTIONS + 1];
    int count = http_server_pollfds(server, fds, (int)(sizeof fds / sizeof fds[0]));
    int rc = tny_poll(fds, (nfds_t)count, timeout_ms);
    if (rc < 0 && errno == EINTR) return 0;
    if (rc < 0) return -1;
    return http_server_dispatch(server, fds, count);
}

static int connect_client(http_server *server) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)http_server_port(server));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        close(fd);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_piece(http_server *server, int fd, const char *bytes, size_t len) {
    size_t offset = 0;
    for (int spins = 0; offset < len && spins < 2000; spins++) {
        ssize_t written = send(fd, bytes + offset, len - offset, 0);
        if (written > 0) {
            offset += (size_t)written;
        } else if (written < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        if (server_step(server, 0) != 0) return -1;
    }
    return offset == len ? 0 : -1;
}

static int collect_response(http_server *server, int fd, buf_t *response) {
    for (int spins = 0; spins < 2000; spins++) {
        if (server_step(server, 2) != 0) return -1;
        char bytes[2048];
        ssize_t got = recv(fd, bytes, sizeof bytes, 0);
        if (got > 0) {
            buf_append(response, bytes, (size_t)got);
            continue;
        }
        if (got == 0) return 0;
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    }
    return -1;
}

static int exchange_split(http_server *server, const char *request, size_t request_len,
                          size_t split, buf_t *response) {
    int fd = connect_client(server);
    if (fd < 0) return -1;
    int rc = 0;
    if (send_piece(server, fd, request, split) != 0 ||
        send_piece(server, fd, request + split, request_len - split) != 0 ||
        collect_response(server, fd, response) != 0)
        rc = -1;
    close(fd);
    return rc;
}

static int response_status(const buf_t *response) {
    int status = 0;
    return response->data && sscanf(response->data, "HTTP/1.1 %d", &status) == 1 ? status : -1;
}

TEST http_server_binds_ephemeral_ipv4_loopback(void) {
    handler_state state = {0};
    buf_init(&state.body);
    http_server *server = start_server(&state);
    ASSERT(server != NULL);
    ASSERT(http_server_port(server) > 0);
    ASSERT(strstr(http_server_url(server), "http://127.0.0.1:") == http_server_url(server));

    struct pollfd fd = {-1, 0, 0};
    ASSERT_EQ(1, http_server_pollfds(server, &fd, 1));
    struct sockaddr_in address = {0};
    socklen_t address_len = sizeof address;
    ASSERT_EQ(0, getsockname(fd.fd, (struct sockaddr *)&address, &address_len));
    ASSERT_EQ(AF_INET, address.sin_family);
    ASSERT_EQ((long)htonl(INADDR_LOOPBACK), (long)address.sin_addr.s_addr);
    ASSERT_EQ(http_server_port(server), (int)ntohs(address.sin_port));

    http_server_destroy(&server);
    buf_free(&state.body);
    PASS();
}

TEST content_length_survives_every_split_boundary(void) {
    const char request[] = "POST /content HTTP/1.1\r\n"
                           "Host: 127.0.0.1\r\n"
                           "Authorization: Bearer callback-secret\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: 17\r\n\r\n"
                           "{\"hello\":\"world\"}";
    handler_state state = {0};
    buf_init(&state.body);
    http_server *server = start_server(&state);
    ASSERT(server != NULL);
    for (size_t split = 1; split < sizeof request - 1; split++) {
        buf_t response;
        buf_init(&response);
        ASSERT_EQm("content-length request split failed", 0,
                   exchange_split(server, request, sizeof request - 1, split, &response));
        ASSERT_EQm("content-length status", 201, response_status(&response));
        ASSERT_STR_EQ("/content", state.path);
        ASSERT_STR_EQ("{\"hello\":\"world\"}", state.body.data);
        buf_free(&response);
    }
    ASSERT_EQ((int)(sizeof request - 2), state.calls);
    http_server_destroy(&server);
    buf_free(&state.body);
    PASS();
}

TEST chunked_survives_every_split_boundary(void) {
    const char request[] = "POST /chunked HTTP/1.1\r\n"
                           "Authorization: Bearer callback-secret\r\n"
                           "Transfer-Encoding: chunked\r\n\r\n"
                           "5\r\n{\"hel\r\n"
                           "C\r\nlo\":\"world\"}\r\n"
                           "0\r\nX-Trailer: accepted\r\n\r\n";
    handler_state state = {0};
    buf_init(&state.body);
    http_server *server = start_server(&state);
    ASSERT(server != NULL);
    for (size_t split = 1; split < sizeof request - 1; split++) {
        buf_t response;
        buf_init(&response);
        ASSERT_EQm("chunked request split failed", 0,
                   exchange_split(server, request, sizeof request - 1, split, &response));
        ASSERT_EQm("chunked status", 201, response_status(&response));
        ASSERT_STR_EQ("/chunked", state.path);
        ASSERT_STR_EQ("{\"hello\":\"world\"}", state.body.data);
        buf_free(&response);
    }
    ASSERT_EQ((int)(sizeof request - 2), state.calls);
    http_server_destroy(&server);
    buf_free(&state.body);
    PASS();
}

static int one_status(http_server *server, const char *request) {
    buf_t response;
    buf_init(&response);
    int rc = exchange_split(server, request, strlen(request), strlen(request), &response);
    int status = rc == 0 ? response_status(&response) : -1;
    buf_free(&response);
    return status;
}

TEST http_server_returns_bounded_protocol_errors(void) {
    handler_state state = {0};
    buf_init(&state.body);
    http_server *server = start_server(&state);
    ASSERT(server != NULL);

    ASSERT_EQ(400, one_status(server, "not-http\r\n\r\n"));
    ASSERT_EQ(401, one_status(server, "POST /ok HTTP/1.1\r\nContent-Length: 0\r\n\r\n"));
    ASSERT_EQ(401,
              one_status(server, "POST /ok HTTP/1.1\r\nAuthorization: Bearer callback-secretx\r\n"
                                 "Content-Length: 0\r\n\r\n"));
    ASSERT_EQ(405, one_status(server,
                              "GET /ok HTTP/1.1\r\nAuthorization: Bearer callback-secret\r\n\r\n"));
    ASSERT_EQ(400,
              one_status(server, "POST /ok HTTP/1.1\r\nAuthorization: Bearer callback-secret\r\n"
                                 "Content-Length: nope\r\n\r\n"));
    ASSERT_EQ(400,
              one_status(server, "POST /ok HTTP/1.1\r\nAuthorization: Bearer callback-secret\r\n"
                                 "Content-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n"));
    ASSERT_EQ(404, one_status(server,
                              "POST /missing HTTP/1.1\r\nAuthorization: Bearer callback-secret\r\n"
                              "Content-Length: 0\r\n\r\n"));
    ASSERT_EQ(500,
              one_status(server, "POST /fail HTTP/1.1\r\nAuthorization: Bearer callback-secret\r\n"
                                 "Content-Length: 0\r\n\r\n"));
    ASSERT_EQ(500,
              one_status(server,
                         "POST /invalid-response HTTP/1.1\r\n"
                         "Authorization: Bearer callback-secret\r\nContent-Length: 0\r\n\r\n"));

    char oversized_length[256];
    snprintf(oversized_length, sizeof oversized_length,
             "POST /ok HTTP/1.1\r\nAuthorization: Bearer callback-secret\r\n"
             "Content-Length: %u\r\n\r\n",
             HTTP_SERVER_MAX_BODY_BYTES + 1u);
    ASSERT_EQ(413, one_status(server, oversized_length));

    size_t large_len = HTTP_SERVER_MAX_HEADER_BYTES + 256;
    char *large = malloc(large_len + 1);
    ASSERT(large != NULL);
    int prefix = snprintf(large, large_len + 1,
                          "POST /ok HTTP/1.1\r\nAuthorization: Bearer callback-secret\r\nX: ");
    ASSERT(prefix > 0);
    memset(large + prefix, 'a', large_len - (size_t)prefix - 4);
    memcpy(large + large_len - 4, "\r\n\r\n", 4);
    large[large_len] = 0;
    ASSERT_EQ(413, one_status(server, large));
    free(large);

    /* Only the three authenticated, routed zero-body requests reached the callback. */
    ASSERT_EQ(3, state.calls);
    http_server_destroy(&server);
    buf_free(&state.body);
    PASS();
}

TEST teardown_is_idempotent_and_does_not_change_sigpipe(void) {
    struct sigaction before = {0}, after = {0};
    ASSERT_EQ(0, sigaction(SIGPIPE, NULL, &before));
    handler_state state = {0};
    buf_init(&state.body);
    http_server *server = start_server(&state);
    ASSERT(server != NULL);

    int fd = connect_client(server);
    ASSERT(fd >= 0);
    const char request[] = "POST /ok HTTP/1.1\r\nAuthorization: Bearer callback-secret\r\n"
                           "Content-Length: 0\r\n\r\n";
    ASSERT_EQ(0, send_piece(server, fd, request, sizeof request - 1));
    struct linger reset = {1, 0};
    ASSERT_EQ(0, setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof reset));
    close(fd);
    for (int i = 0; i < 10; i++) ASSERT_EQ(0, server_step(server, 1));

    ASSERT_EQ(0, sigaction(SIGPIPE, NULL, &after));
    ASSERT_EQ((long)before.sa_handler, (long)after.sa_handler);
    http_server_stop(server);
    http_server_stop(server);
    ASSERT_EQ(-1, http_server_port(server));
    ASSERT(http_server_url(server) == NULL);
    http_server_destroy(&server);
    http_server_destroy(&server);
    ASSERT(server == NULL);
    buf_free(&state.body);
    PASS();
}

TEST deferred_response_completes_later_and_detects_disconnect(void) {
    handler_state state = {0};
    buf_init(&state.body);
    http_server *server = start_server(&state);
    ASSERT(server != NULL);
    int fd = connect_client(server);
    ASSERT(fd >= 0);
    const char request[] = "POST /defer HTTP/1.1\r\nAuthorization: Bearer callback-secret\r\n"
                           "Content-Length: 2\r\n\r\n{}";
    ASSERT_EQ(0, send_piece(server, fd, request, sizeof request - 1));
    for (int i = 0; i < 10 && state.deferred_id == 0; i++) ASSERT_EQ(0, server_step(server, 1));
    ASSERT(state.deferred_id != 0);
    ASSERT(http_server_request_alive(server, state.deferred_id));
    char probe;
    ASSERT_EQ(-1, (int)recv(fd, &probe, 1, 0));
    ASSERT(errno == EAGAIN || errno == EWOULDBLOCK);
    const char body[] = "{\"later\":true}";
    http_server_response response = {200, "application/json", body, sizeof body - 1,
                                     state.deferred_id};
    ASSERT_EQ(1, http_server_complete(server, state.deferred_id, &response));
    buf_t wire;
    buf_init(&wire);
    ASSERT_EQ(0, collect_response(server, fd, &wire));
    ASSERT_EQ(200, response_status(&wire));
    ASSERT(strstr(wire.data, body) != NULL);
    ASSERT_FALSE(http_server_request_alive(server, state.deferred_id));
    ASSERT_EQ(0, http_server_complete(server, state.deferred_id, &response));
    close(fd);
    buf_free(&wire);

    fd = connect_client(server);
    ASSERT(fd >= 0);
    state.deferred_id = 0;
    ASSERT_EQ(0, send_piece(server, fd, request, sizeof request - 1));
    for (int i = 0; i < 10 && state.deferred_id == 0; i++) ASSERT_EQ(0, server_step(server, 1));
    ASSERT(state.deferred_id != 0);
    uint64_t disconnected = state.deferred_id;
    close(fd);
    for (int i = 0; i < 10 && http_server_request_alive(server, disconnected); i++)
        ASSERT_EQ(0, server_step(server, 1));
    ASSERT_FALSE(http_server_request_alive(server, disconnected));
    ASSERT_EQ(0, http_server_complete(server, disconnected, &response));
    http_server_destroy(&server);
    buf_free(&state.body);
    PASS();
}

SUITE(http_server_suite) {
    RUN_TEST(http_server_binds_ephemeral_ipv4_loopback);
    RUN_TEST(content_length_survives_every_split_boundary);
    RUN_TEST(chunked_survives_every_split_boundary);
    RUN_TEST(http_server_returns_bounded_protocol_errors);
    RUN_TEST(deferred_response_completes_later_and_detects_disconnect);
    RUN_TEST(teardown_is_idempotent_and_does_not_change_sigpipe);
}

#ifdef HTTP_SERVER_STANDALONE
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(http_server_suite);
    GREATEST_MAIN_END();
}
#endif
