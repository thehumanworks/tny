/* test_net.c — SSE parser, Connect framing, URL parsing, TLS stream. */
#include "greatest.h"
#include "net/net.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- collectors ---- */

typedef struct {
    char *events[32];
    int n;
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
    char *payloads[32];
    size_t lens[32];
    int n;
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
    const char hdr[5] = {0x00, 0x7f, (char)0xff, (char)0xff, (char)0xff};
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

/* ---- chunked transfer decoding (http1.c) ---- */

/* Feed a canned HTTP response through a socketpair in `slice`-byte writes,
 * draining the parser between writes so every possible read boundary is
 * exercised — including the CRLF-after-chunk-data split that once made the
 * decoder mistake the leftover bytes for the terminating 0-chunk. */
static int chunked_drive(const char *resp, size_t resp_len, int slice, buf_t *body) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    set_nonblock(sv[0], true);
    http_conn *c = http_from_fd(sv[0]);
    int status = -2;
    int rc = 0;
    for (size_t off = 0; off < resp_len && rc == 0;) {
        size_t n = (size_t)slice < resp_len - off ? (size_t)slice : resp_len - off;
        if (write(sv[1], resp + off, n) != (ssize_t)n) {
            rc = -1;
            break;
        }
        off += n;
        if (status == -2) {
            status = http_read_response(c, 0);
            if (status == -2) continue;
            if (status != 200) {
                rc = -1;
                break;
            }
        }
        for (;;) {
            char tmp[64];
            ssize_t bn = http_body_read(c, tmp, sizeof tmp);
            if (bn > 0) {
                buf_append(body, tmp, (size_t)bn);
                continue;
            }
            if (bn == -2) break;   /* would-block: feed the next slice */
            rc = bn == 0 ? 1 : -1; /* 1 done, -1 framing error */
            break;
        }
    }
    http_close(c);
    close(sv[1]);
    return rc;
}

TEST chunked_survives_every_split_boundary(void) {
    /* uppercase hex on purpose; body spells out the reassembled payload */
    const char resp[] = "HTTP/1.1 200 OK\r\n"
                        "Transfer-Encoding: chunked\r\n\r\n"
                        "5\r\nhello\r\n"
                        "A\r\n 0123456\r\n\r\n"
                        "2\r\nok\r\n"
                        "0\r\n\r\n";
    for (int slice = 1; slice <= 7; slice++) {
        buf_t body;
        buf_init(&body);
        int rc = chunked_drive(resp, sizeof resp - 1, slice, &body);
        ASSERT_EQm("clean end-of-body at every slice size", 1, rc);
        ASSERT_STR_EQ("hello 0123456\r\nok", body.data);
        buf_free(&body);
    }
    PASS();
}

/* MCP uses the same HTTP response for fixed JSON and chunked JSON. Exercise
 * every exact two-write boundary across status, session header, chunk size,
 * JSON UTF-8 bytes, data CRLF, and the terminal zero chunk. */
static int chunked_drive_exact_split(const char *resp, size_t resp_len, size_t split, buf_t *body,
                                     char *session, size_t session_cap) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    set_nonblock(sv[0], true);
    http_conn *c = http_from_fd(sv[0]);
    int status = -2;
    int rc = 0;
    const size_t parts[2] = {split, resp_len - split};
    size_t off = 0;
    for (int part = 0; part < 2 && rc == 0; part++) {
        if (write(sv[1], resp + off, parts[part]) != (ssize_t)parts[part]) {
            rc = -1;
            break;
        }
        off += parts[part];
        if (status == -2) {
            status = http_read_response(c, 0);
            if (status == 200) {
                const char *value = http_header(c, "Mcp-Session-Id");
                if (!value) {
                    rc = -1;
                    break;
                }
                snprintf(session, session_cap, "%s", value);
            } else if (status != -2) {
                rc = -1;
                break;
            }
        }
        if (status != 200) continue;
        for (;;) {
            char tmp[17];
            ssize_t n = http_body_read(c, tmp, sizeof tmp);
            if (n > 0) {
                buf_append(body, tmp, (size_t)n);
                continue;
            }
            if (n == -2) break;
            rc = n == 0 ? 1 : -1;
            break;
        }
    }
    http_close(c);
    close(sv[1]);
    return rc;
}

