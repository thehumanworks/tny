/* acp_wire.c — JSONL framing + JSON-RPC message builders (docs/backends/acp.md). */
#include "backends/acp/acp_wire.h"
#include "util/tny_poll.h"
#include "util/alloc.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

void acp_reader_init(acp_reader *r) {
    buf_init(&r->buf);
    r->overflow = false;
}

void acp_reader_free(acp_reader *r) { buf_free(&r->buf); }

void acp_reader_feed(acp_reader *r, const char *data, size_t n) {
    if (r->overflow || tny_alloc_scope_failed()) return;
    if (n > ACP_MAX_MSG + 16384 || r->buf.len > ACP_MAX_MSG + 16384 - n) {
        r->overflow = true;
        buf_clear(&r->buf);
        return;
    }
    buf_append(&r->buf, data, n);
    if (r->buf.len > ACP_MAX_MSG && !memchr(r->buf.data, '\n', r->buf.len)) {
        r->overflow = true;
        buf_clear(&r->buf);
    }
}

char *acp_reader_next(acp_reader *r, size_t *len_out) {
    if (r->overflow || !r->buf.len || tny_alloc_scope_failed()) return NULL;
    char *nl = memchr(r->buf.data, '\n', r->buf.len);
    if (!nl) return NULL;
    size_t n = (size_t)(nl - r->buf.data);
    if (n > ACP_MAX_MSG) {
        r->overflow = true;
        buf_clear(&r->buf);
        return NULL;
    }
    size_t trimmed = n;
    while (trimmed && (r->buf.data[trimmed - 1] == '\r')) trimmed--;
    char *line = xstrndup(r->buf.data, trimmed);
    buf_consume(&r->buf, n + 1);
    if (len_out) *len_out = trimmed;
    return line;
}

int acp_write_line(int fd, const char *json, size_t len) {
    if (fd < 0 || len > ACP_MAX_MSG || tny_alloc_scope_failed()) return -1;
    buf_t out;
    buf_init(&out);
    buf_append(&out, json, len);
    buf_append(&out, "\n", 1);
    if (out.oom) { /* never write a truncated line as success */
        buf_free(&out);
        return -1;
    }
    int64_t deadline = monotonic_ms() + 5000;
    size_t off = 0;
    int rc = 0;
    while (off < out.len) {
        int64_t remaining = deadline - monotonic_ms();
        if (remaining <= 0) {
            rc = -1;
            break;
        }
        ssize_t w = write(fd, out.data + off, out.len - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = {fd, POLLOUT, 0};
            if (tny_poll(&p, 1, (int)remaining) <= 0) {
                rc = -1;
                break;
            }
            continue;
        }
        rc = -1;
        break;
    }
    buf_free(&out);
    return rc;
}

void acp_fmt_request(buf_t *b, int64_t id, const char *method, const char *params_json) {
    buf_appendf(b, "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"method\":", (long long)id);
    jescape(b, method);
    buf_appendf(b, ",\"params\":%s}", params_json ? params_json : "{}");
}

void acp_fmt_notify(buf_t *b, const char *method, const char *params_json) {
    buf_appends(b, "{\"jsonrpc\":\"2.0\",\"method\":");
    jescape(b, method);
    buf_appendf(b, ",\"params\":%s}", params_json ? params_json : "{}");
}

void acp_fmt_result(buf_t *b, const char *id_raw, const char *result_json) {
    buf_appendf(b, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id_raw ? id_raw : "null",
                result_json ? result_json : "null");
}

char *acp_id_text(yyjson_val *msg) {
    if (tny_alloc_scope_failed()) return NULL;
    yyjson_val *id = jget(msg, "id");
    if (!id) return xstrdup("null");
    char *raw = jwrite_val(id);
    return raw || tny_alloc_scope_failed() ? raw : xstrdup("null");
}

int64_t acp_id_num(yyjson_val *msg) {
    yyjson_val *id = jget(msg, "id");
    if (!id || !yyjson_is_int(id)) return -1;
    return yyjson_get_sint(id);
}

void acp_append_text_block(buf_t *b, const char *text, size_t len) {
    buf_appends(b, "{\"type\":\"text\",\"text\":");
    if (tny_alloc_scope_failed()) return;
    char *tmp = xstrndup(text, len);
    if (!tmp) return;
    jescape(b, tmp);
    free(tmp);
    buf_appends(b, "}");
}
