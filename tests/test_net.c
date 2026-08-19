/* test_net.c — SSE parser, Connect framing, URL parsing. */
#include "greatest.h"
#include "net/net.h"

#include <stdlib.h>
#include <string.h>

/* ---- collectors ---- */

typedef struct {
    char *events[32];
    int   n;
} sse_col;

static void sse_col_cb(const char *data, size_t len, void *ud) {
    sse_col *c = ud;
    if (c->n < 32) c->events[c->n++] = xstrndup(data, len);
}

static void sse_col_free(sse_col *c) {
    for (int i = 0; i < c->n; i++) free(c->events[i]);
}

typedef struct {
    uint8_t flags[32];
    char   *payloads[32];
    size_t  lens[32];
    int     n;
} frame_col;

static void frame_col_cb(uint8_t flags, const char *payload, size_t len, void *ud) {
    frame_col *c = ud;
    if (c->n < 32) {
        c->flags[c->n] = flags;
        c->lens[c->n] = len;
        c->payloads[c->n++] = xstrndup(payload ? payload : "", len);
    }
}

static void frame_col_free(frame_col *c) {
    for (int i = 0; i < c->n; i++) free(c->payloads[i]);
}

/* ---- SSE ---- */

TEST sse_single_event(void) {
    sse_parser p;
    sse_parser_init(&p);
    sse_col c = {0};
    const char *in = "data: hello\n\n";
    sse_feed(&p, in, strlen(in), sse_col_cb, &c);
    ASSERT_EQ_FMT(1, c.n, "%d");
    ASSERT_STR_EQ("hello", c.events[0]);
    sse_parser_free(&p);
    sse_col_free(&c);
    PASS();
}

TEST sse_byte_by_byte(void) {
    sse_parser p;
    sse_parser_init(&p);
    sse_col c = {0};
    const char *in = "data: {\"x\":1}\n\ndata: [DONE]\n\n";
    for (size_t i = 0; in[i]; i++) sse_feed(&p, in + i, 1, sse_col_cb, &c);
    ASSERT_EQ_FMT(2, c.n, "%d");
    ASSERT_STR_EQ("{\"x\":1}", c.events[0]);
    ASSERT_STR_EQ("[DONE]", c.events[1]);
    sse_parser_free(&p);
    sse_col_free(&c);
    PASS();
}

TEST sse_multiline_data_joined(void) {
    sse_parser p;
    sse_parser_init(&p);
    sse_col c = {0};
    const char *in = "data: line1\ndata: line2\n\n";
    sse_feed(&p, in, strlen(in), sse_col_cb, &c);
    ASSERT_EQ_FMT(1, c.n, "%d");
    ASSERT_STR_EQ("line1\nline2", c.events[0]);
    sse_parser_free(&p);
    sse_col_free(&c);
    PASS();
}

TEST sse_comments_and_crlf(void) {
    sse_parser p;
    sse_parser_init(&p);
    sse_col c = {0};
    const char *in = ": keepalive\r\n\r\ndata: ok\r\n\r\n";
    sse_feed(&p, in, strlen(in), sse_col_cb, &c);
    ASSERT_EQ_FMT(1, c.n, "%d");
    ASSERT_STR_EQ("ok", c.events[0]);
    sse_parser_free(&p);
    sse_col_free(&c);
    PASS();
}

TEST sse_ignores_event_field(void) {
    sse_parser p;
    sse_parser_init(&p);
    sse_col c = {0};
    const char *in = "event: message\ndata: payload\n\n";
    sse_feed(&p, in, strlen(in), sse_col_cb, &c);
    ASSERT_EQ_FMT(1, c.n, "%d");
    ASSERT_STR_EQ("payload", c.events[0]);
    sse_parser_free(&p);
    sse_col_free(&c);
    PASS();
}

/* ---- Connect envelope framing ---- */

