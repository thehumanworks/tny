/* session.h — the registry: a session is pty + vt + metadata. Both the
 * CLI and the HTTP API address sessions only through this. */
#ifndef TNYTTY_SESSION_H
#define TNYTTY_SESSION_H

#include "term/pty.h"
#include "vt/vt.h"

#include <poll.h>
#include <stdbool.h>
#include <time.h>

#define TT_SESSION_ID_LEN 8

/* Per-session pending-input cap. Input the pty cannot take yet is queued
 * here and drained when the master fd is writable; a writer that gets
 * this far ahead of the child is rejected rather than served silently
 * truncated input (docs/http-api.md, docs/architecture.md). */
#define TT_INPUT_QUEUE_MAX (4u * 1024u * 1024u)
/* Adapters that own their input source (the `run` tty) stop reading it
 * above this mark, so the cap itself is only reachable through the API. */
#define TT_INPUT_HIGH_WATER (1u * 1024u * 1024u)
/* Bound one readable pty's work per event-loop turn so a continuous
 * producer cannot starve signals, HTTP, window events, or other panes. */
#define TT_PUMP_READS_PER_TURN 4

typedef struct tt_session {
    char id[TT_SESSION_ID_LEN + 1];
    char **argv; /* NULL-terminated, owned */
    tt_pty pty;
    vt *term;
    bool alive;
    /* A foreground adapter retains this pointer and owns its geometry and
     * lifetime. The HTTP API may still read/write it, but must not resize
     * or destroy it behind that adapter. */
    bool attached;
    int exit_code; /* valid once !alive */
    time_t created;
    /* Input the pty could not take yet: [pend_off, pend_len) is live. */
    char *pending;
    size_t pend_off, pend_len, pend_cap;
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
tt_session *tt_session_create_at(tt_registry *r, char *const argv[], int cols, int rows,
                                 const char *cwd);
tt_session *tt_session_find(tt_registry *r, const char *id);
/* Make this registry owner answer DSR/DA directly into the PTY. Broker-owned
 * sessions enable this so queries keep working with no frontend attached. */
void tt_session_enable_replies(tt_session *s);
/* Kill, reap (blocking), and remove. */
void tt_session_destroy(tt_registry *r, tt_session *s);

/* Drain the pty into the vt. Returns bytes consumed, 0 if nothing ready,
 * -1 on EOF/exit (session marked !alive, exit_code set). If raw is
 * non-NULL the same bytes are appended there (passthrough mirror). */
int tt_session_pump(tt_session *s, char *scratch, size_t scratch_len,
                    void (*raw)(void *user, const char *bytes, size_t len), void *raw_user);
/* Pump at most `max_reads` chunks. Returns the bytes consumed, or -1
 * when no bytes were consumed and the session reached EOF. */
int tt_session_drain(tt_session *s, char *scratch, size_t scratch_len, int max_reads,
                     void (*raw)(void *user, const char *bytes, size_t len), void *raw_user);

/* Write to the pty, queueing whatever write(2) cannot take right now so
 * no byte is ever dropped mid-sequence. All-or-nothing: returns len when
 * every byte was written or queued, 0 for len == 0, or -1 with errno set
 * — ENOBUFS when the queue cap (TT_INPUT_QUEUE_MAX) would be exceeded
 * (nothing is queued), ENXIO when the session is gone. Queued bytes are
 * always sent before later ones. */
int tt_session_write(tt_session *s, const char *bytes, size_t len);
/* Bytes still queued for the pty; the loop polls POLLOUT while > 0. */
size_t tt_session_pending(const tt_session *s);
/* Drain the queue into the pty (call when the master fd is writable).
 * Returns bytes written, or -1 if the pty died (queue dropped). */
int tt_session_flush(tt_session *s);
int tt_session_resize(tt_session *s, int cols, int rows);

/* Add alive sessions whose attached flag matches `attached` to a poll
 * slice, then service that same slice without forwarding raw output.
 * `sessions` has `max` entries and records the session for each fd. */
int tt_registry_poll_fill(tt_registry *r, struct pollfd *fds, tt_session **sessions, int max,
                          bool attached);
void tt_registry_poll_handle(const struct pollfd *fds, tt_session *const *sessions, int n,
                             char *scratch, size_t scratch_len);

#endif
