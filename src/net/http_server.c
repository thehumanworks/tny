/* http_server.c — bounded, poll-driven loopback HTTP callback server. */
#include "net/http_server.h"

#include "picohttpparser.h"
#include "util/util.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define HTTP_SERVER_MAX_HEADERS 64
#define HTTP_SERVER_READ_BYTES  8192

typedef enum {
    HC_UNUSED = 0,
    HC_HEADERS,
    HC_BODY_LENGTH,
    HC_BODY_CHUNKED,
    HC_DEFERRED,
    HC_WRITING
} hc_state;

typedef struct {
    int fd;
    hc_state state;
    buf_t headers;
    buf_t body;
    buf_t output;
    size_t write_offset;
    size_t content_length;
    struct phr_chunked_decoder chunked;
    char path[HTTP_SERVER_MAX_PATH_BYTES + 1];
    size_t path_len;
    uint64_t request_id;
    bool connect_protocol_v1;
} http_server_conn;

struct http_server {
    int listener;
    int port;
    char url[64];
    char *bearer_token;
    size_t bearer_len;
    http_server_post_cb post;
    void *ud;
    http_server_conn conns[HTTP_SERVER_MAX_CONNECTIONS];
    uint64_t next_request_id;
};

static void set_error(char *errbuf, size_t errlen, const char *message) {
    if (!errbuf || errlen == 0) return;
    snprintf(errbuf, errlen, "%s", message);
}

/* GCC's descriptor analyzer confuses the borrowed listener passed to accept(2)
 * with the descriptor returned by accept(2) (GCC analyzer/108648).  It then
 * reports that listener at any operation on the accepted descriptor.  Keep
 * the workaround scoped to the two exact calls in those false traces; the
 * accepted descriptor is closed on every failure or transferred to conns[]. */
static int fd_add_flags(int fd, int get_op, int set_op, int flags) {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif
    int old = fcntl(fd, get_op, 0);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    return old < 0 || fcntl(fd, set_op, old | flags) < 0 ? -1 : 0;
}

static int prepare_socket(int fd) {
    if (fd_add_flags(fd, F_GETFL, F_SETFL, O_NONBLOCK) != 0 ||
        fd_add_flags(fd, F_GETFD, F_SETFD, FD_CLOEXEC) != 0)
        return -1;
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled) != 0) return -1;
#endif
    return 0;
}

static void conn_init(http_server_conn *conn) {
    memset(conn, 0, sizeof *conn);
    conn->fd = -1;
    conn->state = HC_UNUSED;
    buf_init(&conn->headers);
    buf_init(&conn->body);
    buf_init(&conn->output);
}

static void conn_close(http_server_conn *conn) {
    if (conn->fd >= 0) close(conn->fd);
    buf_free(&conn->headers);
    buf_free(&conn->body);
    buf_free(&conn->output);
    conn_init(conn);
}

static bool bytes_case_equal(const char *bytes, size_t len, const char *literal) {
    size_t literal_len = strlen(literal);
    if (len != literal_len) return false;
    for (size_t i = 0; i < len; i++) {
        if (tolower((unsigned char)bytes[i]) != tolower((unsigned char)literal[i])) return false;
    }
    return true;
}

static void trim_ows(const char **value, size_t *len) {
    while (*len && (**value == ' ' || **value == '\t')) {
        (*value)++;
        (*len)--;
    }
    while (*len && ((*value)[*len - 1] == ' ' || (*value)[*len - 1] == '\t')) (*len)--;
}

static bool parse_content_length(const char *value, size_t len, size_t *out) {
    trim_ows(&value, &len);
    if (len == 0) return false;
    size_t result = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (ch < '0' || ch > '9') return false;
        unsigned digit = (unsigned)(ch - '0');
        if (result > (SIZE_MAX - digit) / 10) return false;
        result = result * 10 + digit;
    }
    *out = result;
    return true;
}

