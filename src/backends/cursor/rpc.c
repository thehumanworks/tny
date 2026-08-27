/* rpc.c — Connect over HTTP/1.1 with the JSON codec
 * (docs/backends/cursor-bridge.md, "Wire format").
 *   unary  : Content-Type application/json, bare JSON in and out
 *   stream : Content-Type application/connect+json, 5-byte enveloped frames
 * Unary calls are short and loopback-only, so they block with a deadline;
 * the Send stream is fully non-blocking and lives in the caller's poll loop. */
#include "backends/cursor/cursor.h"
#include "util/tny_poll.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNARY_TIMEOUT_MS 30000

void cursor_error_line(const char *body, size_t len, const char *fallback, char *out,
                       size_t outlen) {
    yyjson_doc *doc = len ? jparse(body, len) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *e = jget(root, "error");
    if (!e) e = root; /* unary Connect errors are the bare error object */
    const char *msg = jget_str(e, "message");
    const char *code = jget_str(e, "code");
    if (msg && *msg) snprintf(out, outlen, "%s%s%.200s", code ? code : "", code ? ": " : "", msg);
    else if (code && *code) snprintf(out, outlen, "%.100s", code);
    else snprintf(out, outlen, "%s", fallback);
    yyjson_doc_free(doc);
}

static void auth_headers(const char *token, const char *ctype, buf_t *auth, const char *hdrs[5]) {
    buf_appendf(auth, "Authorization: Bearer %s", token);
    hdrs[0] = ctype;
    hdrs[1] = "Connect-Protocol-Version: 1";
    hdrs[2] = auth->data;
    hdrs[3] = "Accept-Encoding: identity";
    hdrs[4] = NULL;
}

/* ---- unary ---- */

void cursor_rpc_init(cursor_rpc *r, const char *base_url, const char *token) {
    memset(r, 0, sizeof *r);
    snprintf(r->base_url, sizeof r->base_url, "%s", base_url);
    snprintf(r->token, sizeof r->token, "%s", token);
}

void cursor_rpc_close(cursor_rpc *r) {
    if (r->conn) {
        http_close(r->conn);
        r->conn = NULL;
    }
}

/* Read the whole response body with a deadline. -1 on error. */
static int read_body(http_conn *c, buf_t *out, int64_t deadline, char *err, size_t errlen) {
    for (;;) {
        char tmp[8192];
        ssize_t n = http_body_read(c, tmp, sizeof tmp);
        if (n == 0) return 0;
        if (n == -1) {
            snprintf(err, errlen, "bridge closed the connection mid-response");
            return -1;
        }
        if (n == -2) {
            int left = (int)(deadline - now_ms());
            if (left <= 0) {
                snprintf(err, errlen, "bridge response timed out");
                return -1;
            }
            struct pollfd pf = {http_fd(c), POLLIN, 0};
            tny_poll(&pf, 1, left > 200 ? 200 : left);
            continue;
        }
        if (out->len + (size_t)n > CURSOR_MAX_MSG_BYTES) {
            snprintf(err, errlen, "bridge response exceeds %u bytes", CURSOR_MAX_MSG_BYTES);
            return -1;
        }
        buf_append(out, tmp, (size_t)n);
    }
}

char *cursor_rpc_unary(cursor_rpc *r, const char *service, const char *method, const char *body,
                       int timeout_ms, char *err, size_t errlen) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s", service, method);
    buf_t auth;
    buf_init(&auth);
    const char *hdrs[5];
    auth_headers(r->token, "Content-Type: application/json", &auth, hdrs);

    int rc = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!r->conn) {
            char oerr[256];
            r->conn = http_open(r->base_url, oerr, sizeof oerr);
            if (!r->conn) {
                snprintf(err, errlen, "cannot reach the bridge at %s: %s", r->base_url, oerr);
                buf_free(&auth);
                return NULL;
            }
        }
        rc = http_request(r->conn, "POST", path, hdrs, body, strlen(body));
        if (rc == 0) break;
        cursor_rpc_close(r); /* stale keep-alive: reopen once */
    }
    buf_free(&auth);
    if (rc != 0) {
        snprintf(err, errlen, "%s failed: cannot write to the bridge", method);
        return NULL;
    }

    int64_t deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : UNARY_TIMEOUT_MS);
    int status;
    for (;;) {
        int left = (int)(deadline - now_ms());
        status = http_read_response(r->conn, left > 0 ? left : 0);
        if (status != -2) break;
        if (left <= 0) {
            snprintf(err, errlen, "%s timed out", method);
            cursor_rpc_close(r);
            return NULL;
        }
    }
    if (status < 0) {
        snprintf(err, errlen, "%s failed: bridge closed the connection", method);
        cursor_rpc_close(r);
        return NULL;
    }

    buf_t out;
    buf_init(&out);
    if (read_body(r->conn, &out, deadline, err, errlen) != 0) {
        buf_free(&out);
        cursor_rpc_close(r);
        return NULL;
    }
    if (status != 200) {
        char detail[300];
        char fallback[64];
        snprintf(fallback, sizeof fallback, "HTTP %d", status);
        cursor_error_line(out.data ? out.data : "", out.len, fallback, detail, sizeof detail);
        if (status == 401 || status == 403)
            snprintf(err, errlen, "%s rejected: bridge auth failed (%s)", method, detail);
        else snprintf(err, errlen, "%s failed: %s", method, detail);
        buf_free(&out);
        return NULL;
    }
    if (!out.data) buf_appends(&out, "{}");
    return buf_detach(&out);
}

