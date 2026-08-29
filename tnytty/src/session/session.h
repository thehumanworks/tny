/* session.h — the registry: a session is pty + vt + metadata. Both the
 * CLI and the HTTP API address sessions only through this. */
#ifndef TNYTTY_SESSION_H
#define TNYTTY_SESSION_H

#include "term/pty.h"
#include "vt/vt.h"

#include <stdbool.h>
#include <time.h>

#define TT_SESSION_ID_LEN 8

typedef struct tt_session {
    char id[TT_SESSION_ID_LEN + 1];
    char **argv; /* NULL-terminated, owned */
    tt_pty pty;
    vt *term;
    bool alive;
    int exit_code; /* valid once !alive */
    time_t created;
    struct tt_session *next;
} tt_session;

typedef struct {
    tt_session *head;
    int count;
    int scrollback;
} tt_registry;

void tt_registry_init(tt_registry *r, int scrollback);
void tt_registry_free(tt_registry *r);

/* Spawn argv (NULL argv = $SHELL, else /bin/sh). Returns the session or
 * NULL with errno set. */
tt_session *tt_session_create(tt_registry *r, char *const argv[], int cols, int rows);
tt_session *tt_session_find(tt_registry *r, const char *id);
/* Kill, reap (blocking), and remove. */
void tt_session_destroy(tt_registry *r, tt_session *s);

/* Drain the pty into the vt. Returns bytes consumed, 0 if nothing ready,
 * -1 on EOF/exit (session marked !alive, exit_code set). If raw is
 * non-NULL the same bytes are appended there (passthrough mirror). */
int tt_session_pump(tt_session *s, char *scratch, size_t scratch_len,
                    void (*raw)(void *user, const char *bytes, size_t len), void *raw_user);

int tt_session_write(tt_session *s, const char *bytes, size_t len);
int tt_session_resize(tt_session *s, int cols, int rows);

#endif
