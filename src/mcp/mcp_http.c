/* mcp_http.c — Streamable HTTP MCP transport (docs/adr/0051).
 *
 * One POST per JSON-RPC message. Accepts one application/json response and
 * never issues GET/DELETE or consumes SSE. Modern (2026-07-28)
 * requests carry per-request _meta plus MCP-Protocol-Version / Mcp-Method /
 * Mcp-Name; a 4xx without a recognized modern error falls back to the
 * legacy initialize handshake. Server output is untrusted data. */
#include "mcp/mcp_priv.h"

#include "util/tny_poll.h"

#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void mcp_secret_free(char *s) {
    if (!s) return;
    secure_zero(s, strlen(s));
    free(s);
}

void mcp_header_lines_free(char **headers, size_t nheaders) {
    if (!headers) return;
    for (size_t i = 0; i < nheaders; i++) mcp_secret_free(headers[i]);
    free(headers);
}

void mcp_conf_free(mcp_conf *conf) {
    if (!conf) return;
    if (conf->argv) {
        for (int i = 0; conf->argv[i]; i++) free(conf->argv[i]);
        free(conf->argv);
    }
    free(conf->url);
    mcp_header_lines_free(conf->headers, conf->nheaders);
    memset(conf, 0, sizeof *conf);
}

void mcp_conn_close(mcp_conn *c) {
    if (!c) return;
    if (c->transport == MCP_TRANSPORT_STDIO && c->pid > 0) {
        close(c->in_fd);
        close(c->out_fd);
        kill(c->pid, SIGTERM);
        waitpid(c->pid, NULL, WNOHANG);
    }
    buf_free(&c->rbuf);
    yyjson_doc_free(c->tools);
    free(c->url);
    mcp_header_lines_free(c->headers, c->nheaders);
    mcp_secret_free(c->session_id);
    memset(c, 0, sizeof *c);
}

void mcp_http_status_error(mcp_conn *c, int status) {
    if (status == 401 || status == 403)
        snprintf(c->last_error, sizeof c->last_error,
                 "HTTP %d authentication failed; check configured MCP auth headers", status);
    else snprintf(c->last_error, sizeof c->last_error, "HTTP %d from MCP endpoint", status);
}

static bool content_type_is(const char *value, const char *type) {
    if (!value) return false;
    size_t n = strlen(type);
    return strncasecmp(value, type, n) == 0 &&
           (value[n] == '\0' || value[n] == ';' || isspace((unsigned char)value[n]));
}

static int http_body_collect(http_conn *h, buf_t *body) {
    int64_t deadline = now_ms() + MCP_TIMEOUT_MS;
    for (;;) {
        char tmp[16384];
        ssize_t n = http_body_read(h, tmp, sizeof tmp);
        if (n > 0) {
            if (body->len > MCP_HTTP_MAX_BODY - (size_t)n) return -1;
            buf_append(body, tmp, (size_t)n);
            if (buf_oom(body)) return -1;
            continue;
        }
        if (n == 0) return 0;
        if (n == -1) return -1;
        int left = (int)(deadline - now_ms());
        if (left <= 0) return -1;
        struct pollfd pf = {http_fd(h), POLLIN, 0};
        if (tny_poll(&pf, 1, left) <= 0) return -1;
    }
}

static void append_modern_params(buf_t *out, const char *params_json) {
    const char *params = params_json ? params_json : "{}";
    size_t n = strlen(params);
    if (n >= 2 && params[0] == '{' && params[n - 1] == '}') {
        buf_append(out, params, n - 1);
        if (n > 2) buf_appends(out, ",");
    } else {
        buf_appends(out, "{");
    }
    buf_appends(
        out,
        "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"" MCP_MODERN_VERSION
        "\",\"io.modelcontextprotocol/clientInfo\":{\"name\":\"tny\",\"version\":\"" TNY_VERSION
        "\"},\"io.modelcontextprotocol/clientCapabilities\":{}}}");
}

static bool header_plain_ascii(const char *value) {
    size_t n = strlen(value);
    if (n == 0 || value[0] == ' ' || value[0] == '\t' || value[n - 1] == ' ' ||
        value[n - 1] == '\t')
        return false;
    if (n >= 11 && strncmp(value, "=?base64?", 9) == 0 && strcmp(value + n - 2, "?=") == 0)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (ch < 0x20 || ch > 0x7e) return false;
    }
    return true;
}

static void append_routing_value(buf_t *line, const char *name, const char *value) {
    buf_appendf(line, "%s: ", name);
    if (header_plain_ascii(value)) {
        buf_appends(line, value);
        return;
    }
    buf_appends(line, "=?base64?");
    b64_encode((const uint8_t *)value, strlen(value), line);
    buf_appends(line, "?=");
}

/* One HTTP JSON-RPC message. Returns a parsed response body for both success
 * and HTTP error statuses so era negotiation can inspect standard errors. */
