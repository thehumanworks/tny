/* oauth_loopback.h — shared plumbing for native browser OAuth sign-ins:
 * the 127.0.0.1 callback listener, the redirect a user pastes from another
 * machine, and the best-effort browser launch. Used by codex_login.c and
 * openai_login.c; never prints secrets. */
#ifndef TNY_OAUTH_LOOPBACK_H
#define TNY_OAUTH_LOOPBACK_H

#include <stdbool.h>
#include <stddef.h>

/* Percent-decoded value of `key` in a query string (`a=1&b=2`), malloc'd,
 * or NULL. '+' stays literal: OAuth values are percent-encoded. */
char *oauth_query_get(const char *query, size_t qlen, const char *key);

/* Listen on 127.0.0.1:port; the fd, or -1 (port busy, or no sockets on the
 * wasm build). */
int oauth_loopback_listen(int port);
/* The port a listener is bound to (port 0 asks the OS for one); -1 on error. */
int oauth_loopback_port(int lfd);

/* Best-effort launch of the system browser; the caller prints the URL. */
void oauth_open_browser(const char *url);

/* What a callback must carry to be accepted. */
typedef struct {
    const char *path;       /* exact callback path, e.g. "/auth/callback" */
    const char *state;      /* this attempt's state */
    bool require_client_id; /* registration: the issued client_id must ride along */
    const char *client_id;  /* reauthorization: a supplied client_id must equal this */
} oauth_callback_expect;

typedef enum {
    OAUTH_CALLBACK_NONE = 0, /* not this attempt's result; keep waiting */
    OAUTH_CALLBACK_CODE,     /* code (and client_id when present) */
    OAUTH_CALLBACK_ERROR     /* this attempt was refused; error names why */
} oauth_callback_kind;

typedef struct {
    char *code;      /* authorization code (secret) */
    char *client_id; /* issued client id from the callback, or NULL */
    char *error;     /* OAuth error code for OAUTH_CALLBACK_ERROR */
} oauth_callback;

void oauth_callback_free(oauth_callback *cb);

/* Validate a callback query against `e`. *why names a rejection (static
 * text, safe to print). A state-matched `error=` or a foreign client_id is
 * OAUTH_CALLBACK_ERROR; a stray or malformed request is NONE. */
oauth_callback_kind oauth_callback_parse(const char *query, size_t qlen,
                                         const oauth_callback_expect *e, oauth_callback *out,
                                         const char **why);

/* Accept one connection on lfd and answer it with a small page. */
oauth_callback_kind oauth_loopback_serve(int lfd, const oauth_callback_expect *e,
                                         oauth_callback *out);

#endif
