#include "core/execution_control.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define CONTROL_TEXT_MAX  (4u * 1024u * 1024u)
#define CONTROL_TOTAL_MAX (8u * 1024u * 1024u)
#define CONTROL_ID_MAX    4096u
#define PERMISSION_MASK   (TNY_PERM_ALLOW_ONCE | TNY_PERM_ALLOW_ALWAYS | TNY_PERM_DENY)

typedef struct {
    const char *key;
    size_t offset;
    size_t limit;
} control_string;

#define REQUEST_STRING(field, limit) {#field, offsetof(tny_openai_control_request, field), limit}
static const control_string request_strings[] = {
    REQUEST_STRING(tool_id, CONTROL_ID_MAX),
    REQUEST_STRING(tool_name, CONTROL_ID_MAX),
    REQUEST_STRING(arguments_json, CONTROL_TEXT_MAX),
    REQUEST_STRING(original_arguments_json, CONTROL_TEXT_MAX),
    REQUEST_STRING(result, CONTROL_TEXT_MAX),
    REQUEST_STRING(control_extension, CONTROL_ID_MAX),
    REQUEST_STRING(control_reason, CONTROL_TEXT_MAX),
    REQUEST_STRING(permission_summary, CONTROL_TEXT_MAX),
    REQUEST_STRING(subagent_id, CONTROL_ID_MAX),
    REQUEST_STRING(subagent_action, CONTROL_ID_MAX),
    REQUEST_STRING(subagent_outcome, CONTROL_ID_MAX),
};
#undef REQUEST_STRING
#define RESPONSE_STRING(field, limit) {#field, offsetof(tny_openai_control_response, field), limit}
static const control_string response_strings[] = {
    RESPONSE_STRING(arguments_json, CONTROL_TEXT_MAX),
    RESPONSE_STRING(result, CONTROL_TEXT_MAX),
    RESPONSE_STRING(extension, CONTROL_ID_MAX),
    RESPONSE_STRING(reason, CONTROL_TEXT_MAX),
};
#undef RESPONSE_STRING
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

static bool supported_kind(tny_openai_control_kind kind) {
    return kind == TNY_OPENAI_CONTROL_PRE_TOOL || kind == TNY_OPENAI_CONTROL_PERMISSION ||
           kind == TNY_OPENAI_CONTROL_POST_TOOL || kind == TNY_OPENAI_CONTROL_SUBAGENT_START ||
           kind == TNY_OPENAI_CONTROL_SUBAGENT_END;
}

static bool valid_request(const tny_openai_control_request *request) {
    if (!request || !supported_kind(request->kind) || request->permission_options < 0 ||
        (request->permission_options & ~PERMISSION_MASK))
        return false;
    if (request->kind == TNY_OPENAI_CONTROL_SUBAGENT_START ||
        request->kind == TNY_OPENAI_CONTROL_SUBAGENT_END)
        return request->subagent_id && *request->subagent_id && request->subagent_action &&
               *request->subagent_action;
    return request->tool_id && *request->tool_id && request->tool_name && *request->tool_name;
}

/* Every nullable member is represented explicitly. Exact field count plus
 * presence/type checks reject unknown members, missing fields and duplicates. */
static bool encode_strings(yyjson_mut_doc *doc, yyjson_mut_val *out, const void *record,
                           const control_string *fields, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        const char *str;
        memcpy(&str, (const char *)record + fields[i].offset, sizeof str);
        if (!str) {
            if (!yyjson_mut_obj_add_null(doc, out, fields[i].key)) return false;
            continue;
        }
        size_t len = strnlen(str, fields[i].limit + 1);
        if (len > fields[i].limit || len > CONTROL_TOTAL_MAX - total) return false;
        total += len;
        if (!yyjson_mut_obj_add_strncpy(doc, out, fields[i].key, str, len)) return false;
    }
    return true;
}

static bool decode_strings(yyjson_val *value, void *record, const control_string *fields,
                           size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        yyjson_val *item = jget(value, fields[i].key);
        if (!item) return false;
        const char *str = NULL;
        if (!yyjson_is_null(item)) {
            if (!yyjson_is_str(item)) return false;
            size_t len = yyjson_get_len(item);
            str = yyjson_get_str(item);
            if (len > fields[i].limit || len > CONTROL_TOTAL_MAX - total || memchr(str, '\0', len))
                return false;
            total += len;
        }
        memcpy((char *)record + fields[i].offset, &str, sizeof str);
    }
    return true;
}

static bool integer(yyjson_val *value, const char *key, int64_t min, int64_t max, int64_t *out) {
    yyjson_val *item = jget(value, key);
    if (!yyjson_is_int(item)) return false;
    if (yyjson_is_uint(item) && yyjson_get_uint(item) > (uint64_t)max) return false;
    int64_t number = yyjson_get_sint(item);
    if (number < min || number > max) return false;
    *out = number;
    return true;
}

