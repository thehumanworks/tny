#include "core/tnyjev_internal.h"
#include "json/json.h"
#include "net/net.h"
#include "util/tny_poll.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool text_valid(const char *s, size_t max, bool label) {
    if (!s) return false;
    size_t n = strnlen(s, max + 1);
    if (!n || n > max || !utf8_valid_bytes(s, n)) return false;
    if (label)
        for (size_t i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)s[i];
            if (ch < 32 || ch == 127 ||
                (ch == 0xc2 && (unsigned char)s[i + 1] >= 0x80 && (unsigned char)s[i + 1] <= 0x9f))
                return false;
        }
    return true;
}

static bool append_value(buf_t *out, tnyjev_value value, bool nullable) {
    if (value.type != TNYJEV_TEXT && value.type != TNYJEV_JSON) return false;
    if (!value.data) {
        if (!nullable) return false;
        buf_appends(out, "null");
        return true;
    }
    if (!(nullable && value.type == TNYJEV_TEXT && !*value.data) &&
        !text_valid(value.data, TNYJEV_MAX_BYTES, false))
        return false;
    if (value.type == TNYJEV_TEXT) {
        jescape(out, value.data);
        return true;
    }
    if (value.type != TNYJEV_JSON) return false;
    yyjson_doc *doc = jparse(value.data, strlen(value.data));
    yyjson_val *v = doc ? yyjson_doc_get_root(doc) : NULL;
    bool ok =
        yyjson_is_str(v) || yyjson_is_obj(v) || yyjson_is_arr(v) || (nullable && yyjson_is_null(v));
    if (ok) buf_appends(out, value.data);
    yyjson_doc_free(doc);
    return ok;
}

tnyjev_status tnyjev_encode(const tnyjev_request *r, const char *model, buf_t *body) {
    if (!r || !body || body->len || !text_valid(model, 255, true) ||
        !text_valid(r->instructions, TNYJEV_MAX_BYTES, false))
        return TNYJEV_INVALID;
    if (r->kind == TNYJEV_CHOOSE) {
        if (!r->choices || !r->choice_count || r->choice_count > TNYJEV_MAX_CHOICES)
            return TNYJEV_INVALID;
        for (size_t i = 0; i < r->choice_count; i++) {
            if (!text_valid(r->choices[i].key, 1024, true)) return TNYJEV_INVALID;
            for (size_t j = 0; j < i; j++)
                if (!strcmp(r->choices[i].key, r->choices[j].key)) return TNYJEV_INVALID;
        }
    } else if (r->kind != TNYJEV_SCORE || r->choice_count || r->choices) {
        return TNYJEV_INVALID;
    }
    buf_appends(body, "{\"model\":");
    jescape(body, model);
    buf_appends(body, ",\"state\":");
    if (!append_value(body, r->state, false)) return TNYJEV_INVALID;
    buf_appends(body, ",\"questions\":{\"decision\":{\"type\":");
    jescape(body, r->kind == TNYJEV_SCORE ? "noul" : "choice");
    buf_appends(body, ",\"instructions\":");
    jescape(body, r->instructions);
    if (r->kind == TNYJEV_CHOOSE) {
        buf_appends(body, ",\"criteria\":{");
        for (size_t i = 0; i < r->choice_count; i++) {
            if (i) buf_appends(body, ",");
            jescape(body, r->choices[i].key);
            buf_appends(body, ":");
            if (!append_value(body, r->choices[i].description, true)) return TNYJEV_INVALID;
            if (body->len > TNYJEV_MAX_BYTES) return TNYJEV_INVALID;
        }
        buf_appends(body, "}");
    }
    buf_appends(body, "}}}");
    if (body->oom) return TNYJEV_OOM;
    return body->len <= TNYJEV_MAX_BYTES ? TNYJEV_OK : TNYJEV_INVALID;
}

/* Reject duplicate typed fields, including escaped aliases. Unknown fields
 * remain forward-compatible; never accept embedded-NUL field aliases. */
static yyjson_val *field(yyjson_val *obj, const char *name) {
    if (!yyjson_is_obj(obj)) return NULL;
    yyjson_val *key, *value, *found = NULL;
    size_t i, n;
    yyjson_obj_foreach(obj, i, n, key, value) {
        if (yyjson_get_len(key) == strlen(name) &&
            !memcmp(yyjson_get_str(key), name, strlen(name))) {
            if (found) return NULL;
            found = value;
        }
    }
    return found;
}

static const char *string(yyjson_val *v) {
    const char *s = yyjson_get_str(v);
    return s && strlen(s) == yyjson_get_len(v) ? s : NULL;
}

