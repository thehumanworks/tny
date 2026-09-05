#include "core/tools_image.h"
#include "core/image_service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Borrow strings only after rejecting NUL truncation and wrong JSON types. */
static const char *text(yyjson_val *args, const char *key, bool *ok) {
    yyjson_val *v = jget(args, key);
    if (!v) return NULL;
    size_t n = yyjson_get_len(v);
    const char *s = yyjson_get_str(v);
    if (!s || !utf8_valid_bytes(s, n)) {
        *ok = false;
        return NULL;
    }
    return s;
}

static bool request_args(tools_env *env, yyjson_val *args, bool edit, tny_image_request *r,
                         char **paths, char *err, size_t len) {
    bool ok = true;
    *r = (tny_image_request){
        .edit = edit, .cancelled = env->cancelled, .userdata = env->cancelled_ud};
    r->prompt = text(args, "prompt", &ok);
    r->provider = text(args, "provider", &ok);
    r->model = text(args, "model", &ok);
    r->quality = text(args, "quality", &ok);
    r->size = text(args, "size", &ok);
    const char *output = text(args, "output_file", &ok);
    yyjson_val *images = jget(args, "images");
    if (!ok || !r->prompt || !output || !*output || (images && !yyjson_is_arr(images)) ||
        yyjson_arr_size(images) > TNY_IMAGE_REFERENCES_MAX || edit != (yyjson_arr_size(images) > 0))
        goto bad;
    r->image_count = yyjson_arr_size(images);
    char *patherr = NULL;
    paths[0] = tool_resolve_path(env, output, &patherr);
    free(patherr);
    if (!paths[0]) goto bad;
    r->output_file = paths[0];
    for (size_t i = 0; i < r->image_count; i++) {
        yyjson_val *v = yyjson_arr_get(images, i);
        const char *s = yyjson_get_str(v);
        if (!s || !*s || !utf8_valid_bytes(s, yyjson_get_len(v))) goto bad;
        patherr = NULL;
        paths[i + 1] = tool_resolve_path(env, s, &patherr);
        free(patherr);
        if (!paths[i + 1]) goto bad;
        r->images[i] = paths[i + 1];
    }
    return true;
bad:
    snprintf(err, len,
             "invalid image arguments: prompt/output_file strings and 1-5 edit images required; no "
             "embedded NUL");
    return false;
}

static void free_paths(char **paths) {
    for (size_t i = 0; i <= TNY_IMAGE_REFERENCES_MAX; i++) free(paths[i]);
}

char *tool_image_detail(tools_env *env, yyjson_val *args, bool edit, char **error) {
    char *paths[TNY_IMAGE_REFERENCES_MAX + 1] = {0};
    tny_image_request r;
    char err[256];
    buf_t b;
    buf_init(&b);
    if (request_args(env, args, edit, &r, paths, err, sizeof err)) {
        /* JSON escaping keeps paths containing newlines/quotes unambiguous in
         * the permission prompt. Each reference belongs to this grant. */
        buf_appends(&b, "{\"provider\":");
        jescape(&b, r.provider ? r.provider : "codex");
        buf_appends(&b, ",\"output_file\":");
        jescape(&b, r.output_file);
        buf_appends(&b, ",\"images\":[");
        for (size_t i = 0; i < r.image_count; i++) {
            if (i) buf_appends(&b, ",");
            jescape(&b, r.images[i]);
        }
        buf_appends(&b, "]}");
    } else *error = tool_err("%s", err);
    free_paths(paths);
    return buf_detach(&b);
}

int tool_image_run(tools_env *env, yyjson_val *args, bool edit, buf_t *out, char *err, size_t len) {
    if (env->ctx->library_mode || env->ctx->ssh_host) {
        snprintf(err, len,
                 "image tools require a local CLI runtime; unavailable over --ssh or libtny");
        return 1;
    }
    char *paths[TNY_IMAGE_REFERENCES_MAX + 1] = {0};
    tny_image_request r;
    tny_image_result result;
    int rc = 1;
    if (request_args(env, args, edit, &r, paths, err, len)) {
        rc = tny_image_run(env->ctx, &r, &result, err, len);
        if (!rc) tny_image_result_json(&r, &result, out);
        if (out->oom) {
            snprintf(err, len, "cannot format image result");
            rc = 1;
        }
    }
    free_paths(paths);
    return rc;
}
