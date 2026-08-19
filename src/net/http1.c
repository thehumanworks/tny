/* http1.c — minimal HTTP/1.1 client over nstream with chunked decoding. */
#include "net/net.h"
#include "picohttpparser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <poll.h>

#define MAX_HEADERS 64

/* local helpers: avoid nonstandard memmem/strcasestr */
static char *find_seq(char *hay, size_t hlen, const char *needle, size_t nlen) {
    if (hlen < nlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0) return hay + i;
    return NULL;
}

static bool header_has_token(const char *value, const char *token) {
    size_t tl = strlen(token);
    for (const char *p = value; *p; p++)
        if (strncasecmp(p, token, tl) == 0) return true;
    return false;
}

typedef enum { BODY_NONE, BODY_LENGTH, BODY_CHUNKED, BODY_EOF } body_mode;

struct http_conn {
    nstream *s;
    url_parts base;
    bool tls;
    char prefix[1024];

    buf_t in;               /* raw unparsed input */
    /* response state */
    char  hdr_names[MAX_HEADERS][64];
    char  hdr_values[MAX_HEADERS][512];
    int   n_hdrs;
    body_mode mode;
    size_t body_left;       /* BODY_LENGTH: bytes remaining */
    size_t chunk_left;      /* BODY_CHUNKED: bytes left in current chunk */
    bool  chunk_final;      /* saw 0-size chunk */
    bool  body_done;
};

const char *http_prefix(http_conn *c) { return c->prefix; }
int http_fd(http_conn *c) { return nstream_fd(c->s); }

http_conn *http_open(const char *base_url, char *err, size_t errlen) {
    url_parts u;
    if (url_parse(base_url, &u) != 0) {
        snprintf(err, errlen, "bad base URL: %s", base_url);
        return NULL;
    }
    bool tls;
    if (strcmp(u.scheme, "https") == 0) tls = true;
    else if (strcmp(u.scheme, "http") == 0) tls = false;
    else { snprintf(err, errlen, "unsupported scheme %s", u.scheme); return NULL; }

    nstream *s = nstream_connect(u.host, u.port, tls, 15000, err, errlen);
    if (!s) return NULL;
    http_conn *c = calloc(1, sizeof *c);
    if (!c) { nstream_close(s); return NULL; }
    c->s = s;
    c->base = u;
    c->tls = tls;
    /* strip trailing slash from prefix */
    snprintf(c->prefix, sizeof c->prefix, "%s", u.path);
    size_t pl = strlen(c->prefix);
    if (pl && c->prefix[pl - 1] == '/') c->prefix[pl - 1] = 0;
    if (strcmp(c->prefix, "/") == 0) c->prefix[0] = 0;
    buf_init(&c->in);
    return c;
}

int http_request(http_conn *c, const char *method, const char *path,
                 const char **headers, const char *body, size_t body_len) {
    buf_t req;
    buf_init(&req);
    buf_appendf(&req, "%s %s HTTP/1.1\r\n", method, path);
    buf_appendf(&req, "Host: %s\r\n", c->base.host);
    buf_appends(&req, "Connection: keep-alive\r\n");
    buf_appends(&req, "User-Agent: tny/" "0.1.0" "\r\n");
    if (headers)
        for (int i = 0; headers[i]; i++) buf_appendf(&req, "%s\r\n", headers[i]);
    if (body) buf_appendf(&req, "Content-Length: %zu\r\n", body_len);
    buf_appends(&req, "\r\n");
    if (body) buf_append(&req, body, body_len);
    int rc = nstream_write_all(c->s, req.data, req.len);
    buf_free(&req);
    /* reset response state */
    c->n_hdrs = 0;
    c->mode = BODY_NONE;
    c->body_left = c->chunk_left = 0;
    c->chunk_final = false;
    c->body_done = false;
    return rc;
}

/* 1 got data, 0 EOF, -1 error, -2 would-block (after timeout_ms). */
static int fill(http_conn *c, int timeout_ms) {
    char tmp[16384];
    ssize_t n = nstream_read(c->s, tmp, sizeof tmp);
    if (n == -2) {
        if (timeout_ms == 0) return -2;
        struct pollfd pf = {nstream_fd(c->s), POLLIN, 0};
        int pr = poll(&pf, 1, timeout_ms);
        if (pr <= 0) return -2;
        n = nstream_read(c->s, tmp, sizeof tmp);
        if (n == -2) return -2;
    }
    if (n == 0) return 0;
    if (n < 0) return -1;
    buf_append(&c->in, tmp, (size_t)n);
    return 1;
}