static bool auth_matches(const http_server *server, const char *value, size_t len) {
    trim_ows(&value, &len);
    static const char prefix[] = "Bearer ";
    size_t expected_len = sizeof prefix - 1 + server->bearer_len;
    size_t length_difference = len ^ expected_len;
    unsigned char difference = 0;
    for (size_t i = 0; i < expected_len; i++) {
        unsigned char actual = i < len ? (unsigned char)value[i] : 0;
        unsigned char expected = i < sizeof prefix - 1
                                     ? (unsigned char)prefix[i]
                                     : (unsigned char)server->bearer_token[i - (sizeof prefix - 1)];
        difference |= actual ^ expected;
    }
    while (length_difference) {
        difference |= (unsigned char)length_difference;
        length_difference >>= 8;
    }
    return difference == 0;
}

static const char *reason_phrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Content";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return status < 400 ? "OK" : "Error";
    }
}

static bool content_type_valid(const char *value) {
    if (!value) return true;
    size_t len = strlen(value);
    if (len == 0 || len > 128) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (ch < 0x20 || ch > 0x7e || ch == '\r' || ch == '\n') return false;
    }
    return true;
}

static int queue_response(http_server_conn *conn, int status, const char *content_type,
                          const void *body, size_t body_len, const char *extra_header) {
    if (status < 200 || status > 599 || !content_type_valid(content_type) || (body_len && !body) ||
        body_len > HTTP_SERVER_MAX_RESPONSE_BYTES)
        return -1;
    if (!content_type) content_type = "application/json";
    buf_clear(&conn->output);
    buf_appendf(&conn->output,
                "HTTP/1.1 %d %s\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n"
                "Cache-Control: no-store\r\n",
                status, reason_phrase(status), content_type, body_len);
    if (extra_header) buf_appends(&conn->output, extra_header);
    buf_appends(&conn->output, "\r\n");
    if (body_len) buf_append(&conn->output, body, body_len);
    if (buf_oom(&conn->output)) return -1;
    conn->write_offset = 0;
    conn->state = HC_WRITING;
    return 0;
}

static void queue_static(http_server_conn *conn, int status) {
    const char *body;
    const char *extra = NULL;
    switch (status) {
    case 400: body = "{\"error\":\"bad request\"}"; break;
    case 401:
        body = "{\"error\":\"unauthorized\"}";
        extra = "WWW-Authenticate: Bearer\r\n";
        break;
    case 404: body = "{\"error\":\"not found\"}"; break;
    case 405:
        body = "{\"error\":\"method not allowed\"}";
        extra = "Allow: POST\r\n";
        break;
    case 413: body = "{\"error\":\"payload too large\"}"; break;
    default:
        status = 500;
        body = "{\"error\":\"internal server error\"}";
        break;
    }
    if (queue_response(conn, status, NULL, body, strlen(body), extra) != 0) conn_close(conn);
}

