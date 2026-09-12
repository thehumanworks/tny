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

static bool flag(yyjson_val *args, const char *key, bool dflt, bool *ok) {
    yyjson_val *v = jget(args, key);
    if (!v) return dflt;
    if (!yyjson_is_bool(v)) *ok = false;
    return yyjson_get_bool(v);
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
    r->strict_size = flag(args, "strict_size", false, &ok);
    r->no_manifest = !flag(args, "persist_manifest", true, &ok);
    const char *source = text(args, "from_manifest", &ok);
    const char *artifact = text(args, "artifact", &ok);
    const char *output = text(args, "output_file", &ok);
    yyjson_val *images = jget(args, "images");
    size_t count = yyjson_arr_size(images) + (artifact ? 1u : 0u);
    /* A rerun carries its own recorded references and prompt; mixing it with
     * new references or a second record would make the operation ambiguous. */
    if (!ok || !output || !*output || (images && !yyjson_is_arr(images)) ||
        (source && (artifact || count)) || (artifact && !*artifact) || (source && !*source) ||
        count > TNY_IMAGE_REFERENCES_MAX || (!source && !r->prompt) ||
        (!source && edit != (count > 0)) || (artifact && !edit))
        goto bad;
    char *patherr = NULL;
    paths[0] = tool_resolve_path(env, output, &patherr);
    free(patherr);
    if (!paths[0]) goto bad;
    r->output_file = paths[0];
    size_t slot = 1;
    if (source) {
        patherr = NULL;
        paths[slot] = tool_resolve_path(env, source, &patherr);
        free(patherr);
        if (!paths[slot]) goto bad;
        r->from_manifest = paths[slot++];
        r->replay = false; /* the tool name, not the record, fixes the operation */
    }
    if (artifact) {
        patherr = NULL;
        paths[slot] = tool_resolve_path(env, artifact, &patherr);
        free(patherr);
        if (!paths[slot]) goto bad;
        r->image_is_artifact[r->image_count] = true;
        r->images[r->image_count++] = paths[slot++];
    }
    size_t i, n;
    yyjson_val *v;
    yyjson_arr_foreach(images, i, n, v) {
        const char *s = yyjson_get_str(v);
        if (!s || !*s || !utf8_valid_bytes(s, yyjson_get_len(v))) goto bad;
        patherr = NULL;
        paths[slot] = tool_resolve_path(env, s, &patherr);
        free(patherr);
        if (!paths[slot]) goto bad;
        r->images[r->image_count++] = paths[slot++];
    }
    return true;
bad:
    snprintf(err, len,
             "invalid image arguments: prompt/output_file strings, boolean "
             "strict_size/persist_manifest and 1-5 edit images required; from_manifest replaces "
             "them and excludes artifact; no embedded NUL");
    return false;
}

static void free_paths(char **paths) {
    for (size_t i = 0; i < TNY_IMAGE_PATHS_MAX; i++) free(paths[i]);
}

/* Resolve exactly what this call would upload, from records only. A path that
 * came out of a manifest is confined to the directories this runtime already
 * allows, so a hostile record can never widen tny's reach; a path the caller
 * typed keeps its existing behavior and is governed by the grant alone. */
static bool resolve_plan(tools_env *env, const tny_image_request *r, tny_image_plan *plan,
                         char *err, size_t len) {
    if (tny_image_plan_resolve(r, plan, err, len)) return false;
    for (size_t i = 0; i < plan->reference_count; i++) {
        if (!*plan->references[i].expected) continue;
        if (!perm_path_allowed(env->ctx, plan->references[i].path)) {
            snprintf(err, len, "image manifest references a file outside the allowed directories");
            return false;
        }
    }
    return true;
}

/* The grant scope is the real resolved provider, destination, record source
 * and every reference path that would actually be uploaded. The provider is
 * the plan's — a replay that inherits another provider from its record is
 * described as that provider, so an old rule or grant written for a different
 * one no longer authorizes it (ADR 0095). */
static void describe(const tny_image_request *r, const tny_image_plan *plan, buf_t *b) {
    buf_appends(b, "{\"provider\":");
    jescape(b, plan->provider ? plan->provider : "codex");
    buf_appends(b, ",\"output_file\":");
    jescape(b, r->output_file);
    if (r->from_manifest) {
        buf_appends(b, ",\"from_manifest\":");
        jescape(b, r->from_manifest);
    }
    buf_appends(b, ",\"images\":[");
    for (size_t i = 0; i < plan->reference_count; i++) {
        if (i) buf_appends(b, ",");
        jescape(b, plan->references[i].path);
    }
    buf_appends(b, "]}");
}

