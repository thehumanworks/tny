/* acp_wire.c — JSONL framing + JSON-RPC message builders (docs/backends/acp.md). */
#include "backends/acp/acp_wire.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

void acp_reader_init(acp_reader *r) {
    buf_init(&r->buf);
    r->overflow = false;
}

void acp_reader_free(acp_reader *r) { buf_free(&r->buf); }

void acp_reader_feed(acp_reader *r, const char *data, size_t n) {
    if (r->overflow) return;
    buf_append(&r->buf, data, n);
    if (r->buf.len > ACP_MAX_MSG && !memchr(r->buf.data, '\n', r->buf.len)) {
        r->overflow = true;
        buf_clear(&r->buf);
    }
}

char *acp_reader_next(acp_reader *r, size_t *len_out) {
    if (r->overflow || !r->buf.len) return NULL;
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
    if (fd < 0) return -1;
    buf_t out;
    buf_init(&out);
    buf_append(&out, json, len);
    buf_append(&out, "\n", 1);
    size_t off = 0;
    int rc = 0;
    while (off < out.len) {
        ssize_t w = write(fd, out.data + off, out.len - off);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = {fd, POLLOUT, 0};
            if (poll(&p, 1, 5000) <= 0) { rc = -1; break; }
            continue;
        }
        rc = -1;
        break;
    }
    buf_free(&out);
    return rc;
}

int acp_send_request(int fd, int64_t id, const char *method, const char *params_json) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"method\":", (long long)id);
    jescape(&b, method);
    buf_appendf(&b, ",\"params\":%s}", params_json ? params_json : "{}");
    int rc = acp_write_line(fd, b.data, b.len);
    buf_free(&b);
    return rc;
}

int acp_send_notify(int fd, const char *method, const char *params_json) {
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"jsonrpc\":\"2.0\",\"method\":");
    jescape(&b, method);
    buf_appendf(&b, ",\"params\":%s}", params_json ? params_json : "{}");
    int rc = acp_write_line(fd, b.data, b.len);
    buf_free(&b);
    return rc;
}

int acp_send_result(int fd, const char *id_raw, const char *result_json) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}",
                id_raw ? id_raw : "null", result_json ? result_json : "null");
    int rc = acp_write_line(fd, b.data, b.len);
    buf_free(&b);
    return rc;
}

int acp_send_error(int fd, const char *id_raw, int code, const char *msg) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":",
                id_raw ? id_raw : "null", code);
    jescape(&b, msg ? msg : "error");
    buf_appends(&b, "}}");
    int rc = acp_write_line(fd, b.data, b.len);
    buf_free(&b);
    return rc;
}

char *acp_id_text(yyjson_val *msg) {
    yyjson_val *id = jget(msg, "id");
    if (!id) return xstrdup("null");
    if (yyjson_is_int(id) || yyjson_is_uint(id) || yyjson_is_sint(id)) {
        char tmp[32];
        snprintf(tmp, sizeof tmp, "%lld", (long long)yyjson_get_sint(id));
        return xstrdup(tmp);
    }
    if (yyjson_is_str(id)) {
        buf_t b;
        buf_init(&b);
        jescape(&b, yyjson_get_str(id));
        return buf_detach(&b);
    }
    char *raw = jwrite_val(id);
    return raw ? raw : xstrdup("null");
}

int64_t acp_id_num(yyjson_val *msg) {
    yyjson_val *id = jget(msg, "id");
    if (!id || !yyjson_is_num(id)) return -1;
    return yyjson_get_sint(id);
}

/* Text of one ContentBlock, or NULL when the block carries no plain text. */
static const char *block_text(yyjson_val *blk, const char **bad) {
    const char *type = jget_str(blk, "type");
    if (!type) type = "text";
    if (strcmp(type, "text") == 0) return jget_str(blk, "text");
    if (strcmp(type, "resource_link") == 0) return jget_str(blk, "uri");
    if (strcmp(type, "resource") == 0) {
        yyjson_val *res = jget(blk, "resource");
        const char *t = jget_str(res, "text");
        if (t) return t;
        *bad = "resource(blob)";
        return NULL;
    }
    *bad = type;
    return NULL;
}

bool acp_blocks_to_text(yyjson_val *arr, buf_t *out, const char **bad) {
    *bad = NULL;
    if (!arr || !yyjson_is_arr(arr)) return true;
    size_t idx, max;
    yyjson_val *blk;
    yyjson_arr_foreach(arr, idx, max, blk) {
        if (!yyjson_is_obj(blk)) continue;
        const char *why = NULL;
        const char *t = block_text(blk, &why);
        if (!t) {
            if (why) { *bad = why; return false; }
            continue;
        }
        if (out->len) buf_appends(out, "\n");
        buf_appends(out, t);
    }
    return true;
}

void acp_append_text_block(buf_t *b, const char *text, size_t len) {
    buf_appends(b, "{\"type\":\"text\",\"text\":");
    char *tmp = xstrndup(text, len);
    jescape(b, tmp);
    free(tmp);
    buf_appends(b, "}");
}
