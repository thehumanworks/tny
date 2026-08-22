/* tny_poll.h — the one blocking-wait seam (docs/adr/0017).
 *
 * Every frontend loop and one-shot wait goes through tny_poll instead of
 * poll(2) so the whole binary has a single suspension point to retarget:
 * native builds forward to poll(2) verbatim (src/util/tny_poll.c); the wasm
 * build waits on the pseudo-fd registry and yields to the JS event loop via
 * Asyncify (src/net/net_wasm.c). Semantics are exactly poll(2)'s: fds/events
 * in, revents out, returns the ready count, 0 on timeout, -1 on error. */
#ifndef TNY_POLL_H
#define TNY_POLL_H

#include <poll.h>

int tny_poll(struct pollfd *fds, nfds_t n, int timeout_ms);

#endif
