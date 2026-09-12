#include "core/tools_image.h"
#include "core/image_export.h"
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
    r->job = text(args, "job", &ok);
    yyjson_val *item = jget(args, "item");
    r->job_item_set = item != NULL;
    if (item && (!yyjson_is_uint(item) || yyjson_get_uint(item) >= TNY_JOBS_MAX_ITEMS)) ok = false;
    r->job_item = (int)yyjson_get_uint(item);
    if (!!r->job != r->job_item_set ||
        (r->job && (!edit || !tny_jobs_valid_id(r->job) || env->ctx->library_mode)))
        ok = false;
    r->preview = flag(args, "preview", false, &ok);
    r->strict_size = flag(args, "strict_size", false, &ok);
    r->no_manifest = !flag(args, "persist_manifest", true, &ok);
    const char *source = text(args, "from_manifest", &ok);
    const char *artifact = text(args, "artifact", &ok);
    const char *output = text(args, "output_file", &ok);
    yyjson_val *images = jget(args, "images");
    size_t count = yyjson_arr_size(images) + (artifact ? 1u : 0u) + (r->job ? 1u : 0u);
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
    if (tny_image_plan_resolve(env->ctx, r, plan, err, len)) return false;
    /* A record of a local export documents work tny did itself. Rerunning it
     * as a provider request would invent a generation that never happened, so
     * it is refused with the operation that does apply (docs/adr/0094). */
    if (tny_image_manifest_derived(plan->source)) {
        snprintf(err, len,
                 "this record documents a local image export, not a provider operation; export "
                 "again from its source, or pass the derived image as an edit reference");
        return false;
    }
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
/* False preserves ordinary grant keys byte-for-byte. True binds the upload
 * target in addition to (not instead of) the existing operation identity. */
static void preview_detail(tools_env *env, bool preview, buf_t *b) {
    if (!preview || b->oom) return;
    buf_appends(b, "\nconversation_preview:");
    jescape(b, env->ctx->provider_name ? env->ctx->provider_name : "openai");
    buf_appends(b, ":");
    jescape(b, env->ctx->model ? env->ctx->model : "");
}

static void preview_result(tools_env *env, bool requested, bool success,
                           const tny_image_preview_identity *id, buf_t *out) {
    if (!requested || out->oom) return;
    tny_image_preview_result preview;
    tny_image_preview_coordinate(success, id, env->preview_admit, env->preview_ud, &preview);
    tny_image_preview_append(out, id, &preview);
}

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
    buf_appends(b, "]");
    for (size_t i = 0; i < plan->reference_count; i++) {
        const tny_image_reference *ref = &plan->references[i];
        if (!*ref->job.id) continue;
        buf_appends(b, ",\"job\":");
        tny_image_job_json(&ref->job, b);
        buf_appends(b, ",\"sha256\":");
        jescape(b, ref->expected);
        buf_appends(b, ",\"source_manifest\":");
        if (ref->source_manifest) jescape(b, ref->source_manifest);
        else buf_appends(b, "null");
        buf_appends(b, ",\"source_operation\":");
        jescape(b, ref->source_operation);
    }
    buf_appends(b, "}");
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
    if (described) {
        describe(&r, plan, &b);
        preview_detail(env, r.preview, &b);
    }
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

