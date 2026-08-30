#include "broker/protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *find_header_end(const char *s, size_t len) {
    if (len < 4) return NULL;
    for (size_t i = 0; i + 4 <= len; i++)
        if (memcmp(s + i, "\r\n\r\n", 4) == 0) return s + i + 4;
    return NULL;
}

void tt_http_response_parser_init(tt_http_response_parser *p) {
    memset(p, 0, sizeof *p);
    tt_buf_init(&p->bytes);
}

void tt_http_response_parser_free(tt_http_response_parser *p) {
    tt_buf_free(&p->bytes);
    memset(p, 0, sizeof *p);
}

int tt_http_response_parser_feed(tt_http_response_parser *p, const void *bytes, size_t len) {
    if (p->complete) {
        errno = EALREADY;
        return -1;
    }
    if (len > TT_BROKER_RESPONSE_MAX - p->bytes.len) {
        errno = EMSGSIZE;
        return -1;
    }
    if (!tt_buf_append(&p->bytes, bytes, len)) {
        errno = ENOMEM;
        return -1;
    }
    if (!p->header_len) {
        const char *end = find_header_end(p->bytes.data, p->bytes.len);
        if (!end) return 0;
        p->header_len = (size_t)(end - p->bytes.data);
        if (sscanf(p->bytes.data, "HTTP/1.1 %d", &p->status) != 1) {
            errno = EPROTO;
            return -1;
        }
        const char *at = p->bytes.data;
        const char *limit = p->bytes.data + p->header_len;
        bool found = false;
        while (at < limit) {
            const char *line = strstr(at, "\r\n");
            if (!line || line >= limit) break;
            if ((size_t)(line - at) >= 15 && strncasecmp(at, "Content-Length:", 15) == 0) {
                char *tail = NULL;
                errno = 0;
                unsigned long long n = strtoull(at + 15, &tail, 10);
                if (errno || !tail || tail > line || n > TT_BROKER_RESPONSE_MAX - p->header_len) {
                    errno = EPROTO;
                    return -1;
                }
                while (tail < line && (*tail == ' ' || *tail == '\t')) tail++;
                if (tail != line) {
                    errno = EPROTO;
                    return -1;
                }
                p->content_len = (size_t)n;
                found = true;
                break;
            }
            at = line + 2;
        }
        if (!found) {
            errno = EPROTO;
            return -1;
        }
    }
    if (p->bytes.len < p->header_len + p->content_len) return 0;
    if (p->bytes.len != p->header_len + p->content_len) {
        errno = EPROTO;
        return -1;
    }
    p->complete = true;
    return 1;
}

const unsigned char *tt_http_response_body(const tt_http_response_parser *p, size_t *len) {
    if (!p->complete) return NULL;
    if (len) *len = p->content_len;
    return (const unsigned char *)p->bytes.data + p->header_len;
}