int http_read_response(http_conn *c, int timeout_ms) {
    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        int minor;
        int status;
        const char *msg;
        size_t msg_len;
        struct phr_header hdrs[MAX_HEADERS];
        size_t n_hdrs = MAX_HEADERS;
        int pret = phr_parse_response(c->in.data, c->in.len, &minor, &status,
                                      &msg, &msg_len, hdrs, &n_hdrs, 0);
        if (pret > 0) {
            c->n_hdrs = (int)n_hdrs;
            c->mode = BODY_EOF;
            for (size_t i = 0; i < n_hdrs; i++) {
                size_t nl = hdrs[i].name_len < 63 ? hdrs[i].name_len : 63;
                size_t vl = hdrs[i].value_len < 511 ? hdrs[i].value_len : 511;
                memcpy(c->hdr_names[i], hdrs[i].name, nl);
                c->hdr_names[i][nl] = 0;
                memcpy(c->hdr_values[i], hdrs[i].value, vl);
                c->hdr_values[i][vl] = 0;
            }
            const char *te = http_header(c, "Transfer-Encoding");
            const char *cl = http_header(c, "Content-Length");
            if (te && header_has_token(te, "chunked")) c->mode = BODY_CHUNKED;
            else if (cl) {
                c->mode = BODY_LENGTH;
                c->body_left = (size_t)strtoull(cl, NULL, 10);
                if (c->body_left == 0) c->body_done = true;
            }
            if (status == 204 || status == 304) { c->mode = BODY_NONE; c->body_done = true; }
            buf_consume(&c->in, (size_t)pret);
            return status;
        }
        if (pret == -1) return -1;
        int left = (int)(deadline - now_ms());
        if (left < 0) left = 0;
        int fr = fill(c, left);
        if (fr == 0 || fr == -1) return -1; /* EOF or error before headers */
        if (fr == -2) return -2;            /* incomplete; retry later */
    }
}

const char *http_header(http_conn *c, const char *name) {
    for (int i = 0; i < c->n_hdrs; i++)
        if (strcasecmp(c->hdr_names[i], name) == 0) return c->hdr_values[i];
    return NULL;
}

/* Decode chunked framing from c->in into out. */
static ssize_t chunked_read(http_conn *c, char *out, size_t cap) {
    size_t produced = 0;
    for (;;) {
        if (c->chunk_final) {
            /* after the 0-chunk line: either "\r\n" (no trailers) or
             * trailer lines ending with a blank line */
            if (c->in.len >= 2 && memcmp(c->in.data, "\r\n", 2) == 0) {
                buf_consume(&c->in, 2);
                c->body_done = true;
                return (ssize_t)produced;
            }
            if (c->in.len >= 4) {
                char *end = find_seq(c->in.data, c->in.len, "\r\n\r\n", 4);
                if (end) {
                    buf_consume(&c->in, (size_t)(end - c->in.data) + 4);
                    c->body_done = true;
                }
            }
            return (ssize_t)produced; /* need more data if not done */
        }
        if (c->chunk_left == 0) {
            if (c->in.len == 0) return (ssize_t)produced;
            char *nl = memchr(c->in.data, '\n', c->in.len);
            if (!nl) return (ssize_t)produced;
            size_t linelen = (size_t)(nl - c->in.data) + 1;
            unsigned long sz = strtoul(c->in.data, NULL, 16);
            buf_consume(&c->in, linelen);
            if (sz == 0) { c->chunk_final = true; continue; }
            c->chunk_left = sz;
        }
        if (c->in.len == 0) return (ssize_t)produced;
        size_t take = c->chunk_left < c->in.len ? c->chunk_left : c->in.len;
        if (take > cap - produced) take = cap - produced;
        if (take == 0) return (ssize_t)produced;
        memcpy(out + produced, c->in.data, take);
        produced += take;
        c->chunk_left -= take;
        buf_consume(&c->in, take);
        if (c->chunk_left == 0) {
            /* trailing CRLF after chunk data */
            if (c->in.len >= 2) buf_consume(&c->in, 2);
            else if (c->in.len == 1 && c->in.data[0] == '\r') buf_consume(&c->in, 1);
        }
        if (produced == cap) return (ssize_t)produced;
    }
}

ssize_t http_body_read(http_conn *c, char *out, size_t cap) {
    if (c->body_done) return 0;
    for (;;) {
        ssize_t got = 0;
        if (c->mode == BODY_CHUNKED) {
            got = chunked_read(c, out, cap);
            if (got > 0) return got;
            if (c->body_done) return 0;
        } else if (c->mode == BODY_LENGTH) {
            size_t take = c->body_left < c->in.len ? c->body_left : c->in.len;
            if (take > cap) take = cap;
            if (take > 0) {
                memcpy(out, c->in.data, take);
                buf_consume(&c->in, take);
                c->body_left -= take;
                if (c->body_left == 0) c->body_done = true;
                return (ssize_t)take;
            }
        } else { /* BODY_EOF */
            size_t take = c->in.len < cap ? c->in.len : cap;
            if (take > 0) {
                memcpy(out, c->in.data, take);
                buf_consume(&c->in, take);
                return (ssize_t)take;
            }
        }
        int fr = fill(c, 0);
        if (fr == -2) return -2; /* nothing buffered right now */
        if (fr == 0) {           /* EOF */
            if (c->mode == BODY_EOF) { c->body_done = true; return 0; }
            return -1;           /* truncated chunked / length body */
        }
        if (fr == -1) return -1;
    }
}

void http_close(http_conn *c) {
    if (!c) return;
    nstream_close(c->s);
    buf_free(&c->in);
    free(c);
}
