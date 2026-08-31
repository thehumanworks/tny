/* callbacks.h — host-owned sdk.v1 custom-tool and local-store callbacks. */
#ifndef TNY_BACKENDS_CURSOR_CALLBACKS_H
#define TNY_BACKENDS_CURSOR_CALLBACKS_H

#include <stdbool.h>
#include <stddef.h>
#include <poll.h>
#include <pthread.h>

struct custom_tool_registry;
typedef struct cursor_callbacks cursor_callbacks;
typedef int (*cursor_callbacks_thread_create_fn)(pthread_t *thread, const pthread_attr_t *attr,
                                                 void *(*start)(void *), void *arg);

#define CURSOR_CALLBACK_POLLFD_CAPACITY 10

typedef struct {
    struct custom_tool_registry *tools; /* borrowed */
    const char *state_dir;              /* tny state root; copied */
    bool enable_tools;
    bool enable_store;
    bool allow_sensitive_tools;
    /* Private dependency-injection seam. NULL selects pthread_create. */
    cursor_callbacks_thread_create_fn thread_create;
} cursor_callbacks_options;

cursor_callbacks *cursor_callbacks_start(const cursor_callbacks_options *options, char *err,
                                         size_t errlen);
const char *cursor_callbacks_url(const cursor_callbacks *callbacks);
const char *cursor_callbacks_token(const cursor_callbacks *callbacks);
bool cursor_callbacks_tools_started(const cursor_callbacks *callbacks);
bool cursor_callbacks_store_started(const cursor_callbacks *callbacks);

/* Protojson map for LocalAgentOptions.customTools. configured_tools_json may
 * be a map of author metadata; its matching outputSchema is preserved. */
char *cursor_callbacks_tool_definitions(cursor_callbacks *callbacks,
                                        const char *configured_tools_json);

int cursor_callbacks_pollfds(cursor_callbacks *callbacks, struct pollfd *fds, int max);
int cursor_callbacks_dispatch(cursor_callbacks *callbacks, const struct pollfd *fds, int n);

/* During blocking bridge unary RPCs (notably CreateAgent/ResumeAgent), the
 * bridge may synchronously call the custom store. This bounded helper lends
 * the one callback server to a single pump thread until blocking_end. Existing
 * deferred tools remain pending for the owner thread; new custom tools fail
 * closed because libtny callbacks are owner-thread-only. */
int cursor_callbacks_blocking_begin(cursor_callbacks *callbacks, char *err, size_t errlen);
void cursor_callbacks_blocking_end(cursor_callbacks *callbacks);
void cursor_callbacks_stop(cursor_callbacks *callbacks);
void cursor_callbacks_destroy(cursor_callbacks **callbacksp);

#endif
