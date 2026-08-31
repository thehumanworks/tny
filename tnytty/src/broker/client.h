/* client.h — GUI-facing HTTP client for the per-user broker. */
#ifndef TNYTTY_BROKER_CLIENT_H
#define TNYTTY_BROKER_CLIENT_H

#include "broker/protocol.h"

#include <poll.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char socket_path[104];
    int pending_fd;
    tt_http_response_parser pending;
} tt_broker_client;

/* Starts the broker when needed. Call before initializing AppKit. */
int tt_broker_client_open(tt_broker_client *c, const char *socket_path, char *err, size_t errcap);
void tt_broker_client_close(tt_broker_client *c);

int tt_broker_client_create(tt_broker_client *c, char *const argv[], const char *cwd, int cols,
                            int rows, char id[9]);
int tt_broker_client_attach(tt_broker_client *c, const char *id);
int tt_broker_client_detach(tt_broker_client *c, const char *id);
int tt_broker_client_kill(tt_broker_client *c, const char *id);
int tt_broker_client_input(tt_broker_client *c, const char *id, const void *bytes, size_t len);
int tt_broker_client_resize(tt_broker_client *c, const char *id, int cols, int rows);
int tt_broker_client_list(tt_broker_client *c, tt_buf *json);
/* Ask the detached broker to expose its registry on the public TCP API.
 * Repeating the same configuration is idempotent; a conflicting listener
 * fails rather than disrupting another GUI. */
int tt_broker_client_listen(tt_broker_client *c, const char *host, int port, const char *token,
                            bool *auth_enabled, char *err, size_t errcap);

/* Blocking startup/reconnect snapshot. The returned bytes are owned by
 * body; view points into them. */
int tt_broker_client_snapshot(tt_broker_client *c, const char *id, int scrollback, tt_buf *body);

/* Nonblocking periodic snapshot pump for the GUI loop. Only one request may
 * be in flight per client. Start writes a small local request, then fill /
 * pump integrate the response fd into the caller's poll set. `view` points
 * into client storage until the next begin/close. */
int tt_broker_client_snapshot_begin(tt_broker_client *c, const char *id, int scrollback);
int tt_broker_client_pollfd(const tt_broker_client *c, struct pollfd *out);
int tt_broker_client_pump(tt_broker_client *c, short revents, const void **body, size_t *len);

#endif
