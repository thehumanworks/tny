/* ws.c — WebSocket client: tny owns TCP/TLS + the HTTP handshake, wslay owns
 * framing (docs/language-and-runtime.md). Text frames only for JSON-RPC. */
#include "net/net.h"
#include "util/tny_poll.h"
#include "picohttpparser.h"
#include <wslay/wslay.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

struct ws_conn {
    nstream *s;
    wslay_event_context_ptr ctx;
    buf_t pre;        /* bytes read past the handshake, fed to wslay first */
    ws_msg_cb cb;
    void *ud;
    bool dead;
};

static ssize_t recv_cb(wslay_event_context_ptr ctx, uint8_t *buf, size_t len,
                       int flags, void *user_data) {
    (void)flags;
    ws_conn *w = user_data;
    if (w->pre.len) {
        size_t take = w->pre.len < len ? w->pre.len : len;
        memcpy(buf, w->pre.data, take);
        buf_consume(&w->pre, take);
        return (ssize_t)take;
    }
    ssize_t n = nstream_read(w->s, buf, len);
    if (n > 0) return n;
    if (n == -2) {
        wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        return -1;
    }
    wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
    return -1;
}

static ssize_t send_cb(wslay_event_context_ptr ctx, const uint8_t *data,
                       size_t len, int flags, void *user_data) {
    (void)flags;
    ws_conn *w = user_data;
    if (nstream_write_all(w->s, data, len) != 0) {
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
    }
    return (ssize_t)len;
}

static int genmask_cb(wslay_event_context_ptr ctx, uint8_t *buf, size_t len,
                      void *user_data) {
    (void)ctx; (void)user_data;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t got = read(fd, buf, len);
        close(fd);
        if (got == (ssize_t)len) return 0;
    }
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(now_ms() >> (i % 8));
    return 0;
}

static void on_msg_cb(wslay_event_context_ptr ctx,
                      const struct wslay_event_on_msg_recv_arg *arg,
                      void *user_data) {
    (void)ctx;
    ws_conn *w = user_data;
    if (arg->opcode == WSLAY_TEXT_FRAME && w->cb)
        w->cb((const char *)arg->msg, arg->msg_length, w->ud);
    /* ping/pong handled by wslay; binary frames ignored per codex doc */
}

