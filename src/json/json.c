#include "json/json.h"
#include "util/alloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

yyjson_val *jget(yyjson_val *obj, const char *key) {
    if (!obj || !yyjson_is_obj(obj)) return NULL;
    return yyjson_obj_get(obj, key);
}

const char *jget_str(yyjson_val *obj, const char *key) { return jget_strn(obj, key, NULL); }

const char *jget_strn(yyjson_val *obj, const char *key, size_t *len_out) {
    yyjson_val *v = jget(obj, key);
    if (!v || !yyjson_is_str(v)) {
        if (len_out) *len_out = 0;
        return NULL;
    }
    if (len_out) *len_out = yyjson_get_len(v);
    return yyjson_get_str(v);
}

int64_t jget_int(yyjson_val *obj, const char *key, int64_t dflt) {
    yyjson_val *v = jget(obj, key);
    if (!v) return dflt;
    if (yyjson_is_int(v)) return yyjson_get_sint(v);
    if (yyjson_is_real(v)) return (int64_t)yyjson_get_real(v);
    return dflt;
}

double jget_num(yyjson_val *obj, const char *key, double dflt) {
    yyjson_val *v = jget(obj, key);
    if (!v) return dflt;
    if (yyjson_is_num(v)) return yyjson_get_num(v);
    return dflt;
}

bool jget_bool(yyjson_val *obj, const char *key, bool dflt) {
    yyjson_val *v = jget(obj, key);
    return v && yyjson_is_bool(v) ? yyjson_get_bool(v) : dflt;
}

yyjson_doc *jparse(const char *data, size_t len) {
    return data ? yyjson_read_opts((char *)(uintptr_t)data, len, 0, jallocator(), NULL) : NULL;
}

yyjson_doc *jparse_file(const char *path) {
    size_t len = 0;
    char *data = file_slurp(path, &len);
    if (!data) return NULL;
    yyjson_doc *doc = yyjson_read_opts(data, len, 0, jallocator(), NULL);
    free(data);
    return doc;
}

char *jwrite(yyjson_mut_doc *doc) {
    return yyjson_mut_write_opts(doc, 0, jallocator(), NULL, NULL);
}

char *jwrite_pretty(yyjson_mut_doc *doc) {
    return yyjson_mut_write_opts(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, jallocator(), NULL, NULL);
}

char *jwrite_val(yyjson_val *val) {
    return yyjson_val_write_opts(val, 0, jallocator(), NULL, NULL);
}

char *jwrite_mut_val(yyjson_mut_val *val) {
    return yyjson_mut_val_write_opts(val, 0, jallocator(), NULL, NULL);
}

void jescape(buf_t *b, const char *s) {
    if (!b || !s) {
        if (b) b->oom = true;
        return;
    }
    buf_appends(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': buf_appends(b, "\\\""); break;
        case '\\': buf_appends(b, "\\\\"); break;
        case '\n': buf_appends(b, "\\n"); break;
        case '\r': buf_appends(b, "\\r"); break;
        case '\t': buf_appends(b, "\\t"); break;
        default:
            if (*p < 0x20) buf_appendf(b, "\\u%04x", *p);
            else buf_append(b, p, 1);
        }
    }
    buf_appends(b, "\"");
}
static void *json_malloc(void *ctx, size_t size) {
    (void)ctx;
    return tny_alloc_malloc(size);
}

static void *json_realloc(void *ctx, void *ptr, size_t old_size, size_t size) {
    (void)ctx;
    (void)old_size;
    return tny_alloc_realloc(ptr, size);
}

static void json_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

const yyjson_alc *jallocator(void) {
    static const yyjson_alc allocator = {json_malloc, json_realloc, json_free, NULL};
    return &allocator;
}