char *tool_image_preview_detail(tools_env *env, yyjson_val *args,
                                tny_image_preview_selection **selection, char **error) {
    bool ok = true;
    const char *manifest = text(args, "manifest", &ok);
    const char *job = text(args, "job", &ok);
    yyjson_val *item_value = jget(args, "item");
    int item =
        item_value && yyjson_is_uint(item_value) && yyjson_get_uint(item_value) < TNY_JOBS_MAX_ITEMS
            ? (int)yyjson_get_uint(item_value)
            : -1;
    if (!ok || (!!manifest == !!job) || (job && (item < 0 || yyjson_obj_size(args) != 2)) ||
        (manifest && (!*manifest || item_value || yyjson_obj_size(args) != 1)) ||
        env->ctx->library_mode) {
        *error =
            tool_err("image_preview requires exactly a manifest string or job and integer item");
        return NULL;
    }
    char *path = manifest ? tool_resolve_path(env, manifest, error) : NULL;
    if (manifest && !path) return NULL;
    char err[256] = "cannot prepare image preview";
    *selection = tny_image_preview_select(env->ctx, path, job, item, err, sizeof err);
    free(path);
    if (!*selection) {
        *error = tool_err("%s", err);
        return NULL;
    }
    /* Admission canonicalizes and confines the selected path before capture.
     * Do not compare a manifest spelling (e.g. macOS /var) to canonical roots
     * here. No image bytes have been read at this permission boundary. */
    buf_t detail;
    buf_init(&detail);
    buf_appends(&detail, "{\"manifest\":");
    if ((*selection)->identity.manifest) jescape(&detail, (*selection)->identity.manifest);
    else buf_appends(&detail, "null");
    if ((*selection)->identity.job) {
        buf_appends(&detail, ",\"job\":");
        tny_image_job_json((*selection)->identity.job, &detail);
    }
    buf_appends(&detail, ",\"path\":");
    jescape(&detail, (*selection)->path);
    buf_appends(&detail, ",\"sha256\":");
    jescape(&detail, (*selection)->identity.sha256);
    buf_appends(&detail, ",\"operation_id\":");
    jescape(&detail, (*selection)->identity.operation_id);
    buf_appends(&detail, "}");
    preview_detail(env, true, &detail);
    if (detail.oom) {
        buf_free(&detail);
        *error = tool_err("out of memory preparing preview");
        return NULL;
    }
    return buf_detach(&detail);
}

char *tool_image_preview_execute(tools_env *env, const tny_image_preview_selection *selection) {
    if (!selection) return tool_err("missing approved preview selection");
    tny_image_preview_result preview;
    tny_image_preview_coordinate(true, &selection->identity, env->preview_admit, env->preview_ud,
                                 &preview);
    bool queued = preview.status == TNY_IMAGE_PREVIEW_QUEUED;
    buf_t out;
    buf_init(&out);
    /* Keep the established error marker without passing unbounded lineage
     * through tool_err's fixed-size diagnostic formatter. */
    if (!queued) buf_appends(&out, "error: ");
    buf_appendf(&out, "{\"kind\":\"image_preview\",\"ok\":%s}", queued ? "true" : "false");
    tny_image_preview_append(&out, &selection->identity, &preview);
    char *result = out.oom ? NULL : buf_detach(&out);
    buf_free(&out);
    if (!result) return tool_err("preview result unavailable; no acknowledgment retry");
    return result;
}

/* ---- explicit local exports and contact sheets (ADR 0094) ---------------- */

typedef struct {
    char *paths[TNY_IMAGE_EXPORT_PATHS_MAX];
    char columns[16];
} export_strings;

static void free_export_strings(export_strings *owned) {
    for (size_t i = 0; i < TNY_IMAGE_EXPORT_PATHS_MAX; i++) free(owned->paths[i]);
}

/* An ordered list of {"image": PATH} / {"artifact": RECORD} objects, so the
 * sheet's order is exactly the order the caller wrote and an artifact may
 * appear anywhere in it. */
static bool export_sources(tools_env *env, yyjson_val *args, tny_image_export_request *r,
                           export_strings *owned, size_t *slot) {
    yyjson_val *sources = jget(args, "sources");
    if (!sources || !yyjson_is_arr(sources) || !yyjson_arr_size(sources) ||
        yyjson_arr_size(sources) > TNY_IMAGE_EXPORT_SOURCES_MAX)
        return false;
    size_t i, n;
    yyjson_val *v;
    yyjson_arr_foreach(sources, i, n, v) {
        bool ok = true;
        if (!yyjson_is_obj(v)) return false;
        const char *image = text(v, "image", &ok);
        const char *artifact = text(v, "artifact", &ok);
        const char *value = image ? image : artifact;
        if (!ok || (image && artifact) || !value || !*value) return false;
        char *patherr = NULL;
        owned->paths[*slot] = tool_resolve_path(env, value, &patherr);
        free(patherr);
        if (!owned->paths[*slot]) return false;
        r->source_is_artifact[r->source_count] = artifact != NULL;
        r->sources[r->source_count++] = owned->paths[(*slot)++];
    }
    return true;
}