/* ---- server stream ---- */

void cursor_stream_init(cursor_stream *s, const char *base_url, const char *token) {
    memset(s, 0, sizeof *s);
    s->state = CS_IDLE;
    snprintf(s->base_url, sizeof s->base_url, "%s", base_url);
    snprintf(s->token, sizeof s->token, "%s", token);
    connect_decoder_init(&s->dec);
}

void cursor_stream_stop(cursor_stream *s) {
    if (s->conn) {
        http_close(s->conn);
        s->conn = NULL;
    }
    connect_decoder_free(&s->dec);
    connect_decoder_init(&s->dec);
    s->state = CS_IDLE;
}

int cursor_stream_fd(cursor_stream *s) { return s->conn ? http_fd(s->conn) : -1; }

int cursor_stream_start(cursor_stream *s, const char *service, const char *method, const char *body,
                        char *err, size_t errlen) {
    cursor_stream_stop(s);
    char oerr[256];
    s->conn = http_open(s->base_url, oerr, sizeof oerr);
    if (!s->conn) {
        snprintf(err, errlen, "cannot reach the bridge at %s: %s", s->base_url, oerr);
        return -1;
    }
    char path[256];
    snprintf(path, sizeof path, "%s/%s", service, method);
    buf_t auth;
    buf_init(&auth);
    const char *hdrs[5];
    auth_headers(s->token, "Content-Type: application/connect+json", &auth, hdrs);

    buf_t framed;
    buf_init(&framed);
    connect_frame_encode(&framed, 0, body, strlen(body));
    int rc = http_request(s->conn, "POST", path, hdrs, framed.data, framed.len);
    buf_free(&framed);
    buf_free(&auth);
    if (rc != 0) {
        snprintf(err, errlen, "%s failed: cannot write to the bridge", method);
        cursor_stream_stop(s);
        return -1;
    }
    s->state = CS_HEADERS;
    return 0;
}

int cursor_stream_pump(cursor_stream *s, connect_frame_cb cb, void *ud, char *err, size_t errlen) {
    if (s->state == CS_IDLE || !s->conn) return 1;

    if (s->state == CS_HEADERS) {
        int status = http_read_response(s->conn, 0);
        if (status == -2) return 0;
        if (status < 0) {
            snprintf(err, errlen, "bridge closed the connection before responding");
            return -1;
        }
        if (status != 200) {
            buf_t body;
            buf_init(&body);
            read_body(s->conn, &body, now_ms() + 2000, err, errlen);
            char detail[300];
            char fallback[64];
            snprintf(fallback, sizeof fallback, "HTTP %d", status);
            cursor_error_line(body.data ? body.data : "", body.len, fallback, detail,
                              sizeof detail);
            if (status == 401 || status == 403)
                snprintf(err, errlen, "bridge auth failed (%s)", detail);
            else snprintf(err, errlen, "bridge stream failed: %s", detail);
            buf_free(&body);
            return -1;
        }
        s->state = CS_BODY;
    }

    for (;;) {
        char tmp[16384];
        ssize_t n = http_body_read(s->conn, tmp, sizeof tmp);
        if (n == -2) return 0;
        if (n == 0) return 1;
        if (n < 0) {
            snprintf(err, errlen, "bridge stream aborted mid-response");
            return -1;
        }
        if (connect_decoder_feed(&s->dec, tmp, (size_t)n, cb, ud) != 0) {
            snprintf(err, errlen, "bridge sent an oversized stream frame");
            return -1;
        }
    }
}
