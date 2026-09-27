/* Deterministic virtual capabilities. Never opens model-selected paths. */
#include "bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int64_t bench_clock_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
bool bench_append(bench_state *s, const char *text, size_t len) {
    if (len > BENCH_OUTPUT_LIMIT - s->output_len || memchr(text, 0, len)) return false;
    memcpy(s->output + s->output_len, text, len);
    s->output_len += len;
    s->output[s->output_len] = 0;
    return true;
}
static const char *text(yyjson_val *v) {
    const char *s = yyjson_get_str(v);
    return s && strlen(s) == yyjson_get_len(v) ? s : NULL;
}
static char *invalid(bench_state *s) {
    s->invalid_call = true;
    return strdup("error: undeclared capability, invalid arguments, or missing virtual input");
}
char *bench_describe(bench_state *s, const char *name) {
    yyjson_doc *d = yyjson_read(s->catalog, strlen(s->catalog), 0);
    yyjson_val *v;
    size_t i, n;
    char *out = NULL;
    yyjson_arr_foreach(yyjson_doc_get_root(d), i, n, v) {
        const char *candidate = text(yyjson_obj_get(v, "name"));
        if (candidate && !strcmp(candidate, name)) {
            out = yyjson_val_write(v, 0, NULL);
            break;
        }
    }
    yyjson_doc_free(d);
    return out;
}
char *bench_call(void *ud, const char *name, const char *args) {
    bench_state *s = ud;
    if (++s->call_count > BENCH_CALL_LIMIT) return invalid(s);
    yyjson_doc *d = yyjson_read(args, strlen(args), 0);
    yyjson_val *a = yyjson_doc_get_root(d);
    if (!a || !yyjson_is_obj(a)) {
        yyjson_doc_free(d);
        return invalid(s);
    }
    yyjson_mut_val *event = yyjson_mut_obj(s->result);
    yyjson_mut_obj_add_strcpy(s->result, event, "name", name);
    yyjson_mut_obj_add_val(s->result, event, "args", yyjson_val_mut_copy(s->result, a));
    yyjson_mut_arr_add_val(s->calls, event);
    size_t fields = yyjson_obj_size(a);
    const char *path = text(yyjson_obj_get(a, "path"));
    char *out = NULL;
    if (!strcmp(name, "fs.read") && fields == 1 && path) {
        const char *content = text(yyjson_obj_get(yyjson_obj_get(s->fixture, "files"), path));
        if (content) out = strdup(content);
    } else if (!strcmp(name, "fs.write") && fields == 2 && path) {
        const char *content = text(yyjson_obj_get(a, "content"));
        if (content && strlen(content) <= 262144) {
            yyjson_mut_obj_put(s->writes, yyjson_mut_strcpy(s->result, path),
                               yyjson_mut_strcpy(s->result, content));
            out = strdup("{\"ok\":true}");
        }
    } else if (!strcmp(name, "fs.list") && fields == 1) {
        const char *prefix = text(yyjson_obj_get(a, "prefix"));
        if (prefix) {
            yyjson_mut_doc *list = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *root = yyjson_mut_arr(list);
            yyjson_mut_doc_set_root(list, root);
            yyjson_val *key, *value;
            size_t i, n;
            yyjson_obj_foreach(yyjson_obj_get(s->fixture, "files"), i, n, key, value) {
                (void)value;
                const char *file = text(key);
                if (file && !strncmp(file, prefix, strlen(prefix)))
                    yyjson_mut_arr_add_strcpy(list, root, file);
            }
            out = yyjson_mut_write(list, 0, NULL);
            yyjson_mut_doc_free(list);
        }
    } else if (!strcmp(name, "api.page") && fields == 1) {
        yyjson_val *cursor = yyjson_obj_get(a, "cursor");
        const char *key = yyjson_is_null(cursor) ? "__first__" : text(cursor);
        if (key) {
            yyjson_val *page = yyjson_obj_get(yyjson_obj_get(s->fixture, "pages"), key);
            if (page) out = yyjson_val_write(page, 0, NULL);
        }
    } else if (!strcmp(name, "api.check") && fields == 1) {
        const char *id = text(yyjson_obj_get(a, "id"));
        if (id) {
            size_t seen = 0, i, n;
            yyjson_mut_val *call;
            yyjson_mut_arr_foreach(s->calls, i, n, call) {
                const char *method = yyjson_mut_get_str(yyjson_mut_obj_get(call, "name"));
                const char *prior =
                    yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_obj_get(call, "args"), "id"));
                if (method && prior && !strcmp(method, name) && !strcmp(prior, id)) ++seen;
            }
            yyjson_val *replies = yyjson_obj_get(yyjson_obj_get(s->fixture, "checks"), id);
            yyjson_val *reply = seen ? yyjson_arr_get(replies, seen - 1) : NULL;
            if (reply) out = yyjson_val_write(reply, 0, NULL);
        }
    }
    yyjson_doc_free(d);
    return out ? out : invalid(s);
}
int main(void) {
    const size_t limit = 2 * 1024 * 1024;
    char *wire = malloc(limit + 1);
    if (!wire) return 2;
    size_t len = fread(wire, 1, limit, stdin);
    if (ferror(stdin) || (!feof(stdin) && fgetc(stdin) != EOF)) {
        free(wire);
        return 2;
    }
    wire[len] = 0;
    yyjson_doc *input = yyjson_read(wire, len, 0);
    free(wire);
    yyjson_val *root = yyjson_doc_get_root(input);
    const char *code = text(yyjson_obj_get(root, "code"));
    const char *catalog = text(yyjson_obj_get(root, "catalog"));
    yyjson_val *fixture = yyjson_obj_get(root, "fixture");
    if (!code || strlen(code) > 262144 || !catalog || !yyjson_is_obj(fixture)) {
        yyjson_doc_free(input);
        return 2;
    }
    bench_state *s = calloc(1, sizeof(*s));
    if (!s) {
        yyjson_doc_free(input);
        return 2;
    }
    s->fixture = fixture;
    s->catalog = catalog;
    s->result = yyjson_mut_doc_new(NULL);
    s->calls = yyjson_mut_arr(s->result);
    s->writes = yyjson_mut_obj(s->result);
    yyjson_mut_val *result = yyjson_mut_obj(s->result);
    yyjson_mut_doc_set_root(s->result, result);
    int64_t start = bench_clock_ns();
    s->deadline = start + 2000000000;
    bool ok = bench_execute(s, code);
    int64_t elapsed = bench_clock_ns() - start;
    yyjson_mut_obj_add_bool(s->result, result, "runtime_ok", ok);
    yyjson_mut_obj_add_bool(s->result, result, "invalid_call", s->invalid_call);
    yyjson_mut_obj_add_strcpy(s->result, result, "stdout", s->output);
    yyjson_mut_obj_add_int(s->result, result, "runtime_ns", elapsed);
    yyjson_mut_obj_add_val(s->result, result, "calls", s->calls);
    yyjson_mut_obj_add_val(s->result, result, "writes", s->writes);
    char *output = yyjson_mut_write(s->result, 0, NULL);
    int rc = output && puts(output) >= 0 ? 0 : 2;
    free(output);
    yyjson_mut_doc_free(s->result);
    yyjson_doc_free(input);
    free(s);
    return rc;
}
