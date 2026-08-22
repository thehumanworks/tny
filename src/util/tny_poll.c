/* tny_poll.c — native tny_poll: poll(2), nothing else. The wasm build
 * excludes this file and gets tny_poll from src/net/net_wasm.c instead. */
#include "util/tny_poll.h"

int tny_poll(struct pollfd *fds, nfds_t n, int timeout_ms) {
    return poll(fds, n, timeout_ms);
}