static bool probability(yyjson_val *v, double *out) {
    if (!yyjson_is_num(v)) return false;
    double p = yyjson_get_num(v);
    if (!isfinite(p) || p < 0 || p > 1) return false;
    *out = p;
    return true;
}

tnyjev_status tnyjev_decode(const tnyjev_request *r, const char *body, size_t len,
                            tnyjev_result *result) {
    if (!result) return TNYJEV_INVALID;
    memset(result, 0, sizeof *result);
    if (!r || !body || !len || len > TNYJEV_MAX_BYTES) return TNYJEV_PROTOCOL;
    yyjson_doc *doc = jparse(body, len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *answer = field(field(root, "answers"), "decision");
    const char *model = string(field(root, "model"));
    const char *type = string(field(answer, "type"));
    yyjson_val *usage = field(root, "usage");
    yyjson_val *in = field(usage, "input_tokens"), *out = field(usage, "output_tokens");
    tnyjev_result parsed = {.kind = r->kind};
    bool ok = text_valid(model, sizeof parsed.model - 1, true) && type && yyjson_is_uint(in) &&
              yyjson_is_uint(out);
    if (!ok) goto done;
    memcpy(parsed.model, model, strlen(model) + 1);
    parsed.input_tokens = yyjson_get_uint(in);
    parsed.output_tokens = yyjson_get_uint(out);
    if (r->kind == TNYJEV_SCORE) {
        ok = !strcmp(type, "noul") && probability(field(answer, "noul"), &parsed.value.score);
    } else if (r->kind == TNYJEV_CHOOSE && r->choices && r->choice_count &&
               r->choice_count <= TNYJEV_MAX_CHOICES) {
        const char *chosen = string(field(answer, "choice"));
        yyjson_val *probs = field(answer, "probabilities");
        ok = !strcmp(type, "choice") && chosen && yyjson_is_obj(probs) &&
             yyjson_obj_size(probs) == r->choice_count &&
             probability(field(answer, "confidence"), &parsed.value.choose.confidence);
        if (!ok) goto done;
        size_t selected = r->choice_count;
        double sum = 0, highest = 0;
        for (size_t i = 0; i < r->choice_count; i++) {
            double p;
            if (!probability(field(probs, r->choices[i].key), &p)) {
                ok = false;
                goto done;
            }
            parsed.value.choose.probabilities[i] = p;
            sum += p;
            if (p > highest) highest = p;
            if (!strcmp(chosen, r->choices[i].key)) selected = i;
        }
        if (selected == r->choice_count) {
            ok = false;
            goto done;
        }
        ok = fabs(sum - 1.0) <= 0.00001;
        if (ok) ok = parsed.value.choose.probabilities[selected] + 1e-9 >= highest;
        parsed.value.choose.choice_index = selected;
        parsed.value.choose.count = r->choice_count;
    } else {
        ok = false;
    }
done:
    yyjson_doc_free(doc);
    if (ok) *result = parsed;
    return ok ? TNYJEV_OK : TNYJEV_PROTOCOL;
}

static bool endpoint_valid(const char *url) {
    if (!text_valid(url, 1200, true) || strpbrk(url, " \t\r\n@?#\\")) return false;
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) return false;
    const char *authority = scheme_end + 3;
    const char *path = strchr(authority, '/');
    const char *colon = strchr(authority, ':');
    if (colon && (!path || colon < path)) {
        size_t digits = strspn(colon + 1, "0123456789");
        const char *end = colon + 1 + digits;
        if (!digits || digits > 5 || (path ? end != path : *end != '\0')) return false;
        unsigned long port = strtoul(colon + 1, NULL, 10);
        if (!port || port > 65535) return false;
    }
    url_parts parts;
    if (url_parse(url, &parts)) return false;
    if (path && strlen(path) >= sizeof parts.path) return false;
    size_t hostlen =
        strspn(parts.host, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-");
    if (hostlen != strlen(parts.host)) return false;
    return !strcmp(parts.scheme, "https") ||
           (!strcmp(parts.scheme, "http") &&
            (!strcmp(parts.host, "127.0.0.1") || !strcmp(parts.host, "localhost")));
}

static bool cancelled(const tnyjev_config *c) { return c->cancelled && c->cancelled(c->userdata); }

static const char *status_text(tnyjev_status status) {
    switch (status) {
    case TNYJEV_OK: return "";
    case TNYJEV_INVALID: return "invalid Jev input, endpoint or limits (see tny score --help)";
    case TNYJEV_AUTH: return "set a valid TYPESAFE_API_KEY to use Jev";
    case TNYJEV_TRANSPORT: return "Jev transport failed";
    case TNYJEV_HTTP: return "Jev HTTP request failed";
    case TNYJEV_PROTOCOL: return "invalid, incomplete or oversized Jev response";
    case TNYJEV_TIMEOUT: return "Jev response timed out";
    case TNYJEV_CANCELLED: return "Jev request interrupted";
    case TNYJEV_OOM: return "out of memory";
    }
    return "Jev request failed";
}

tnyjev_status tnyjev_evaluate(const tnyjev_config *c, const tnyjev_request *r,
                              tnyjev_result *result, char *err, size_t errlen) {
    tnyjev_status status = TNYJEV_INVALID;
    buf_t body = {0}, auth = {0}, response = {0};
    http_conn *conn = NULL;
    int http_status = 0;
    if (err && errlen) *err = '\0';
    if (result) memset(result, 0, sizeof *result);
    if (!c || !r || !result || c->timeout_ms > 300000) goto done;
    const char *url = c->url ? c->url : TNYJEV_DEFAULT_URL;
    const char *model = c->model ? c->model : TNYJEV_DEFAULT_MODEL;
    if (!endpoint_valid(url)) goto done;
    status = TNYJEV_AUTH;
    if (!text_valid(c->api_key, 8192, true)) goto done;
    for (const unsigned char *p = (const unsigned char *)c->api_key; *p; p++)
        if (*p < 33 || *p > 126) goto done;
    status = tnyjev_encode(r, model, &body);
    if (status != TNYJEV_OK) goto done;
    if (cancelled(c)) {
        status = TNYJEV_CANCELLED;
        goto done;
    }
    buf_appends(&auth, "Authorization: Bearer ");
    buf_appends(&auth, c->api_key);
    status = TNYJEV_OOM;
    if (auth.oom) goto done;
    const char *headers[] = {auth.data, "Content-Type: application/json",
                             "Accept: application/json", NULL};
    char transport_error[256] = "";
    status = TNYJEV_TRANSPORT;
    conn = http_open(url, transport_error, sizeof transport_error);
    if (!conn || http_request(conn, "POST", http_prefix(conn), headers, body.data, body.len))
        goto done;
    int64_t deadline = monotonic_ms() + (c->timeout_ms ? c->timeout_ms : 60000);
    bool have_headers = false;
    for (;;) {
        if (cancelled(c)) {
            status = TNYJEV_CANCELLED;
            break;
        }
        int64_t remaining = deadline - monotonic_ms();
        if (remaining <= 0) {
            status = TNYJEV_TIMEOUT;
            break;
        }
        if (!have_headers) {
            http_status = http_read_response(conn, 0);
            if (http_status != -2) {
                if (http_status != 200) {
                    status = http_status > 0 ? TNYJEV_HTTP : TNYJEV_TRANSPORT;
                    break;
                }
                const char *ct = http_header(conn, "Content-Type");
                if (!ct || strncmp(ct, "application/json", 16) != 0 ||
                    (ct[16] && ct[16] != ';' && ct[16] != ' ')) {
                    status = TNYJEV_PROTOCOL;
                    break;
                }
                have_headers = true;
            }
        }
        if (have_headers) {
            char chunk[8192];
            ssize_t n = http_body_read(conn, chunk, sizeof chunk);
            if (n == 0) {
                status = tnyjev_decode(r, response.data, response.len, result);
                break;
            }
            if (n == -1 || (n > 0 && (size_t)n > TNYJEV_MAX_BYTES - response.len)) {
                status = TNYJEV_PROTOCOL;
                break;
            }
            if (n > 0) {
                buf_append(&response, chunk, (size_t)n);
                if (response.oom) {
                    status = TNYJEV_OOM;
                    break;
                }
                continue;
            }
        }
        struct pollfd fd = {.fd = http_fd(conn), .events = POLLIN};
        if (tny_poll(&fd, 1, remaining > 50 ? 50 : (int)remaining) < 0 && errno != EINTR) {
            status = TNYJEV_TRANSPORT;
            break;
        }
    }
done:
    http_close(conn);
    if (auth.data) secure_zero(auth.data, auth.len);
    if (body.data) secure_zero(body.data, body.len);
    if (response.data) secure_zero(response.data, response.len);
    buf_free(&auth);
    buf_free(&body);
    buf_free(&response);
    if (err && errlen) {
        if (status == TNYJEV_HTTP)
            snprintf(err, errlen, "Jev HTTP %d%s", http_status,
                     http_status == 401 || http_status == 403   ? "; check TYPESAFE_API_KEY"
                     : http_status == 429 || http_status == 529 ? "; retry later"
                                                                : "");
        else snprintf(err, errlen, "%s", status_text(status));
    }
    return status;
}
