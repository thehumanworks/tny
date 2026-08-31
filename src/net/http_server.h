/* http_server.h — bounded loopback-only HTTP/1.1 POST callback server.
 *
 * The server is pull-driven: add its descriptors to the application's poll
 * set with http_server_pollfds(), then pass the completed set to
 * http_server_dispatch().  Handler input and response storage are borrowed
 * only for the duration of the callback. */
#ifndef TNY_HTTP_SERVER_H
#define TNY_HTTP_SERVER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <poll.h>

#define HTTP_SERVER_MAX_CONNECTIONS    8
#define HTTP_SERVER_POLLFD_CAPACITY    (HTTP_SERVER_MAX_CONNECTIONS + 1)
#define HTTP_SERVER_MAX_HEADER_BYTES   (16u * 1024u)
#define HTTP_SERVER_MAX_BODY_BYTES     (1024u * 1024u)
#define HTTP_SERVER_MAX_RESPONSE_BYTES (HTTP_SERVER_MAX_BODY_BYTES + 4096u)
#define HTTP_SERVER_MAX_PATH_BYTES     2048u

typedef struct http_server http_server;

typedef struct {
    int status;               /* 200..599 */
    const char *content_type; /* NULL selects application/json */
    const void *body;         /* may be NULL only when body_len is zero */
    size_t body_len;
    /* Stable request identity for HTTP_SERVER_POST_DEFERRED.  The handler may
     * retain this scalar (never the borrowed response pointer) and complete
     * it later with http_server_complete(). */
    uint64_t request_id;
} http_server_response;

typedef struct {
    const char *path;
    size_t path_len;
    const char *body;
    size_t body_len;
    bool connect_protocol_v1; /* exactly one `Connect-Protocol-Version: 1` */
} http_server_request;

enum {
    HTTP_SERVER_POST_ERROR = -1,
    HTTP_SERVER_POST_NOT_FOUND = 0,
    HTTP_SERVER_POST_HANDLED = 1,
    HTTP_SERVER_POST_DEFERRED = 2
};

/* Return HTTP_SERVER_POST_HANDLED after filling response,
 * HTTP_SERVER_POST_NOT_FOUND when path has no route, or
 * HTTP_SERVER_POST_ERROR when the route failed. */
typedef int (*http_server_post_cb)(const http_server_request *request,
                                   http_server_response *response, void *ud);

/* Bind 127.0.0.1 on an ephemeral port. bearer_token is the token only, without
 * the "Bearer " prefix, and must be non-empty. */
http_server *http_server_start(const char *bearer_token, http_server_post_cb post, void *ud,
                               char *errbuf, size_t errlen);

/* Borrowed URL, valid until destroy; form: http://127.0.0.1:<port>. */
const char *http_server_url(const http_server *server);
int http_server_port(const http_server *server);

/* Returns the number of descriptors written, never more than max. */
int http_server_pollfds(http_server *server, struct pollfd *fds, int max);
/* Dispatch readiness by descriptor identity. Client protocol errors are
 * answered and return zero; -1 means the listening socket failed. */
int http_server_dispatch(http_server *server, const struct pollfd *fds, int n);

/* Complete a deferred unary request from the server's event-loop thread.
 * Returns 1 on success, 0 when the peer disconnected or the request was
 * already completed, and -1 for an invalid response. */
int http_server_complete(http_server *server, uint64_t request_id,
                         const http_server_response *response);
bool http_server_request_alive(const http_server *server, uint64_t request_id);

/* stop is idempotent and retains the allocation. destroy nulls *serverp, so
 * repeated calls with the same pointer variable are safe. */
void http_server_stop(http_server *server);
void http_server_destroy(http_server **serverp);

#endif
