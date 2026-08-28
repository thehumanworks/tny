/* url.c — URL splitting, shared by every transport (native sockets and the
 * wasm fetch/WebSocket glue), so the parser exists exactly once. */
#include "net/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int url_parse(const char *url, url_parts *out) {
    memset(out, 0, sizeof *out);
    const char *p = strstr(url, "://");
    if (!p || (size_t)(p - url) >= sizeof out->scheme) return -1;
    memcpy(out->scheme, url, (size_t)(p - url));
    p += 3;
    if (strcmp(out->scheme, "unix") == 0) {
        snprintf(out->path, sizeof out->path, "%s", p);
        return 0;
    }
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(hostend - p));
    if (colon) {
        size_t hl = (size_t)(colon - p);
        if (hl >= sizeof out->host) return -1;
        memcpy(out->host, p, hl);
        out->port = atoi(colon + 1);
    } else {
        size_t hl = (size_t)(hostend - p);
        if (hl >= sizeof out->host) return -1;
        memcpy(out->host, p, hl);
        if (strcmp(out->scheme, "https") == 0 || strcmp(out->scheme, "wss") == 0) out->port = 443;
        else out->port = 80;
    }
    snprintf(out->path, sizeof out->path, "%s", slash ? slash : "/");
    if (!out->host[0] || out->port <= 0) return -1;
    return 0;
}
