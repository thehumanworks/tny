/* sdk_error.c -- Connect JSON + minimal protobuf SdkErrorDetails decoder. */
#include "backends/cursor/sdk_error.h"

#include "json/json.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} wire_reader;

static void replace_string(char **dst, const uint8_t *src, size_t len) {
    char *next = xstrndup((const char *)src, len);
    if (!next) return;
    free(*dst);
    *dst = next;
}

void cursor_sdk_error_init(cursor_sdk_error *error) {
    if (error) memset(error, 0, sizeof *error);
}

void cursor_sdk_error_free(cursor_sdk_error *error) {
    if (!error) return;
    free(error->message);
    free(error->request_id);
    free(error->help_url);
    free(error->provider);
    memset(error, 0, sizeof *error);
}

static bool wire_varint(wire_reader *r, uint64_t *out) {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64 && r->p < r->end; shift += 7) {
        uint8_t byte = *r->p++;
        if (shift == 63 && (byte & 0xfeu)) return false;
        value |= (uint64_t)(byte & 0x7fu) << shift;
        if (!(byte & 0x80u)) {
            *out = value;
            return true;
        }
    }
    return false;
}

static bool wire_bytes(wire_reader *r, const uint8_t **out, size_t *len) {
    uint64_t n;
    if (!wire_varint(r, &n) || n > (uint64_t)(r->end - r->p) || n > SIZE_MAX) return false;
    *out = r->p;
    *len = (size_t)n;
    r->p += (size_t)n;
    return true;
}

static bool wire_skip(wire_reader *r, unsigned type) {
    uint64_t ignored;
    const uint8_t *bytes;
    size_t len;
    switch (type) {
    case 0: return wire_varint(r, &ignored);
    case 1:
        if ((size_t)(r->end - r->p) < 8) return false;
        r->p += 8;
        return true;
    case 2: return wire_bytes(r, &bytes, &len);
    case 5:
        if ((size_t)(r->end - r->p) < 4) return false;
        r->p += 4;
        return true;
    default: return false;
    }
}

static bool parse_duration(cursor_sdk_error *error, const uint8_t *data, size_t len) {
    wire_reader r = {data, data + len};
    while (r.p < r.end) {
        uint64_t tag, value;
        if (!wire_varint(&r, &tag) || !tag) return false;
        unsigned field = (unsigned)(tag >> 3), type = (unsigned)(tag & 7u);
        if (field == 1 && type == 0) {
            if (!wire_varint(&r, &value)) return false;
            error->retry_after_seconds = (int64_t)value;
        } else if (field == 2 && type == 0) {
            if (!wire_varint(&r, &value) || value > UINT32_MAX) return false;
            error->retry_after_nanos = (int32_t)(uint32_t)value;
        } else if (!wire_skip(&r, type)) {
            return false;
        }
    }
    error->has_retry_after = true;
    return true;
}

static bool parse_rate_limit(cursor_sdk_error *error, const uint8_t *data, size_t len) {
    wire_reader r = {data, data + len};
    while (r.p < r.end) {
        uint64_t tag, value;
        if (!wire_varint(&r, &tag) || !tag) return false;
        unsigned field = (unsigned)(tag >> 3), type = (unsigned)(tag & 7u);
        if (field >= 1 && field <= 3 && type == 0) {
            if (!wire_varint(&r, &value)) return false;
            if (field == 1) {
                error->rate_limit.has_limit = true;
                error->rate_limit.limit = value;
            } else if (field == 2) {
                error->rate_limit.has_remaining = true;
                error->rate_limit.remaining = value;
            } else {
                error->rate_limit.has_reset_epoch_seconds = true;
                error->rate_limit.reset_epoch_seconds = value;
            }
        } else if (!wire_skip(&r, type)) {
            return false;
        }
    }
    error->has_rate_limit = true;
    return true;
}

static bool parse_details(cursor_sdk_error *error, const uint8_t *data, size_t len) {
    wire_reader r = {data, data + len};
    while (r.p < r.end) {
        uint64_t tag, value;
        const uint8_t *bytes;
        size_t n;
        if (!wire_varint(&r, &tag) || !tag) return false;
        unsigned field = (unsigned)(tag >> 3), type = (unsigned)(tag & 7u);
        if (field == 2 && type == 0) {
            if (!wire_varint(&r, &value) || value > INT32_MAX) return false;
            error->sdk_error_code = (int32_t)value;
        } else if ((field == 1 || field == 3 || field == 4 || field == 5) && type == 2) {
            if (!wire_bytes(&r, &bytes, &n) || !utf8_valid_bytes(bytes, n)) return false;
            if (field == 1) replace_string(&error->request_id, bytes, n);
            else if (field == 3) replace_string(&error->message, bytes, n);
            else if (field == 4) replace_string(&error->help_url, bytes, n);
            else replace_string(&error->provider, bytes, n);
        } else if ((field == 6 || field == 7) && type == 2) {
            if (!wire_bytes(&r, &bytes, &n)) return false;
            if (field == 6) {
                if (!parse_duration(error, bytes, n)) return false;
            } else if (!parse_rate_limit(error, bytes, n)) {
                return false;
            }
        } else if (!wire_skip(&r, type)) {
            return false;
        }
    }
    error->has_sdk_details = true;
    return true;
}