static void flush_response(http_server_conn *conn) {
    while (conn->state == HC_WRITING && conn->write_offset < conn->output.len) {
        ssize_t written = send(conn->fd, conn->output.data + conn->write_offset,
                               conn->output.len - conn->write_offset, MSG_NOSIGNAL);
        if (written > 0) {
            conn->write_offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        conn_close(conn);
        return;
    }
    if (conn->state == HC_WRITING) conn_close(conn);
}

static void invoke_post(http_server *server, http_server_conn *conn) {
    if (++server->next_request_id == 0) ++server->next_request_id;
    conn->request_id = server->next_request_id;
    http_server_response response = {200, "application/json", NULL, 0, conn->request_id};
    const char *body = conn->body.data ? conn->body.data : "";
    http_server_request request = {conn->path, conn->path_len, body, conn->body.len,
                                   conn->connect_protocol_v1};
    int route = server->post(&request, &response, server->ud);
    if (route == HTTP_SERVER_POST_NOT_FOUND) {
        queue_static(conn, 404);
        return;
    }
    if (route == HTTP_SERVER_POST_DEFERRED) {
        conn->state = HC_DEFERRED;
        return;
    }
    if (route != HTTP_SERVER_POST_HANDLED ||
        queue_response(conn, response.status, response.content_type, response.body,
                       response.body_len, NULL) != 0) {
        buf_free(&conn->output);
        buf_init(&conn->output);
        queue_static(conn, 500);
    }
}

static void consume_body(http_server *server, http_server_conn *conn, char *bytes, size_t len) {
    if (conn->state == HC_BODY_LENGTH) {
        size_t remaining = conn->content_length - conn->body.len;
        size_t take = len < remaining ? len : remaining;
        buf_append(&conn->body, bytes, take);
        if (buf_oom(&conn->body)) {
            queue_static(conn, 500);
        } else if (conn->body.len == conn->content_length) {
            invoke_post(server, conn);
        }
        return;
    }
    if (conn->state != HC_BODY_CHUNKED) return;
    size_t decoded = len;
    ssize_t rc = phr_decode_chunked(&conn->chunked, bytes, &decoded);
    if (decoded > HTTP_SERVER_MAX_BODY_BYTES - conn->body.len) {
        queue_static(conn, 413);
        return;
    }
    buf_append(&conn->body, bytes, decoded);
    if (buf_oom(&conn->body)) {
        queue_static(conn, 500);
    } else if (rc == -1) {
        queue_static(conn, 400);
    } else if (rc >= 0) {
        invoke_post(server, conn);
    }
}

static void parse_headers(http_server *server, http_server_conn *conn) {
    struct phr_header headers[HTTP_SERVER_MAX_HEADERS];
    size_t num_headers = HTTP_SERVER_MAX_HEADERS;
    const char *method = NULL;
    const char *path = NULL;
    size_t method_len = 0, path_len = 0;
    int minor_version = 0;
    int consumed = phr_parse_request(conn->headers.data, conn->headers.len, &method, &method_len,
                                     &path, &path_len, &minor_version, headers, &num_headers, 0);
    (void)minor_version;
    if (consumed == -2) {
        if (conn->headers.len > HTTP_SERVER_MAX_HEADER_BYTES) queue_static(conn, 413);
        return;
    }
    if (consumed < 0 || (size_t)consumed > HTTP_SERVER_MAX_HEADER_BYTES || path_len == 0 ||
        path_len > HTTP_SERVER_MAX_PATH_BYTES || path[0] != '/') {
        queue_static(conn,
                     consumed >= 0 && (size_t)consumed > HTTP_SERVER_MAX_HEADER_BYTES ? 413 : 400);
        return;
    }

    bool have_auth = false, auth_ok = false, have_length = false, have_transfer = false;
    bool have_connect_version = false;
    bool bad = false;
    size_t content_length = 0;
    for (size_t i = 0; i < num_headers; i++) {
        if (!headers[i].name) {
            bad = true;
            break;
        }
        if (bytes_case_equal(headers[i].name, headers[i].name_len, "Authorization")) {
            if (have_auth) bad = true;
            have_auth = true;
            auth_ok = auth_matches(server, headers[i].value, headers[i].value_len);
        } else if (bytes_case_equal(headers[i].name, headers[i].name_len, "Content-Length")) {
            if (have_length ||
                !parse_content_length(headers[i].value, headers[i].value_len, &content_length))
                bad = true;
            have_length = true;
        } else if (bytes_case_equal(headers[i].name, headers[i].name_len, "Transfer-Encoding")) {
            const char *value = headers[i].value;
            size_t value_len = headers[i].value_len;
            trim_ows(&value, &value_len);
            if (have_transfer || !bytes_case_equal(value, value_len, "chunked")) bad = true;
            have_transfer = true;
        } else if (bytes_case_equal(headers[i].name, headers[i].name_len,
                                    "Connect-Protocol-Version")) {
            const char *value = headers[i].value;
            size_t value_len = headers[i].value_len;
            trim_ows(&value, &value_len);
            if (have_connect_version) bad = true;
            have_connect_version = true;
            conn->connect_protocol_v1 = value_len == 1 && value[0] == '1';
        }
    }
    if (have_length && have_transfer) bad = true;
    if (bad) {
        queue_static(conn, 400);
        return;
    }
    if (!have_auth || !auth_ok) {
        queue_static(conn, 401);
        return;
    }
    if (method_len != 4 || memcmp(method, "POST", 4) != 0) {
        queue_static(conn, 405);
        return;
    }
    if (have_length && content_length > HTTP_SERVER_MAX_BODY_BYTES) {
        queue_static(conn, 413);
        return;
    }

    memcpy(conn->path, path, path_len);
    conn->path[path_len] = 0;
    conn->path_len = path_len;
    conn->content_length = content_length;
    size_t body_offset = (size_t)consumed;
    size_t body_bytes = conn->headers.len - body_offset;
    if (have_transfer) {
        conn->state = HC_BODY_CHUNKED;
        memset(&conn->chunked, 0, sizeof conn->chunked);
        conn->chunked.consume_trailer = 1;
    } else if (have_length && content_length != 0) {
        conn->state = HC_BODY_LENGTH;
    } else {
        invoke_post(server, conn);
    }
    if ((conn->state == HC_BODY_LENGTH || conn->state == HC_BODY_CHUNKED) && body_bytes)
        consume_body(server, conn, conn->headers.data + body_offset, body_bytes);
    buf_free(&conn->headers);
    buf_init(&conn->headers);
}

static void read_request(http_server *server, http_server_conn *conn) {
    char bytes[HTTP_SERVER_READ_BYTES];
    for (;;) {
        ssize_t got = recv(conn->fd, bytes, sizeof bytes, 0);
        if (got > 0) {
            if (conn->state == HC_HEADERS) {
                buf_append(&conn->headers, bytes, (size_t)got);
                if (buf_oom(&conn->headers)) {
                    queue_static(conn, 500);
                } else {
                    parse_headers(server, conn);
                }
            } else {
                consume_body(server, conn, bytes, (size_t)got);
            }
            if (conn->state == HC_WRITING || conn->state == HC_UNUSED) break;
            continue;
        }
        if (got == 0) {
            if (conn->state == HC_HEADERS || conn->state == HC_BODY_LENGTH ||
                conn->state == HC_BODY_CHUNKED || conn->state == HC_DEFERRED)
                queue_static(conn, 400);
            break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        conn_close(conn);
        break;
    }
    if (conn->state == HC_WRITING) flush_response(conn);
}

static short revents_for(const struct pollfd *fds, int n, int fd) {
    short events = 0;
    for (int i = 0; i < n; i++) {
        if (fds[i].fd == fd) events |= fds[i].revents;
    }
    return events;
}

static http_server_conn *free_conn(http_server *server) {
    for (size_t i = 0; i < HTTP_SERVER_MAX_CONNECTIONS; i++) {
        if (server->conns[i].state == HC_UNUSED) return &server->conns[i];
    }
    return NULL;
}

static void accept_ready(http_server *server) {
    for (;;) {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif
        int fd = accept(server->listener, NULL, NULL);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        if (fd < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (prepare_socket(fd) != 0) {
            close(fd);
            continue;
        }
        http_server_conn *conn = free_conn(server);
        if (!conn) {
            close(fd);
            continue;
        }
        conn->fd = fd;
        conn->state = HC_HEADERS;
    }
}

http_server *http_server_start(const char *bearer_token, http_server_post_cb post, void *ud,
                               char *errbuf, size_t errlen) {
    if (!bearer_token || !bearer_token[0] || !post) {
        set_error(errbuf, errlen, "callback server requires a bearer token and POST handler");
        return NULL;
    }
    http_server *server = calloc(1, sizeof *server);
    if (!server) {
        set_error(errbuf, errlen, "out of memory starting callback server");
        return NULL;
    }
    server->listener = -1;
    for (size_t i = 0; i < HTTP_SERVER_MAX_CONNECTIONS; i++) conn_init(&server->conns[i]);
    server->bearer_token = xstrdup(bearer_token);
    if (!server->bearer_token) {
        set_error(errbuf, errlen, "out of memory starting callback server");
        http_server_destroy(&server);
        return NULL;
    }
    server->bearer_len = strlen(bearer_token);
    server->post = post;
    server->ud = ud;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || prepare_socket(fd) != 0) {
        if (fd >= 0) close(fd);
        set_error(errbuf, errlen, "could not create callback server socket");
        http_server_destroy(&server);
        return NULL;
    }
    int enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof enabled);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(0);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 ||
        listen(fd, HTTP_SERVER_MAX_CONNECTIONS) != 0) {
        close(fd);
        set_error(errbuf, errlen, "could not bind callback server to loopback");
        http_server_destroy(&server);
        return NULL;
    }
    socklen_t address_len = sizeof address;
    if (getsockname(fd, (struct sockaddr *)&address, &address_len) != 0) {
        close(fd);
        set_error(errbuf, errlen, "could not read callback server address");
        http_server_destroy(&server);
        return NULL;
    }
    server->listener = fd;
    server->port = ntohs(address.sin_port);
    snprintf(server->url, sizeof server->url, "http://127.0.0.1:%d", server->port);
    return server;
}

