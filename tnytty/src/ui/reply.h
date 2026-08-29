/* reply.h — terminal answers on their way back to the child.
 *
 * In `tnytty gui` tnytty *is* the terminal, so it owes the program
 * running in it an answer when it asks: cursor position (DSR/CPR),
 * device attributes (DA). ADR 0001 keeps the VT core I/O-free, so the
 * core hands its answers to a callback and the adapter decides where
 * they go — here, straight back into the pty. This shim is the adapter
 * half, kept platform-free so the round trip is unit-testable. */
#ifndef TNYTTY_UI_REPLY_H
#define TNYTTY_UI_REPLY_H

#include "vt/vt.h"

#include <stddef.h>

/* Returns bytes written, or negative on error (same contract as
 * tt_session_write). */
typedef int (*tt_reply_sink)(void *user, const char *bytes, size_t len);

typedef struct {
    tt_reply_sink sink;
    void *user;
    size_t bytes; /* total answered, for tests and diagnostics */
} tt_reply;

/* Route t's answers into sink. The tt_reply must outlive the vt. */
void tt_reply_attach(vt *t, tt_reply *r, tt_reply_sink sink, void *user);

#endif