static int b64_digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static size_t decode_detail(const char *in, uint8_t *out, size_t cap) {
    size_t len = strlen(in), written = 0;
    uint32_t value = 0;
    unsigned bits = 0;
    bool padding = false;
    if (len > ((CURSOR_SDK_MAX_ERROR_DETAIL + 2u) / 3u) * 4u + 2u) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=') {
            padding = true;
            continue;
        }
        int digit = b64_digit(c);
        if (digit < 0 || padding) return 0;
        value = (value << 6) | (uint32_t)digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written == cap) return 0;
            out[written++] = (uint8_t)(value >> bits);
            if (bits) value &= (UINT32_C(1) << bits) - 1u;
            else value = 0;
        }
    }
    if (bits >= 6 || value != 0) return 0;
    return written;
}

static bool detail_type_matches(const char *type) {
    static const char suffix[] = "sdk.v1.SdkErrorDetails";
    size_t n = type ? strlen(type) : 0, sn = sizeof suffix - 1;
    return n >= sn && memcmp(type + n - sn, suffix, sn) == 0;
}

int cursor_sdk_error_parse(cursor_sdk_error *error, const char *body, size_t len, int http_status) {
    if (!error || (!body && len) || len > CURSOR_SDK_MAX_ERROR_BODY) return -1;
    cursor_sdk_error_free(error);
    error->http_status = http_status;
    yyjson_doc *doc = jparse(body ? body : "", len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return -1;
    }
    /* Unary failures are bare errors. A server-stream EndStreamResponse wraps
     * the same object under "error" beside optional response metadata. */
    yyjson_val *source = jget(root, "error");
    if (!yyjson_is_obj(source)) source = root;
    const char *code = jget_str(source, "code");
    const char *message = jget_str(source, "message");
    if (code) snprintf(error->connect_code, sizeof error->connect_code, "%.39s", code);
    if (message) error->message = xstrdup(message);

    yyjson_val *details = jget(source, "details");
    if (yyjson_is_arr(details)) {
        size_t idx, max;
        yyjson_val *detail;
        yyjson_arr_foreach(details, idx, max, detail) {
            if (!yyjson_is_obj(detail) || !detail_type_matches(jget_str(detail, "type"))) continue;
            const char *encoded = jget_str(detail, "value");
            if (!encoded || !*encoded) continue;
            size_t cap = (strlen(encoded) * 3u) / 4u + 3u;
            if (cap > CURSOR_SDK_MAX_ERROR_DETAIL) continue;
            uint8_t *decoded = malloc(cap ? cap : 1u);
            if (!decoded) continue;
            size_t decoded_len = decode_detail(encoded, decoded, cap);
            cursor_sdk_error parsed;
            cursor_sdk_error_init(&parsed);
            if (decoded_len && parse_details(&parsed, decoded, decoded_len)) {
                int saved_status = error->http_status;
                char saved_code[sizeof error->connect_code];
                memcpy(saved_code, error->connect_code, sizeof saved_code);
                if (!parsed.message && error->message) parsed.message = xstrdup(error->message);
                cursor_sdk_error_free(error);
                *error = parsed;
                error->http_status = saved_status;
                memcpy(error->connect_code, saved_code, sizeof saved_code);
            } else {
                cursor_sdk_error_free(&parsed);
            }
            free(decoded);
            if (error->has_sdk_details) break;
        }
    }
    yyjson_doc_free(doc);
    return 0;
}

const char *cursor_sdk_error_code_name(int32_t code) {
    static const char *const names[] = {
        "UNSPECIFIED",       "UNAUTHORIZED",         "API_KEY_NOT_FOUND",    "PLAN_REQUIRED",
        "ROLE_FORBIDDEN",    "FEATURE_UNAVAILABLE",  "AGENT_NOT_FOUND",      "RUN_NOT_FOUND",
        "VALIDATION_ERROR",  "INVALID_MODEL",        "INVALID_BRANCH_NAME",  "REPOSITORY_REQUIRED",
        "REPOSITORY_ACCESS", "PR_RESOLUTION_FAILED", "USAGE_LIMIT_EXCEEDED", "AGENT_BUSY",
        "AGENT_ARCHIVED",    "RUN_NOT_CANCELLABLE",  "RATE_LIMIT_EXCEEDED",  "UPSTREAM_ERROR",
        "INTERNAL_ERROR",    "CLIENT_CANCELLED",
    };
    return code >= 0 && (size_t)code < sizeof names / sizeof names[0] ? names[code] : "UNKNOWN";
}
