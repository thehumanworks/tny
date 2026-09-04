/* sse.c — Server-Sent Events line parser (~docs/language-and-runtime.md). */
#include "net/net.h"
#include <string.h>

void sse_parser_init(sse_parser *p) {
    buf_init(&p->acc);
    buf_init(&p->data);
}

void sse_parser_free(sse_parser *p) {
    buf_free(&p->acc);
    buf_free(&p->data);
}

static void handle_line(sse_parser *p, const char *line, size_t len, sse_event_cb cb, void *ud) {
    if (len == 0) { /* blank line: dispatch accumulated event */
        if (p->data.len) {
            cb(p->data.data, p->data.len, ud);
            buf_clear(&p->data);
        }
        return;
    }
    if (line[0] == ':') return; /* comment / keepalive */
    if (len >= 5 && memcmp(line, "data:", 5) == 0) {
        const char *v = line + 5;
        size_t vl = len - 5;
        if (vl && v[0] == ' ') {
            v++;
            vl--;
        }
        if (p->data.len) buf_appends(&p->data, "\n");
        buf_append(&p->data, v, vl);
    }
    /* event:/id:/retry: fields are irrelevant for our providers — ignore */
}

void sse_feed(sse_parser *p, const char *bytes, size_t n, sse_event_cb cb, void *ud) {
    buf_append(&p->acc, bytes, n);
    for (;;) {
        char *nl = memchr(p->acc.data, '\n', p->acc.len);
        if (!nl) break;
        size_t linelen = (size_t)(nl - p->acc.data);
        size_t consumed = linelen + 1;
        if (linelen && p->acc.data[linelen - 1] == '\r') linelen--;
        handle_line(p, p->acc.data, linelen, cb, ud);
        buf_consume(&p->acc, consumed);
    }
}

void sse_flush(sse_parser *p, sse_event_cb cb, void *ud) {
    if (p->acc.len) { /* an unterminated last line */
        size_t linelen = p->acc.len;
        if (p->acc.data[linelen - 1] == '\r') linelen--;
        handle_line(p, p->acc.data, linelen, cb, ud);
        buf_clear(&p->acc);
    }
    handle_line(p, "", 0, cb, ud);
}