TEST mcp_chunked_json_survives_each_exact_split(void) {
    const char payload[] =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"tools\":[{\"name\":\"echo\","
        "\"description\":\"split ☃ safely\"}]}}";
    buf_t response;
    buf_init(&response);
    buf_appends(&response, "HTTP/1.1 200 OK\r\n");
    buf_appends(&response, "Content-Type: application/json\r\n");
    buf_appends(&response, "Mcp-Session-Id: session-split-87\r\n");
    buf_appends(&response, "Transfer-Encoding: chunked\r\n\r\n");
    size_t first = 19;
    buf_appendf(&response, "%zX\r\n", first);
    buf_append(&response, payload, first);
    buf_appends(&response, "\r\n");
    buf_appendf(&response, "%zX\r\n", sizeof payload - 1 - first);
    buf_append(&response, payload + first, sizeof payload - 1 - first);
    buf_appends(&response, "\r\n0\r\n\r\n");

    for (size_t split = 1; split < response.len; split++) {
        buf_t body;
        buf_init(&body);
        char session[64] = "";
        int rc = chunked_drive_exact_split(response.data, response.len, split, &body, session,
                                           sizeof session);
        ASSERT_EQm("clean end-of-body at each exact split", 1, rc);
        ASSERT_STR_EQ(payload, body.data);
        ASSERT_STR_EQ("session-split-87", session);
        buf_free(&body);
    }
    buf_free(&response);
    PASS();
}

TEST chunked_garbage_size_line_is_an_error(void) {
    const char resp[] = "HTTP/1.1 200 OK\r\n"
                        "Transfer-Encoding: chunked\r\n\r\n"
                        "5\r\nhello\r\n"
                        "XYZ\r\n"; /* not hex: must fail, not end cleanly */
    buf_t body;
    buf_init(&body);
    int rc = chunked_drive(resp, sizeof resp - 1, 3, &body);
    ASSERT_EQ(-1, rc);
    ASSERT_STR_EQ("hello", body.data); /* payload before the break survives */
    buf_free(&body);
    PASS();
}

/* ---- TLS stream (stream.c) ---- */

/* Accept one client and answer with plain HTTP instead of a TLS record. */
static void *tls_junk_server(void *arg) {
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd >= 0) {
        const char junk[] = "HTTP/1.0 400 not tls\r\n\r\n";
        (void)!write(cfd, junk, sizeof junk - 1);
        close(cfd);
    }
    return NULL;
}

TEST tls_to_plain_http_server_fails_cleanly(void) {
    /* Two rounds: the second one runs against the already-loaded TLS
     * library, so the load-once cache path is a handshake too. */
    for (int round = 0; round < 2; round++) {
        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT(lfd >= 0);
        struct sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(0, bind(lfd, (struct sockaddr *)&sa, sizeof sa));
        socklen_t sl = sizeof sa;
        ASSERT_EQ(0, getsockname(lfd, (struct sockaddr *)&sa, &sl));
        ASSERT_EQ(0, listen(lfd, 1));
        pthread_t th;
        ASSERT_EQ(0, pthread_create(&th, NULL, tls_junk_server, (void *)(intptr_t)lfd));
        char err[256] = "";
        nstream *s =
            nstream_connect("127.0.0.1", (int)ntohs(sa.sin_port), true, 3000, err, sizeof err);
        pthread_join(th, NULL);
        close(lfd);
        ASSERT_EQ(NULL, s);
        ASSERT(err[0] != '\0');
#if defined(__APPLE__) || defined(__linux__)
        /* TLS is implemented here: this must be a real handshake failure,
         * never the "https not built" fallback or a failed library load. */
        ASSERT(strstr(err, "handshake") != NULL);
#endif
    }
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
    RUN_TEST(chunked_survives_every_split_boundary);
    RUN_TEST(mcp_chunked_json_survives_each_exact_split);
    RUN_TEST(chunked_garbage_size_line_is_an_error);
    RUN_TEST(tls_to_plain_http_server_fails_cleanly);
}
