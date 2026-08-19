/* connectrpc.c — Connect streaming envelope framing (cursor-bridge.md).
 * frame: flags:1 | length:4 big-endian | payload. flags&0x02 = end-of-stream. */
#include "net/net.h"
#include <string.h>

#define CONNECT_MAX_FRAME (64u * 1024u * 1024u)

void connect_frame_encode(buf_t *out, uint8_t flags, const char *payload, size_t len) {
    uint8_t head[5];
    head[0] = flags;
    head[1] = (uint8_t)(len >> 24);
    head[2] = (uint8_t)(len >> 16);
    head[3] = (uint8_t)(len >> 8);
    head[4] = (uint8_t)len;
    buf_append(out, head, 5);
    if (len) buf_append(out, payload, len);
}

void connect_decoder_init(connect_decoder *d) { buf_init(&d->acc); }
void connect_decoder_free(connect_decoder *d) { buf_free(&d->acc); }

int connect_decoder_feed(connect_decoder *d, const char *bytes, size_t n,
                         connect_frame_cb cb, void *ud) {
    buf_append(&d->acc, bytes, n);
    for (;;) {
        if (d->acc.len < 5) return 0;
        const uint8_t *h = (const uint8_t *)d->acc.data;
        uint8_t flags = h[0];
        uint32_t len = (uint32_t)h[1] << 24 | (uint32_t)h[2] << 16 |
                       (uint32_t)h[3] << 8 | h[4];
        if (len > CONNECT_MAX_FRAME) return -1;
        if (d->acc.len < 5 + (size_t)len) return 0;
        /* empty envelopes are keepalives — skip silently */
        if (len > 0 || flags != 0)
            cb(flags, d->acc.data + 5, len, ud);
        buf_consume(&d->acc, 5 + (size_t)len);
    }
}
