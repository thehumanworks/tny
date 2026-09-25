#include "core/execution_protocol.h"
#include <stdlib.h>
#include <string.h>

bool tny_exec_protocol_admit(tny_exec_phase phase, tny_exec_kind kind, uint64_t id,
                             uint64_t expected) {
    if (!id || !expected) return false;
    if (phase == TNY_EXEC_WAIT_START) return kind == TNY_EXEC_EXECUTE && id == 1 && expected == 1;
    if (phase == TNY_EXEC_WAIT_REPLY) return kind == TNY_EXEC_RESULT && id == expected;
    if (phase != TNY_EXEC_WAIT_RESULT) return false;
    if (kind == TNY_EXEC_RESULT) return id == 1;
    return expected >= 2 && id == expected && kind >= TNY_EXEC_CONTROL && kind <= TNY_EXEC_STATE;
}

static bool valid_value(yyjson_val *v, unsigned depth, size_t *left) {
    if (!v || !*left || depth > 64) return false;
    --*left;
    if (yyjson_is_str(v)) return strlen(yyjson_get_str(v)) == yyjson_get_len(v);
    if (yyjson_is_arr(v)) {
        size_t i, n;
        yyjson_val *item;
        yyjson_arr_foreach(v, i, n, item) if (!valid_value(item, depth + 1, left)) return false;
    } else if (yyjson_is_obj(v)) {
        if (yyjson_obj_size(v) > 512) return false;
        size_t i, n;
        yyjson_val *k, *item;
        yyjson_obj_foreach(v, i, n, k, item) {
            if (!valid_value(k, depth + 1, left) || !valid_value(item, depth + 1, left))
                return false;
            size_t j, m;
            yyjson_val *other, *unused;
            yyjson_obj_foreach(v, j, m, other, unused) {
                (void)unused;
                if (j >= i) break;
                if (yyjson_equals(k, other)) return false;
            }
        }
    }
    return true;
}

bool tny_exec_json_valid(yyjson_val *root) {
    size_t left = 200000;
    return valid_value(root, 0, &left);
}

static const char *method_name(tny_exec_kind kind) {
    switch (kind) {
    case TNY_EXEC_EXECUTE: return "execute";
    case TNY_EXEC_CONTROL: return "control";
    case TNY_EXEC_EVENT: return "event";
    case TNY_EXEC_PROMPT: return "prompt";
    case TNY_EXEC_ASK: return "ask";
    case TNY_EXEC_STATE: return "state";
    default: return NULL;
    }
}

tny_exec_kind tny_exec_envelope(yyjson_val *root, uint64_t *id, yyjson_val **body) {
    if (!yyjson_is_obj(root) || !tny_exec_json_valid(root)) return TNY_EXEC_INVALID;
    const char *rpc = jget_str(root, "jsonrpc");
    yyjson_val *version = jget(root, "version"), *ident = jget(root, "id");
    if (!rpc || strcmp(rpc, "2.0") != 0 || !yyjson_is_uint(version) ||
        yyjson_get_uint(version) != TNY_EXEC_PROTOCOL_VERSION || !yyjson_is_uint(ident) ||
        !yyjson_get_uint(ident))
        return TNY_EXEC_INVALID;
    const char *method = jget_str(root, "method");
    tny_exec_kind kind = TNY_EXEC_INVALID;
    if (!method) {
        if (yyjson_obj_size(root) != 4 || !jget(root, "result")) return TNY_EXEC_INVALID;
        kind = TNY_EXEC_RESULT;
        *body = jget(root, "result");
    } else {
        if (yyjson_obj_size(root) != 5 || !yyjson_is_obj(jget(root, "params")))
            return TNY_EXEC_INVALID;
        for (int i = TNY_EXEC_EXECUTE; i <= TNY_EXEC_STATE; i++) {
            if (strcmp(method, method_name((tny_exec_kind)i)) == 0) kind = (tny_exec_kind)i;
        }
        *body = jget(root, "params");
    }
    *id = yyjson_get_uint(ident);
    return kind;
}

char *tny_exec_message(uint64_t id, tny_exec_kind kind, yyjson_val *body) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d) return NULL;
    yyjson_mut_val *r = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, r);
    bool ok = r && yyjson_mut_obj_add_str(d, r, "jsonrpc", "2.0") &&
              yyjson_mut_obj_add_uint(d, r, "version", TNY_EXEC_PROTOCOL_VERSION) &&
              yyjson_mut_obj_add_uint(d, r, "id", id);
    const char *method = method_name(kind);
    if (method) ok = ok && yyjson_mut_obj_add_str(d, r, "method", method);
    if (!method && kind != TNY_EXEC_RESULT) ok = false;
    yyjson_mut_val *value = body ? yyjson_val_mut_copy(d, body) : yyjson_mut_obj(d);
    ok = ok && value && yyjson_mut_obj_add_val(d, r, method ? "params" : "result", value);
    char *out = ok ? jwrite(d) : NULL;
    yyjson_mut_doc_free(d);
    return out;
}