static yyjson_doc *http_message(mcp_conn *c, const char *method, const char *name, const char *body,
                                bool capture_session, bool notification, int *status_out) {
    c->last_error[0] = '\0';
    char neterr[256] = "";
    http_conn *h = http_open(c->url, neterr, sizeof neterr);
    if (!h) {
        snprintf(c->last_error, sizeof c->last_error, "HTTP connection failed: %s",
                 neterr[0] ? neterr : "unknown error");
        return NULL;
    }

    const char *headers[MCP_MAX_HEADERS + 8];
    size_t nh = 0;
    for (size_t i = 0; i < c->nheaders; i++) headers[nh++] = c->headers[i];
    headers[nh++] = "Content-Type: application/json";
    headers[nh++] = "Accept: application/json";
    char version_line[64] = "";
    char method_line[320] = "";
    char session_line[640] = "";
    buf_t name_line;
    buf_init(&name_line);
    if (c->era == MCP_ERA_MODERN) {
        snprintf(version_line, sizeof version_line, "MCP-Protocol-Version: %s",
                 c->protocol_version);
        snprintf(method_line, sizeof method_line, "Mcp-Method: %s", method);
        headers[nh++] = version_line;
        headers[nh++] = method_line;
        if (name) {
            append_routing_value(&name_line, "Mcp-Name", name);
            headers[nh++] = name_line.data;
        }
    } else if (c->protocol_version[0]) {
        snprintf(version_line, sizeof version_line, "MCP-Protocol-Version: %s",
                 c->protocol_version);
        headers[nh++] = version_line;
    }
    if (c->session_id) {
        snprintf(session_line, sizeof session_line, "Mcp-Session-Id: %s", c->session_id);
        headers[nh++] = session_line;
    }
    headers[nh] = NULL;

    const char *path = c->path[0] ? c->path : "/";
    int req_rc = http_request(h, "POST", path, headers, body, strlen(body));
    secure_zero(session_line, sizeof session_line);
    buf_free(&name_line);
    if (req_rc != 0) {
        snprintf(c->last_error, sizeof c->last_error, "HTTP request write failed");
        http_close(h);
        return NULL;
    }
    int status = http_read_response(h, MCP_TIMEOUT_MS);
    if (status < 0) {
        snprintf(c->last_error, sizeof c->last_error, "HTTP response timed out or closed");
        http_close(h);
        return NULL;
    }
    if (status_out) *status_out = status;
    const char *content_type = http_header(h, "Content-Type");
    if (content_type_is(content_type, "text/event-stream")) {
        snprintf(c->last_error, sizeof c->last_error,
                 "unsupported SSE response transport; configure the server to return "
                 "application/json over Streamable HTTP POST");
        http_close(h);
        return NULL;
    }
    if (capture_session) {
        const char *session = http_header(h, "Mcp-Session-Id");
        if (session && *session) {
            bool valid = strlen(session) < sizeof session_line - sizeof "Mcp-Session-Id: ";
            for (const unsigned char *p = (const unsigned char *)session; *p; p++)
                if (*p < 0x21 || *p > 0x7e) valid = false;
            if (!valid) {
                snprintf(c->last_error, sizeof c->last_error,
                         "server returned an invalid or oversized Mcp-Session-Id");
                http_close(h);
                return NULL;
            }
            mcp_secret_free(c->session_id);
            c->session_id = xstrdup(session);
        }
    }
    if (notification) {
        /* Accepted notifications have no response body. This also avoids a
         * 30 s EOF wait when a valid 202 omits Content-Length: 0. */
        http_close(h);
        return NULL;
    }
    buf_t response;
    buf_init(&response);
    if (http_body_collect(h, &response) != 0) {
        snprintf(c->last_error, sizeof c->last_error,
                 "HTTP response body was truncated, too large, or timed out");
        buf_free(&response);
        http_close(h);
        return NULL;
    }
    if (response.len > 0 && !content_type_is(content_type, "application/json")) {
        snprintf(c->last_error, sizeof c->last_error,
                 "HTTP response must be application/json; SSE is unsupported");
        buf_free(&response);
        http_close(h);
        return NULL;
    }
    http_close(h);
    if (response.len == 0) {
        /* Only a success status earns the empty-body diagnostic; on an error
         * status the callers report "HTTP <status>" when last_error is clear. */
        if (status >= 200 && status < 300)
            snprintf(c->last_error, sizeof c->last_error, "HTTP response body was empty");
        buf_free(&response);
        return NULL;
    }
    yyjson_doc *doc = yyjson_read(response.data, response.len, 0);
    buf_free(&response);
    if (!doc)
        snprintf(c->last_error, sizeof c->last_error,
                 "HTTP response was not a single JSON document");
    return doc;
}

