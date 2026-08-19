/* ready.c — the `cursor-sdk-bridge ready {json}` stderr handshake.
 * Isolated so it stays unit-testable: no I/O, no globals, no allocation of
 * anything the caller has to free. Never echoes the payload into errors —
 * the ready line can carry the bridge bearer token. */
#include "backends/cursor/cursor.h"

#include <stdio.h>
#include <string.h>

#define READY_MAX_LINE (64u * 1024u)

bool cursor_ready_is_line(const char *line, size_t len) {
    size_t pl = sizeof(CURSOR_READY_PREFIX) - 1;
    return line && len >= pl && memcmp(line, CURSOR_READY_PREFIX, pl) == 0;
}

/* Copy a required string field (camelCase first, then the proto name). */
static const char *field(yyjson_val *o, const char *camel, const char *snake) {
    const char *v = jget_str(o, camel);
    return v ? v : jget_str(o, snake);
}

static bool copy_field(char *dst, size_t cap, const char *src) {
    if (!src) return false;
    size_t n = strlen(src);
    if (n == 0 || n >= cap) return false;
    memcpy(dst, src, n + 1);
    return true;
}

int cursor_ready_parse(const char *line, size_t len, cursor_ready *out,
                       char *err, size_t errlen) {
    if (!cursor_ready_is_line(line, len)) return 0;
    if (len > READY_MAX_LINE) {
        snprintf(err, errlen, "bridge ready line is implausibly large");
        return -1;
    }
    const char *json = line + (sizeof(CURSOR_READY_PREFIX) - 1);
    size_t jlen = len - (sizeof(CURSOR_READY_PREFIX) - 1);
    while (jlen && (json[jlen - 1] == '\r' || json[jlen - 1] == '\n' ||
                    json[jlen - 1] == ' ' || json[jlen - 1] == '\t'))
        jlen--;
    while (jlen && (*json == ' ' || *json == '\t')) { json++; jlen--; }
    if (!jlen) {
        snprintf(err, errlen, "bridge ready line has no JSON payload");
        return -1;
    }

    yyjson_doc *doc = jparse(json, jlen);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!root || !yyjson_is_obj(root)) {
        snprintf(err, errlen, "bridge ready line is not a JSON object");
        yyjson_doc_free(doc);
        return -1;
    }

    cursor_ready r;
    memset(&r, 0, sizeof r);
    r.schema_version = (int)jget_int(root, "schemaVersion",
                                     jget_int(root, "schema_version", 0));
    copy_field(r.transport, sizeof r.transport, jget_str(root, "transport"));
    copy_field(r.protocol, sizeof r.protocol, jget_str(root, "protocol"));
    copy_field(r.auth_token_file, sizeof r.auth_token_file,
               field(root, "authTokenFile", "auth_token_file"));
    copy_field(r.auth_token, sizeof r.auth_token,
               field(root, "authToken", "auth_token"));

    const char *url = jget_str(root, "url");
    if (url && *url) {
        if (!copy_field(r.url, sizeof r.url, url)) {
            snprintf(err, errlen, "bridge ready line has an oversized url");
            yyjson_doc_free(doc);
            return -1;
        }
        size_t ul = strlen(r.url);
        while (ul > 1 && r.url[ul - 1] == '/') r.url[--ul] = 0;
    } else {
        const char *host = jget_str(root, "host");
        int64_t port = jget_int(root, "port", 0);
        if (host && *host && port > 0 && port < 65536)
            snprintf(r.url, sizeof r.url, "http://%s:%d", host, (int)port);
    }

    const char *why = NULL;
    url_parts u;
    if (r.schema_version != 1) why = "unsupported schemaVersion (need 1)";
    else if (strcmp(r.transport, "tcp") != 0) why = "transport is not \"tcp\"";
    else if (strcmp(r.protocol, "connect") != 0) why = "protocol is not \"connect\"";
    else if (!r.url[0]) why = "no usable endpoint (need url, or host and port)";
    else if (url_parse(r.url, &u) != 0 ||
             (strcmp(u.scheme, "http") != 0 && strcmp(u.scheme, "https") != 0))
        why = "endpoint is not an http(s) URL";
    else if (!r.auth_token_file[0] && !r.auth_token[0])
        why = "no authTokenFile and no inline authToken";
    yyjson_doc_free(doc);
    if (why) {
        snprintf(err, errlen, "bridge ready line rejected: %s", why);
        return -1;
    }
    *out = r;
    return 1;
}