TEST connect_roundtrip(void) {
    buf_t wire;
    buf_init(&wire);
    connect_frame_encode(&wire, 0, "{\"a\":1}", 7);
    connect_frame_encode(&wire, CONNECT_FLAG_END, "{\"error\":null}", 14);

    connect_decoder d;
    connect_decoder_init(&d);
    frame_col c = {0};
    ASSERT_EQ(0, connect_decoder_feed(&d, wire.data, wire.len, frame_col_cb, &c));
    ASSERT_EQ_FMT(2, c.n, "%d");
    ASSERT_EQ(0, c.flags[0]);
    ASSERT_STR_EQ("{\"a\":1}", c.payloads[0]);
    ASSERT_EQ(CONNECT_FLAG_END, c.flags[1]);
    ASSERT_STR_EQ("{\"error\":null}", c.payloads[1]);
    connect_decoder_free(&d);
    frame_col_free(&c);
    buf_free(&wire);
    PASS();
}

TEST connect_fragmented_feed(void) {
    buf_t wire;
    buf_init(&wire);
    connect_frame_encode(&wire, 0, "hello world", 11);

    connect_decoder d;
    connect_decoder_init(&d);
    frame_col c = {0};
    for (size_t i = 0; i < wire.len; i++)
        ASSERT_EQ(0, connect_decoder_feed(&d, wire.data + i, 1, frame_col_cb, &c));
    ASSERT_EQ_FMT(1, c.n, "%d");
    ASSERT_STR_EQ("hello world", c.payloads[0]);
    connect_decoder_free(&d);
    frame_col_free(&c);
    buf_free(&wire);
    PASS();
}

TEST connect_keepalives_skipped(void) {
    /* zero-length flags=0 envelopes are keepalives: the decoder swallows
     * them, but a zero-length END frame must still be delivered */
    buf_t wire;
    buf_init(&wire);
    connect_frame_encode(&wire, 0, "", 0);
    connect_frame_encode(&wire, 0, "x", 1);
    connect_frame_encode(&wire, CONNECT_FLAG_END, "", 0);
    connect_decoder d;
    connect_decoder_init(&d);
    frame_col c = {0};
    ASSERT_EQ(0, connect_decoder_feed(&d, wire.data, wire.len, frame_col_cb, &c));
    ASSERT_EQ_FMT(2, c.n, "%d");
    ASSERT_STR_EQ("x", c.payloads[0]);
    ASSERT_EQ(CONNECT_FLAG_END, c.flags[1]);
    connect_decoder_free(&d);
    frame_col_free(&c);
    buf_free(&wire);
    PASS();
}

TEST connect_oversized_rejected(void) {
    /* declared length far beyond the 64 MiB cap must fail, not allocate */
    const char hdr[5] = { 0x00, 0x7f, (char)0xff, (char)0xff, (char)0xff };
    connect_decoder d;
    connect_decoder_init(&d);
    frame_col c = {0};
    ASSERT_EQ(-1, connect_decoder_feed(&d, hdr, sizeof hdr, frame_col_cb, &c));
    ASSERT_EQ_FMT(0, c.n, "%d");
    connect_decoder_free(&d);
    PASS();
}

/* ---- URL ---- */

TEST url_parse_forms(void) {
    url_parts u;
    ASSERT_EQ(0, url_parse("http://127.0.0.1:8080/v1", &u));
    ASSERT_STR_EQ("http", u.scheme);
    ASSERT_STR_EQ("127.0.0.1", u.host);
    ASSERT_EQ_FMT(8080, u.port, "%d");
    ASSERT_STR_EQ("/v1", u.path);

    ASSERT_EQ(0, url_parse("wss://example.com/socket", &u));
    ASSERT_STR_EQ("wss", u.scheme);
    ASSERT_STR_EQ("example.com", u.host);
    ASSERT_STR_EQ("/socket", u.path);

    ASSERT_EQ(0, url_parse("unix:///tmp/bridge.sock", &u));
    ASSERT_STR_EQ("unix", u.scheme);
    ASSERT_STR_EQ("/tmp/bridge.sock", u.path);

    ASSERT(url_parse("not a url", &u) != 0);
    PASS();
}

SUITE(net_suite) {
    RUN_TEST(sse_single_event);
    RUN_TEST(sse_byte_by_byte);
    RUN_TEST(sse_multiline_data_joined);
    RUN_TEST(sse_comments_and_crlf);
    RUN_TEST(sse_ignores_event_field);
    RUN_TEST(connect_roundtrip);
    RUN_TEST(connect_fragmented_feed);
    RUN_TEST(connect_keepalives_skipped);
    RUN_TEST(connect_oversized_rejected);
    RUN_TEST(url_parse_forms);
}
