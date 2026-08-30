/* broker.h — detached per-user owner of tnytty sessions. */
#ifndef TNYTTY_BROKER_BROKER_H
#define TNYTTY_BROKER_BROKER_H

#include <stddef.h>

/* $XDG_RUNTIME_DIR/tnytty/daemon.sock, else a short /tmp/tnytty-UID path.
 * The containing directory is created/validated mode 0700. */
int tt_broker_default_socket(char *out, size_t cap, char *err, size_t errcap);
int tt_broker_prepare_socket(const char *path, char *err, size_t errcap);

/* Foreground broker loop. ready_fd >= 0 receives one byte once bind has
 * succeeded (or '0' before a startup failure). */
int tt_broker_run(const char *socket_path, int ready_fd);

/* Ensure a detached broker is accepting the socket. Safe to call before
 * AppKit initialization; returns only after a successful connect probe. */
int tt_broker_start(const char *socket_path, int timeout_ms, char *err, size_t errcap);

#endif