static bool export_args(tools_env *env, yyjson_val *args, bool sheet, tny_image_export_request *r,
                        export_strings *owned, char *err, size_t len) {
    bool ok = true;
    *r = (tny_image_export_request){
        .sheet = sheet, .cancelled = env->cancelled, .userdata = env->cancelled_ud};
    r->size = text(args, "size", &ok);
    r->policy = text(args, "fit", &ok);
    r->gravity = text(args, "gravity", &ok);
    r->background = text(args, "background", &ok);
    r->format = text(args, "format", &ok);
    r->labels = text(args, "labels", &ok);
    r->preview = flag(args, "preview", false, &ok);
    r->overwrite = flag(args, "overwrite", false, &ok);
    r->no_manifest = !flag(args, "persist_manifest", true, &ok);
    const char *output = text(args, "output_file", &ok);
    yyjson_val *columns = jget(args, "columns");
    if (columns && !yyjson_is_null(columns)) {
        if (!yyjson_is_uint(columns) || !sheet) goto bad;
        snprintf(owned->columns, sizeof owned->columns, "%llu",
                 (unsigned long long)yyjson_get_uint(columns));
        r->columns = owned->columns;
    }
    if (!ok || !output || !*output || (!sheet && r->labels)) goto bad;
    char *patherr = NULL;
    owned->paths[0] = tool_resolve_path(env, output, &patherr);
    free(patherr);
    if (!owned->paths[0]) goto bad;
    r->output_file = owned->paths[0];
    size_t slot = 1;
    if (!export_sources(env, args, r, owned, &slot)) goto bad;
    return true;
bad:
    snprintf(err, len,
             "invalid export arguments: output_file and WIDTHxHEIGHT size strings, an ordered "
             "sources list of {\"image\":PATH} or {\"artifact\":RECORD} entries, and valid "
             "fit/gravity/background/format%s options",
             sheet ? "/columns/labels" : "");
    return false;
}

/* A path that came out of a record stays inside the directories this runtime
 * already allows, exactly as it does for generation references; a path the
 * caller typed is governed by the grant alone. */
static bool export_inputs_allowed(tools_env *env, const tny_image_export_plan *plan, char *err,
                                  size_t len) {
    for (size_t i = 0;; i++) {
        bool artifact = false;
        const char *path = tny_image_export_plan_input(plan, i, &artifact);
        if (!path) return true;
        if (artifact && !perm_path_allowed(env->ctx, path)) {
            snprintf(err, len, "image manifest references a file outside the allowed directories");
            return false;
        }
    }
}

static tny_image_export_plan *export_plan(tools_env *env, const tny_image_export_request *r,
                                          char *err, size_t len, int *status) {
    *status = 1;
    tny_image_export_plan *plan = tny_image_export_plan_new(r, err, len);
    if (!plan) return NULL;
    if (!export_inputs_allowed(env, plan, err, len)) {
        tny_image_export_plan_free(plan);
        return NULL;
    }
    *status = tny_image_export_plan_capture(plan, err, len);
    if (*status) {
        tny_image_export_plan_free(plan);
        return NULL;
    }
    return plan;
}

static bool export_identity(tools_env *env, yyjson_val *args, bool sheet, buf_t *detail, char *err,
                            size_t len) {
    tny_image_export_request r = {0};
    export_strings owned = {0};
    tny_image_export_plan *plan = NULL;
    int status = 1;
    if (export_args(env, args, sheet, &r, &owned, err, len))
        plan = export_plan(env, &r, err, len, &status);
    bool ok = plan && tny_image_export_plan_detail(plan, detail) == 0;
    if (ok) preview_detail(env, r.preview, detail);
    ok = ok && !detail->oom;
    if (plan && !ok) snprintf(err, len, "cannot format export permission identity");
    tny_image_export_plan_free(plan);
    free_export_strings(&owned);
    return ok;
}