yyjson_doc *mcp_rpc_http(mcp_conn *c, const char *method, const char *name, const char *params_json,
                         bool capture_session, int *status_out) {
    int id = ++c->next_id;
    buf_t req;
    buf_init(&req);
    buf_appendf(&req, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":", id, method);
    if (c->era == MCP_ERA_MODERN) append_modern_params(&req, params_json);
    else buf_appends(&req, params_json ? params_json : "{}");
    buf_appends(&req, "}");
    yyjson_doc *doc =
        buf_oom(&req) ? NULL
                      : http_message(c, method, name, req.data, capture_session, false, status_out);
    buf_free(&req);
    if (doc) {
        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *idv = jget(root, "id");
        /* An error the server cannot attribute may carry id:null; keep it so
         * era negotiation can inspect the standard error code. */
        bool null_id_error = jget(root, "error") && (!idv || yyjson_is_null(idv));
        if (!null_id_error && (int)jget_int(root, "id", -1) != id) {
            yyjson_doc_free(doc);
            snprintf(c->last_error, sizeof c->last_error,
                     "HTTP response returned a mismatched JSON-RPC id");
            return NULL;
        }
    }
    return doc;
}

int mcp_notify_http(mcp_conn *c, const char *method) {
    buf_t req;
    buf_init(&req);
    buf_appendf(&req, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":{}}", method);
    int status = 0;
    yyjson_doc *doc = http_message(c, method, NULL, req.data, false, true, &status);
    yyjson_doc_free(doc);
    buf_free(&req);
    if (status < 200 || status >= 300) {
        if (status > 0) mcp_http_status_error(c, status);
        return -1;
    }
    return 0;
}

static bool discover_advertises_modern(yyjson_doc *doc) {
    yyjson_val *versions = jget(jget(yyjson_doc_get_root(doc), "result"), "supportedVersions");
    if (!versions || !yyjson_is_arr(versions)) return false;
    size_t idx, max;
    yyjson_val *v;
    yyjson_arr_foreach(versions, idx, max, v) {
        if (yyjson_is_str(v) && strcmp(yyjson_get_str(v), MCP_MODERN_VERSION) == 0) return true;
    }
    return false;
}

static bool modern_protocol_error(yyjson_doc *doc) {
    yyjson_val *err = doc ? jget(yyjson_doc_get_root(doc), "error") : NULL;
    int code = (int)jget_int(err, "code", 0);
    return code >= -32022 && code <= -32020;
}

int mcp_conn_open_http(mcp_conn *c, mcp_conf *conf) {
    memset(c, 0, sizeof *c);
    c->transport = MCP_TRANSPORT_HTTP;
    c->era = MCP_ERA_MODERN;
    snprintf(c->protocol_version, sizeof c->protocol_version, "%s", MCP_MODERN_VERSION);
    c->url = conf->url;
    conf->url = NULL;
    c->headers = conf->headers;
    c->nheaders = conf->nheaders;
    conf->headers = NULL;
    conf->nheaders = 0;
    url_parts parsed;
    if (url_parse(c->url, &parsed) != 0) return -1;
    snprintf(c->path, sizeof c->path, "%s", parsed.path);

    int status = 0;
    yyjson_doc *discover = mcp_rpc_http(c, "server/discover", NULL, "{}", false, &status);
    if (discover && status >= 200 && status < 300 && discover_advertises_modern(discover)) {
        yyjson_doc_free(discover);
        return 0; /* v2: no initialize, notification, or session round trip */
    }
    if (modern_protocol_error(discover)) {
        yyjson_doc_free(discover);
        snprintf(c->last_error, sizeof c->last_error,
                 "stateless MCP server does not support protocol %s", MCP_MODERN_VERSION);
        return -1;
    }
    if ((status < 200 || status >= 300) && status != 400 && status != 404 && status != 405) {
        yyjson_doc_free(discover);
        if (!c->last_error[0]) mcp_http_status_error(c, status);
        return -1;
    }
    if (!discover && c->last_error[0] && status != 400 && status != 404 && status != 405)
        return -1; /* network, malformed success JSON, or SSE */
    yyjson_doc_free(discover);

    /* Legacy Streamable HTTP: an era-ambiguous failure from discover falls
     * back to the initialize handshake. We still never issue GET. */
    c->era = MCP_ERA_LEGACY;
    c->protocol_version[0] = '\0';
    c->last_error[0] = '\0';
    yyjson_doc *init =
        mcp_rpc_http(c, "initialize", NULL,
                     "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
                     "\"clientInfo\":{\"name\":\"tny\",\"version\":\"" TNY_VERSION "\"}}",
                     true, &status);
    if (!init || status < 200 || status >= 300) {
        yyjson_doc_free(init);
        if (status == 404 || status == 405)
            snprintf(c->last_error, sizeof c->last_error,
                     "unsupported legacy HTTP+SSE transport; configure the Streamable HTTP "
                     "POST endpoint or use a local stdio proxy");
        else if (!c->last_error[0]) mcp_http_status_error(c, status);
        return -1;
    }
    const char *version = jget_str(jget(yyjson_doc_get_root(init), "result"), "protocolVersion");
    if (version && strlen(version) >= sizeof c->protocol_version) {
        yyjson_doc_free(init);
        snprintf(c->last_error, sizeof c->last_error,
                 "server returned an oversized protocolVersion");
        return -1;
    }
    snprintf(c->protocol_version, sizeof c->protocol_version, "%s",
             version && *version ? version : MCP_LEGACY_VERSION);
    yyjson_doc_free(init);
    if (mcp_notify_http(c, "notifications/initialized") != 0) return -1;
    return 0;
}