ws_conn *ws_connect(const char *url, const char *bearer, int timeout_ms,
                    char *err, size_t errlen) {
    url_parts u;
    if (url_parse(url, &u) != 0) {
        snprintf(err, errlen, "bad ws URL: %s", url);
        return NULL;
    }
    nstream *s = NULL;
    const char *host_hdr = u.host;
    const char *req_path = u.path;
    if (strcmp(u.scheme, "unix") == 0) {
        int fd = unix_connect(u.path);
        if (fd < 0) { snprintf(err, errlen, "unix connect %s failed", u.path); return NULL; }
        s = nstream_from_fd(fd);
        host_hdr = "localhost";
        req_path = "/rpc"; /* dummy upgrade URL per codex-app-server.md */
    } else {
        bool tls = strcmp(u.scheme, "wss") == 0;
        if (!tls && strcmp(u.scheme, "ws") != 0) {
            snprintf(err, errlen, "unsupported scheme %s", u.scheme);
            return NULL;
        }
        s = nstream_connect(u.host, u.port, tls, timeout_ms, err, errlen);
        if (!s) return NULL;
    }

    ws_conn *w = calloc(1, sizeof *w);
    if (!w) { nstream_close(s); return NULL; }
    w->s = s;
    buf_init(&w->pre);

    /* handshake — no Origin header (codex 403s on it) */
    uint8_t keyraw[16];
    genmask_cb(NULL, keyraw, sizeof keyraw, NULL);
    buf_t key;
    buf_init(&key);
    b64_encode(keyraw, sizeof keyraw, &key);

    buf_t req;
    buf_init(&req);
    buf_appendf(&req, "GET %s HTTP/1.1\r\n", req_path[0] ? req_path : "/");
    buf_appendf(&req, "Host: %s\r\n", host_hdr);
    buf_appends(&req, "Upgrade: websocket\r\nConnection: Upgrade\r\n");
    buf_appendf(&req, "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n", key.data);
    if (bearer && *bearer) buf_appendf(&req, "Authorization: Bearer %s\r\n", bearer);
    buf_appends(&req, "\r\n");
    int wrc = nstream_write_all(s, req.data, req.len);
    buf_free(&req);
    if (wrc != 0) {
        snprintf(err, errlen, "ws handshake write failed");
        buf_free(&key);
        ws_close(w);
        return NULL;
    }

    /* read 101 response */
    buf_t resp;
    buf_init(&resp);
    int64_t deadline = now_ms() + timeout_ms;
    int status = -1;
    size_t hdr_end = 0;
    for (;;) {
        char tmp[4096];
        ssize_t n = nstream_read(s, tmp, sizeof tmp);
        if (n == -2) {
            if (now_ms() > deadline) break;
            struct pollfd pf = {nstream_fd(s), POLLIN, 0};
            tny_poll(&pf, 1, 200);
            continue;
        }
        if (n <= 0) break;
        buf_append(&resp, tmp, (size_t)n);
        int minor;
        const char *msg;
        size_t msg_len;
        struct phr_header hdrs[32];
        size_t nh = 32;
        int pret = phr_parse_response(resp.data, resp.len, &minor, &status,
                                      &msg, &msg_len, hdrs, &nh, 0);
        if (pret > 0) { hdr_end = (size_t)pret; break; }
        if (pret == -1) { status = -1; break; }
    }
    if (status != 101) {
        snprintf(err, errlen, "ws upgrade failed (status %d)", status);
        buf_free(&key);
        buf_free(&resp);
        ws_close(w);
        return NULL;
    }
    buf_free(&key);
    if (resp.len > hdr_end)
        buf_append(&w->pre, resp.data + hdr_end, resp.len - hdr_end);
    buf_free(&resp);

    struct wslay_event_callbacks cbs = {
        recv_cb, send_cb, genmask_cb, NULL, NULL, NULL, on_msg_cb
    };
    if (wslay_event_context_client_init(&w->ctx, &cbs, w) != 0) {
        snprintf(err, errlen, "wslay init failed");
        ws_close(w);
        return NULL;
    }
    return w;
}

int ws_send_text(ws_conn *w, const char *data, size_t len) {
    struct wslay_event_msg msg = {WSLAY_TEXT_FRAME, (const uint8_t *)data, len};
    if (wslay_event_queue_msg(w->ctx, &msg) != 0) return -1;
    return wslay_event_send(w->ctx) == 0 ? 0 : -1;
}

int ws_fd(ws_conn *w) { return nstream_fd(w->s); }

bool ws_want_write(ws_conn *w) {
    return w->ctx && wslay_event_want_write(w->ctx);
}

int ws_pump(ws_conn *w, ws_msg_cb cb, void *ud) {
    if (w->dead) return -1;
    w->cb = cb;
    w->ud = ud;
    if (wslay_event_recv(w->ctx) != 0) { w->dead = true; return -1; }
    if (wslay_event_want_write(w->ctx) && wslay_event_send(w->ctx) != 0) {
        w->dead = true;
        return -1;
    }
    if (!wslay_event_want_read(w->ctx) && !wslay_event_want_write(w->ctx)) {
        w->dead = true;
        return -1;
    }
    return 0;
}

void ws_close(ws_conn *w) {
    if (!w) return;
    if (w->ctx) {
        wslay_event_queue_close(w->ctx, WSLAY_CODE_NORMAL_CLOSURE, NULL, 0);
        wslay_event_send(w->ctx);
        wslay_event_context_free(w->ctx);
    }
    nstream_close(w->s);
    buf_free(&w->pre);
    free(w);
}
