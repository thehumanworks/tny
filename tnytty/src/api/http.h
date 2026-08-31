/* http.h — the REST adapter (docs/http-api.md, docs/adr/0002).
 *
 * The router is a pure function from (method, path, body) to a complete
 * HTTP/1.1 response buffer, so it unit-tests without sockets; tt_http is
 * the socket server the CLI drives from its poll loop. */
#ifndef TNYTTY_HTTP_H
#define TNYTTY_HTTP_H

#include "session/session.h"
#include "util/tt.h"

#include <poll.h>
#include <stdbool.h>

typedef struct {
    tt_registry *reg;
    const char *token; /* NULL or "" = no token required */
    const char *version;
} tt_api;

/* Build the full response for an authorized request. */
void tt_api_route(tt_api *api, const char *method, const char *path, const char *body,
                  size_t body_len, tt_buf *out);
/* auth = the Authorization header value, or NULL if absent. */
bool tt_api_auth_ok(const tt_api *api, const char *auth);
void tt_api_error(tt_buf *out, int status, const char *message);
void tt_api_respond(tt_buf *out, int status, const char *content_type, const void *body,
                    size_t body_len);

typedef struct tt_http tt_http;
typedef bool (*tt_http_local_route_fn)(void *user, const char *method, const char *path,
                                       const char *body, size_t body_len, tt_buf *out);

/* Bind and listen. Non-loopback hosts require api->token (docs/adr/0002).
 * Returns NULL with a message in err. */
tt_http *tt_http_listen(tt_api *api, const char *host, int port, char *err, size_t errlen);
/* Bind the same HTTP surface on an AF_UNIX socket. The parent directory
 * must already be private; the socket is chmod 0600 and accepted peers
 * must have the server's effective uid. */
tt_http *tt_http_listen_unix(tt_api *api, const char *path, char *err, size_t errlen);
/* Add broker administration routes to a same-uid Unix listener. The callback
 * returns true only when it produced a complete response. */
void tt_http_set_local_route(tt_http *h, tt_http_local_route_fn route, void *user);
void tt_http_free(tt_http *h);

/* Poll-loop integration: fill appends this server's pollfds (listener +
 * connections) and returns how many; handle consumes the same slice of
 * revents afterwards. */
int tt_http_fill(tt_http *h, struct pollfd *fds, int max);
void tt_http_handle(tt_http *h, const struct pollfd *fds, int n);

#endif