const char *http_server_url(const http_server *server) {
    return server && server->listener >= 0 ? server->url : NULL;
}

int http_server_port(const http_server *server) {
    return server && server->listener >= 0 ? server->port : -1;
}

int http_server_pollfds(http_server *server, struct pollfd *fds, int max) {
    if (!server || !fds || max <= 0 || server->listener < 0) return 0;
    int count = 0;
    fds[count++] = (struct pollfd){server->listener, POLLIN, 0};
    for (size_t i = 0; i < HTTP_SERVER_MAX_CONNECTIONS && count < max; i++) {
        http_server_conn *conn = &server->conns[i];
        if (conn->state == HC_UNUSED) continue;
        short events = conn->state == HC_WRITING ? POLLOUT : POLLIN;
        fds[count++] = (struct pollfd){conn->fd, events, 0};
    }
    return count;
}

int http_server_dispatch(http_server *server, const struct pollfd *fds, int n) {
    if (!server || server->listener < 0) return 0;
    short listener_events = revents_for(fds, n, server->listener);
    if (listener_events & (POLLERR | POLLNVAL)) return -1;
    if (listener_events & POLLIN) accept_ready(server);
    for (size_t i = 0; i < HTTP_SERVER_MAX_CONNECTIONS; i++) {
        http_server_conn *conn = &server->conns[i];
        if (conn->state == HC_UNUSED) continue;
        short events = revents_for(fds, n, conn->fd);
        if (events & POLLIN) read_request(server, conn);
        if (conn->state != HC_UNUSED && events & POLLOUT) flush_response(conn);
        if (conn->state != HC_UNUSED && events & (POLLERR | POLLHUP | POLLNVAL)) {
            if (conn->state == HC_WRITING) flush_response(conn);
            if (conn->state != HC_UNUSED) conn_close(conn);
        }
    }
    return 0;
}

