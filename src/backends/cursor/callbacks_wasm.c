/* callbacks_wasm.c — clean unsupported sdk.v1 callback seam for wasm.
 * Browser fetch cannot accept loopback HTTP callbacks, so the wasm build
 * supplies the same API without pulling native sockets or filesystem stores. */
#include "backends/cursor/callbacks.h"

#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>

struct cursor_callbacks {
    int unused;
};

cursor_callbacks *cursor_callbacks_start(const cursor_callbacks_options *options, char *err,
                                         size_t errlen) {
    (void)options;
    if (err && errlen)
        snprintf(err, errlen, "cursor sdk.v1 callbacks are unavailable in the wasm build");
    return NULL;
}

const char *cursor_callbacks_url(const cursor_callbacks *callbacks) {
    (void)callbacks;
    return NULL;
}

const char *cursor_callbacks_token(const cursor_callbacks *callbacks) {
    (void)callbacks;
    return NULL;
}

bool cursor_callbacks_tools_started(const cursor_callbacks *callbacks) {
    (void)callbacks;
    return false;
}

bool cursor_callbacks_store_started(const cursor_callbacks *callbacks) {
    (void)callbacks;
    return false;
}

char *cursor_callbacks_tool_definitions(cursor_callbacks *callbacks,
                                        const char *configured_tools_json) {
    (void)callbacks;
    (void)configured_tools_json;
    return xstrdup("{}");
}

int cursor_callbacks_pollfds(cursor_callbacks *callbacks, struct pollfd *fds, int max) {
    (void)callbacks;
    (void)fds;
    (void)max;
    return 0;
}

int cursor_callbacks_dispatch(cursor_callbacks *callbacks, const struct pollfd *fds, int n) {
    (void)callbacks;
    (void)fds;
    (void)n;
    return 0;
}

int cursor_callbacks_blocking_begin(cursor_callbacks *callbacks, char *err, size_t errlen) {
    (void)callbacks;
    if (err && errlen)
        snprintf(err, errlen, "cursor sdk.v1 callbacks are unavailable in the wasm build");
    return -1;
}

void cursor_callbacks_blocking_end(cursor_callbacks *callbacks) { (void)callbacks; }

void cursor_callbacks_stop(cursor_callbacks *callbacks) { (void)callbacks; }

void cursor_callbacks_destroy(cursor_callbacks **callbacksp) {
    if (!callbacksp || !*callbacksp) return;
    free(*callbacksp);
    *callbacksp = NULL;
}