static bool boolean(yyjson_val *value, const char *key, bool *out) {
    yyjson_val *item = jget(value, key);
    if (!yyjson_is_bool(item)) return false;
    *out = yyjson_get_bool(item);
    return true;
}

yyjson_mut_val *tny_execution_control_encode_request(yyjson_mut_doc *doc,
                                                     const tny_openai_control_request *request) {
    if (!doc || !valid_request(request)) return NULL;
    yyjson_mut_val *out = yyjson_mut_obj(doc);
    if (!out || !encode_strings(doc, out, request, request_strings, ARRAY_COUNT(request_strings)) ||
        !yyjson_mut_obj_add_int(doc, out, "kind", request->kind) ||
        !yyjson_mut_obj_add_int(doc, out, "permission_options", request->permission_options) ||
        !yyjson_mut_obj_add_bool(doc, out, "original_ok", request->original_ok) ||
        !yyjson_mut_obj_add_bool(doc, out, "subagent_ok", request->subagent_ok))
        return NULL;
    return out;
}

bool tny_execution_control_decode_request(yyjson_val *value, tny_openai_control_request *request) {
    if (!request) return false;
    memset(request, 0, sizeof *request);
    tny_openai_control_request decoded = {0};
    int64_t kind, options;
    if (!yyjson_is_obj(value) || yyjson_obj_size(value) != ARRAY_COUNT(request_strings) + 4 ||
        !decode_strings(value, &decoded, request_strings, ARRAY_COUNT(request_strings)) ||
        !integer(value, "kind", TNY_OPENAI_CONTROL_PRE_TOOL, TNY_OPENAI_CONTROL_SUBAGENT_END,
                 &kind) ||
        !integer(value, "permission_options", 0, PERMISSION_MASK, &options) ||
        !boolean(value, "original_ok", &decoded.original_ok) ||
        !boolean(value, "subagent_ok", &decoded.subagent_ok))
        return false;
    decoded.kind = (tny_openai_control_kind)kind;
    decoded.permission_options = (int)options;
    if (!valid_request(&decoded)) return false;
    *request = decoded;
    return true;
}

static bool valid_response(const tny_openai_control_response *response) {
    return response && response->permission >= TNY_OPENAI_PERMISSION_ABSTAIN &&
           response->permission <= TNY_OPENAI_PERMISSION_DENY &&
           (!response->result_replaced || response->result);
}

yyjson_mut_val *tny_execution_control_encode_response(yyjson_mut_doc *doc,
                                                      const tny_openai_control_response *response) {
    if (!doc || !valid_response(response)) return NULL;
    yyjson_mut_val *out = yyjson_mut_obj(doc);
    if (!out ||
        !encode_strings(doc, out, response, response_strings, ARRAY_COUNT(response_strings)) ||
        !yyjson_mut_obj_add_int(doc, out, "permission", response->permission) ||
        !yyjson_mut_obj_add_bool(doc, out, "deny", response->deny) ||
        !yyjson_mut_obj_add_bool(doc, out, "stop", response->stop) ||
        !yyjson_mut_obj_add_bool(doc, out, "result_replaced", response->result_replaced) ||
        !yyjson_mut_obj_add_bool(doc, out, "result_is_error", response->result_is_error))
        return NULL;
    return out;
}

bool tny_execution_control_decode_response(yyjson_val *value,
                                           tny_openai_control_response *response) {
    if (!response) return false;
    memset(response, 0, sizeof *response);
    tny_openai_control_response decoded = {0};
    int64_t permission;
    if (!yyjson_is_obj(value) || yyjson_obj_size(value) != ARRAY_COUNT(response_strings) + 5 ||
        !decode_strings(value, &decoded, response_strings, ARRAY_COUNT(response_strings)) ||
        !integer(value, "permission", TNY_OPENAI_PERMISSION_ABSTAIN, TNY_OPENAI_PERMISSION_DENY,
                 &permission) ||
        !boolean(value, "deny", &decoded.deny) || !boolean(value, "stop", &decoded.stop) ||
        !boolean(value, "result_replaced", &decoded.result_replaced) ||
        !boolean(value, "result_is_error", &decoded.result_is_error))
        return false;
    decoded.permission = (tny_openai_permission_decision)permission;
    if (!valid_response(&decoded)) return false;
    *response = decoded;
    response->arguments_json = response->result = response->extension = response->reason = NULL;
    for (size_t i = 0; i < ARRAY_COUNT(response_strings); i++) {
        const char *borrowed;
        memcpy(&borrowed, (const char *)&decoded + response_strings[i].offset, sizeof borrowed);
        char *owned = borrowed ? xstrdup(borrowed) : NULL;
        if (borrowed && !owned) {
            tny_execution_control_response_free(response);
            return false;
        }
        memcpy((char *)response + response_strings[i].offset, &owned, sizeof owned);
    }
    return true;
}

void tny_execution_control_response_free(tny_openai_control_response *response) {
    if (!response) return;
    free(response->arguments_json);
    free(response->result);
    free(response->extension);
    free(response->reason);
    memset(response, 0, sizeof *response);
}