char *tool_image_export_detail(tools_env *env, yyjson_val *args, bool sheet, char **error) {
    char err[256];
    buf_t b;
    buf_init(&b);
    if (!export_identity(env, args, sheet, &b, err, sizeof err)) {
        buf_free(&b);
        buf_init(&b);
        *error = tool_err("%s", err);
    }
    return buf_detach(&b);
}

int tool_image_export_run(tools_env *env, yyjson_val *args, bool sheet, buf_t *out, char *err,
                          size_t len, const char *approved_detail) {
    if (env->ctx->library_mode || env->ctx->ssh_host || !tny_image_export_supported()) {
        snprintf(err, len,
                 "image exports require a local CLI runtime; unavailable over --ssh, libtny or "
                 "wasm");
        return 1;
    }
    tny_image_export_request r = {0};
    tny_image_export_result result = {0};
    export_strings owned = {0};
    int rc = 1;
    tny_image_export_plan *plan = NULL;
    if (export_args(env, args, sheet, &r, &owned, err, len))
        plan = export_plan(env, &r, err, len, &rc);
    if (plan) {
        rc = 1;
        bool allowed = true;
        /* Revalidate the execution snapshot against the grant, then stage
         * those same owned bytes and lineage, not a later pathname read. */
        if (env->perm) {
            buf_t detail;
            buf_init(&detail);
            const char *tool = sheet ? "image_contact_sheet" : "image_export";
            allowed = tny_image_export_plan_detail(plan, &detail) == 0;
            if (allowed) preview_detail(env, r.preview, &detail);
            allowed = allowed && !detail.oom &&
                      ((approved_detail && strcmp(approved_detail, detail.data) == 0) ||
                       perm_check(env->perm, tool, detail.data) == PERM_ALLOW);
            if (!allowed)
                snprintf(err, len,
                         "this export's sources, record or destination changed after the call was "
                         "approved; request it again");
            buf_free(&detail);
        }
        if (allowed) {
            rc = tny_image_export_plan_run(env->ctx, plan, &result, err, len);
            if (!rc) tny_image_export_result_json(&r, &result, out);
            /* The derived artifact really is on disk: report it truthfully at
             * this surface too, still as a failure. */
            else if (tny_image_export_retained(&result))
                tny_image_export_retained_json(&r, &result, err, out);
            else if (r.preview) tny_image_export_error_json(&r, &result, err, out);
            tny_image_preview_identity id = {
                result.output, result.sha256, result.manifest_path, result.operation_id, true,
                NULL,          NULL};
            preview_result(env, r.preview, rc == 0, &id, out);
            if (out->oom) {
                buf_free(out);
                buf_init(out);
                if (!rc) {
                    snprintf(err, len,
                             "the image was exported to %s and kept, but its result "
                             "could not be formatted",
                             result.output);
                    rc = 1;
                }
            }
        }
    }
    tny_image_export_plan_free(plan);
    free_export_strings(&owned);
    return rc;
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
        else if (tny_image_strict_failure(&result) || r.preview)
            tny_image_error_json(&r, &result, err, out);
        tny_image_preview_identity id = {
            r.output_file, result.sha256, result.manifest_path, result.operation_id, false,
            NULL,          NULL};
        preview_result(env, r.preview, rc == 0, &id, out);
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

char *tool_image_export_execute(tools_env *env, yyjson_val *args, bool sheet,
                                const char *approved_detail) {
    buf_t out;
    buf_init(&out);
    char err[256] = "";
    int rc = tool_image_export_run(env, args, sheet, &out, err, sizeof err, approved_detail);
    if (rc) {
        /* Any postcommit failure must name the retained artifact. A failure
         * before commit keeps the ordinary marker with no artifact claim. */
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
    return buf_detach(&out);
}