void tool_image_plan_free(tny_image_plan *plan) {
    if (!plan) return;
    tny_image_plan_free(plan);
    free(plan);
}

char *tool_image_detail(tools_env *env, yyjson_val *args, bool edit, tny_image_plan **prepared,
                        char **error) {
    char *paths[TNY_IMAGE_PATHS_MAX] = {0};
    tny_image_request r = {0};
    char err[256] = "out of memory preparing an image call";
    buf_t b;
    buf_init(&b);
    if (prepared) *prepared = NULL;
    tny_image_plan *plan = calloc(1, sizeof *plan);
    bool described = plan && request_args(env, args, edit, &r, paths, err, sizeof err) &&
                     resolve_plan(env, &r, plan, err, sizeof err);
    /* JSON escaping keeps paths containing newlines/quotes unambiguous in the
     * permission prompt. Each reference belongs to this grant. */
    if (described) describe(&r, plan, &b);
    if (!described) *error = tool_err("%s", err);
    else if (b.oom) *error = tool_err("out of memory preparing an image call");
    /* The described plan is exactly what a later execution must run, so it is
     * handed over rather than resolved a second time (ADR 0095). */
    else if (prepared) {
        *prepared = plan;
        plan = NULL;
    }
    tool_image_plan_free(plan);
    free_paths(paths);
    return buf_detach(&b);
}

int tool_image_run(tools_env *env, yyjson_val *args, bool edit, tny_image_plan *prepared,
                   buf_t *out, char *err, size_t len) {
    if (env->ctx->library_mode || env->ctx->ssh_host) {
        snprintf(err, len,
                 "image tools require a local CLI runtime; unavailable over --ssh or libtny");
        return 1;
    }
    char *paths[TNY_IMAGE_PATHS_MAX] = {0};
    /* Both structs are readable before any fallible work below. */
    tny_image_request r = {0};
    tny_image_result result = {0};
    int rc = 1;
    if (request_args(env, args, edit, &r, paths, err, len)) {
        /* The plan resolved for the permission decision is the one that runs:
         * a record edited in between cannot substitute its own provider,
         * settings or reference paths, and the one-time grant this call
         * already has is not re-asked and not widened. */
        rc = tny_image_run_prepared(env->ctx, &r, prepared, &result, err, len);
        if (!rc) tny_image_result_json(&r, &result, out);
        /* The paid artifact is in place and only its record failed: say so,
         * with the committed path. */
        else if (tny_image_retained_failure(&result))
            tny_image_retained_json(&r, &result, err, out);
        /* tny's own strict-size rejection is the only other failure that may
         * describe itself; every other failure leaves the buffer empty. */
        else if (tny_image_strict_failure(&result)) tny_image_error_json(&r, &result, err, out);
        if (out->oom) {
            /* A partially formatted object is never exposed: the call
             * degrades to its ordinary diagnostic with no structured
             * detail at all. */
            buf_free(out);
            buf_init(out);
            if (!rc) {
                snprintf(err, len, "cannot format image result");
                rc = 1;
            }
        }
    }
    free_paths(paths);
    return rc;
}

char *tool_image_execute(tools_env *env, yyjson_val *args, bool edit, tny_image_plan *prepared) {
    buf_t out;
    buf_init(&out);
    char err[256] = "";
    int rc = tool_image_run(env, args, edit, prepared, &out, err, sizeof err);
    if (!rc) return buf_detach(&out);
    /* A strict-size rejection and a retained artifact keep the established
     * `error: ` marker so failure detection is unchanged; the bytes after it
     * are the shared safe object (docs/images.md). Anything else has none. */
    buf_t message;
    buf_init(&message);
    if (out.len) {
        size_t end = out.len;
        while (end && out.data[end - 1] == '\n') end--;
        buf_appends(&message, "error: ");
        buf_append(&message, out.data, end);
    }
    buf_free(&out);
    char *detailed = message.len ? buf_detach(&message) : NULL;
    buf_free(&message);
    return detailed ? detailed : tool_err("%s", err);
}