int http_server_complete(http_server *server, uint64_t request_id,
                         const http_server_response *response) {
    if (!server || !request_id || !response) return -1;
    for (size_t i = 0; i < HTTP_SERVER_MAX_CONNECTIONS; i++) {
        http_server_conn *conn = &server->conns[i];
        if (conn->state != HC_DEFERRED || conn->request_id != request_id) continue;
        if (queue_response(conn, response->status, response->content_type, response->body,
                           response->body_len, NULL) != 0)
            return -1;
        return 1;
    }
    return 0;
}

bool http_server_request_alive(const http_server *server, uint64_t request_id) {
    if (!server || !request_id) return false;
    for (size_t i = 0; i < HTTP_SERVER_MAX_CONNECTIONS; i++)
        if (server->conns[i].state == HC_DEFERRED && server->conns[i].request_id == request_id)
            return true;
    return false;
}

void http_server_stop(http_server *server) {
    if (!server) return;
    if (server->listener >= 0) close(server->listener);
    server->listener = -1;
    server->port = -1;
    server->url[0] = 0;
    for (size_t i = 0; i < HTTP_SERVER_MAX_CONNECTIONS; i++) conn_close(&server->conns[i]);
}

void http_server_destroy(http_server **serverp) {
    if (!serverp || !*serverp) return;
    http_server *server = *serverp;
    http_server_stop(server);
    secure_free(server->bearer_token);
    server->bearer_token = NULL;
    server->bearer_len = 0;
    free(server);
    *serverp = NULL;
}
