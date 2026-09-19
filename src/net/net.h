/* net.h — sockets, TLS shim, HTTP/1.1 + SSE, WebSocket, Connect framing.
 * Shared net code has no knowledge of agents (docs/language-and-runtime.md). */
#ifndef TNY_NET_H
#define TNY_NET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "util/util.h"

/* ---- URL ---- */
typedef struct {
    char scheme[16]; /* http, https, unix */
    char host[256];
    int port;
    char path[1024]; /* includes leading '/', or unix socket path for unix:// */
} url_parts;

int url_parse(const char *url, url_parts *out); /* 0 ok */

/* ---- TCP ---- */
int tcp_connect(const char *host, int port, int timeout_ms); /* fd or -1 */
int unix_connect(const char *path);                          /* fd or -1 */
/* One socket write, with send(2) byte-count/errno semantics and no SIGPIPE.
 * Descriptor-local protection is retained; process signal handlers are untouched. */
ssize_t socket_write(int fd, const void *data, size_t len);
/* Bind + listen on an AF_UNIX stream socket (0600, nonblocking). Unlinks a
 * stale path first — callers hold the session writer flock, so a live
 * listener is never unlinked (docs/adr/0053). fd or -1. */
int unix_listen(const char *path);
int set_nonblock(int fd, bool nb);

/* ---- stream: plain fd or TLS ---- */
typedef struct nstream nstream;

/* Connect TCP (+TLS when tls=true, SNI/verify against host). Blocking
 * handshake with timeout; the fd is left non-blocking afterwards. */
nstream *nstream_connect(const char *host, int port, bool tls, int timeout_ms, char *err,
                         size_t errlen);
nstream *nstream_from_fd(int fd); /* plain, takes ownership */

/* >0 bytes, 0 clean EOF, -1 fatal, -2 would-block (poll and retry). */
ssize_t nstream_read(nstream *s, void *buf, size_t cap);
/* Write everything (poll-waits internally). 0 ok, -1 fatal. */
int nstream_write_all(nstream *s, const void *data, size_t len);
int nstream_fd(nstream *s);
void nstream_close(nstream *s);
/* macOS SecureTransport/CoreFoundation starts process-global runtime state
 * that Apple marks unsafe to inherit across fork without an immediate exec.
 * False only after this process has attempted its first TLS connection;
 * other platforms currently remain fork-safe for the runner transport. */
bool nstream_fork_safe(void);

/* ---- HTTP/1.1 client ---- */
typedef struct http_conn http_conn;

/* base_url: http(s)://host[:port][/prefix] */
http_conn *http_open(const char *base_url, char *err, size_t errlen);
/* Wrap an already-connected fd (plain, no TLS); test seam for the parser. */
http_conn *http_from_fd(int fd);
/* headers: NULL-terminated array of "Name: value" strings (may be NULL). */
int http_request(http_conn *c, const char *method, const char *path, const char **headers,
                 const char *body, size_t body_len);
/* Read the status line + headers. Returns the status code, -1 on error/EOF,
 * or -2 if headers are still incomplete after timeout_ms (poll and retry). */
int http_read_response(http_conn *c, int timeout_ms);
/* Case-insensitive header lookup after http_read_response; borrowed,
 * NUL-terminated copy valid until next request. */
const char *http_header(http_conn *c, const char *name);
/* Decoded body bytes (handles chunked + content-length + read-to-EOF).
 * >0 bytes, 0 body complete, -1 error, -2 would-block. */
ssize_t http_body_read(http_conn *c, char *out, size_t cap);
int http_fd(http_conn *c);
void http_close(http_conn *c);
/* The path prefix from base_url (e.g. "/v1"), for building request paths. */
const char *http_prefix(http_conn *c);

/* ---- SSE ---- */
/* Private parser statuses: malformed/limit vs allocation failure. Views passed
 * to callbacks are borrowed only until that callback returns. No reentry.
 * Init is allocation-free; the first nonempty feed allocates lazily. */
enum { TNY_PARSE_OK = 0, TNY_PARSE_INVALID = -1, TNY_PARSE_OOM = -2 };
typedef void (*sse_event_cb)(const char *data, size_t len, void *ud);

typedef struct {
    void *owner; /* private move-only owner; never copy an initialized parser */
    int status;  /* sticky until free/init */
} sse_parser;

void sse_parser_init(sse_parser *p);
void sse_parser_free(sse_parser *p);
/* Feed raw body bytes; cb fires once per complete event (data joined by \n). */
int sse_feed(sse_parser *p, const char *bytes, size_t n, sse_event_cb cb, void *ud);
/* End of body: dispatch a final event whose terminating blank line never
 * arrived (a last `data:` line closed by EOF). */
int sse_flush(sse_parser *p, sse_event_cb cb, void *ud);

#ifdef __cplusplus
}
#endif

#endif
