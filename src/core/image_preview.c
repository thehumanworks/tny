/* image_preview.c — see core/image_preview.h (docs/adr/0096). */
#include "core/image_preview.h"

#include "util/image_io.h"
#include "core/image_manifest.h"
#include "json/json.h"
#include <stdlib.h>
#include <stdio.h>

#include <string.h>

const char *tny_image_preview_status_name(tny_image_preview_status status) {
    switch (status) {
    case TNY_IMAGE_PREVIEW_QUEUED: return "queued";
    case TNY_IMAGE_PREVIEW_UNSUPPORTED: return "unsupported";
    case TNY_IMAGE_PREVIEW_UNAVAILABLE_SESSION: return "unavailable_session";
    case TNY_IMAGE_PREVIEW_TURN_NOT_READY: return "turn_not_ready";
    case TNY_IMAGE_PREVIEW_FAILED: return "failed";
    case TNY_IMAGE_PREVIEW_NOT_ATTEMPTED: return "not_attempted";
    }
    return "failed";
}

bool tny_image_preview_hash_valid(const char *hex) {
    if (!hex) return false;
    size_t i = 0;
    for (; hex[i]; i++) {
        if (i >= 64) return false;
        char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return i == 64;
}

bool tny_image_preview_hash_matches(const uint8_t *data, size_t len, const char *expect) {
    if (!tny_image_preview_hash_valid(expect) || !data) return false;
    char actual[65];
    if (!tny_image_io_sha256_hex(data, len, actual)) return false;
    return memcmp(actual, expect, 64) == 0;
}

void tny_image_preview_coordinate(bool success, const tny_image_preview_identity *id,
                                  tny_image_preview_admit admit, void *ud,
                                  tny_image_preview_result *result) {
    *result = (tny_image_preview_result){.status = TNY_IMAGE_PREVIEW_NOT_ATTEMPTED};
    if (!success) {
        snprintf(result->code, sizeof result->code, "operation_failed");
        return;
    }
    if (!id || !id->path || !tny_image_preview_hash_valid(id->sha256)) {
        result->status = TNY_IMAGE_PREVIEW_FAILED;
        snprintf(result->code, sizeof result->code, "%s", TNY_IMAGE_PREVIEW_CODE_HASH);
        return;
    }
    if (!admit) {
        result->status = TNY_IMAGE_PREVIEW_UNAVAILABLE_SESSION;
        snprintf(result->code, sizeof result->code, "%s", TNY_IMAGE_PREVIEW_CODE_NO_SESSION);
        return;
    }
    result->status = admit(ud, id, result);
}

const char *tny_image_preview_fallback(const tny_image_preview_result *r) {
    if (r->status == TNY_IMAGE_PREVIEW_QUEUED)
        return "Queued for the next request only; not proof of delivery or inspection.";
    if (strcmp(r->code, TNY_IMAGE_PREVIEW_CODE_TOO_LARGE) == 0)
        return "Explicitly export a smaller separate artifact, then select its manifest for "
               "preview.";
    return "Artifact retained if committed. No retry or attachment fallback was attempted. "
           "Check image_input, allowed roots and an active native tool batch before a new explicit "
           "preview.";
}

void tny_image_preview_append(buf_t *out, const tny_image_preview_identity *id,
                              const tny_image_preview_result *r) {
    if (out->oom || !out->len) return;
    size_t end = out->len;
    while (end && (out->data[end - 1] == '\n' || out->data[end - 1] == ' ')) end--;
    if (!end || out->data[end - 1] != '}') return;
    out->len = end - 1;
    out->data[out->len] = 0;
    buf_appends(out, ",\"preview\":{\"status\":");
    jescape(out, tny_image_preview_status_name(r->status));
    buf_appends(out, ",\"error_code\":");
    if (*r->code) jescape(out, r->code);
    else buf_appends(out, "null");
    buf_appends(out, ",\"receipt_id\":");
    if (*r->receipt) jescape(out, r->receipt);
    else buf_appends(out, "null");
    buf_appends(out, ",\"representation\":\"original_bytes\",\"selected\":{\"path\":");
    jescape(out, id->path ? id->path : "");
    buf_appends(out, ",\"sha256\":");
    jescape(out, id->sha256 ? id->sha256 : "");
    buf_appends(out, ",\"manifest_path\":");
    if (id->manifest && *id->manifest) jescape(out, id->manifest);
    else buf_appends(out, "null");
    buf_appends(out, ",\"operation_id\":");
    if (id->operation_id && *id->operation_id) jescape(out, id->operation_id);
    else buf_appends(out, "null");
    buf_appendf(out, ",\"derived\":%s", id->derived ? "true" : "false");
    if (id->job && *id->job->id) {
        buf_appends(out, ",\"job\":");
        tny_image_job_json(id->job, out);
    }
    if (id->record) {
        const tny_image_manifest *m = id->record;
        buf_appends(out, ",\"lineage\":{\"operation\":");
        jescape(out, m->operation);
        buf_appends(out, ",\"transform_policy\":");
        if (m->transform_policy) jescape(out, m->transform_policy);
        else buf_appends(out, "null");
        buf_appends(out, ",\"transform_format\":");
        if (m->transform_format) jescape(out, m->transform_format);
        else buf_appends(out, "null");
        buf_appendf(out, ",\"canvas_width\":%u,\"canvas_height\":%u,\"sources\":[", m->canvas_width,
                    m->canvas_height);
        size_t count = m->derived ? m->source_count : m->reference_count;
        const tny_image_reference *sources = m->derived ? m->sources : m->references;
        for (size_t i = 0; i < count; i++) {
            buf_appends(out, i ? ",{\"path\":" : "{\"path\":");
            jescape(out, sources[i].path);
            buf_appends(out, ",\"sha256\":");
            jescape(out, sources[i].sha256);
            if (*sources[i].job.id) {
                buf_appends(out, ",\"job\":");
                tny_image_job_json(&sources[i].job, out);
            }
            buf_appends(out, ",\"source_manifest\":");
            if (sources[i].source_manifest) jescape(out, sources[i].source_manifest);
            else buf_appends(out, "null");
            buf_appends(out, ",\"source_operation\":");
            if (*sources[i].source_operation) jescape(out, sources[i].source_operation);
            else buf_appends(out, "null");
            buf_appends(out, "}");
        }
        buf_appends(out, "]}");
    }
    buf_appends(out, "},\"fallback\":");
    jescape(out, tny_image_preview_fallback(r));
    buf_appends(out, "}}\n");
}

tny_image_preview_selection *tny_image_preview_select(const tny_ctx *ctx, const char *path,
                                                      const char *job, int item, char *err,
                                                      size_t len) {
    if (!!path == !!job || (job && item < 0) || (!job && item >= 0)) {
        snprintf(err, len, "preview requires exactly a manifest or a complete job/item selector");
        return NULL;
    }
    tny_image_preview_selection *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (job) {
        s->artifact = tny_jobs_select_artifact(ctx, job, item, err, len);
        tny_job_artifact *a = s->artifact;
        if (!a) {
            tny_image_preview_selection_free(s);
            return NULL;
        }
        s->path = a->path;
        a->path = NULL;
        s->manifest = a->manifest;
        a->manifest = NULL;
        snprintf(s->job.id, sizeof s->job.id, "%s", a->job_id);
        s->job.bytes = a->bytes;
        s->job.item_index = a->item_index;
        s->job.projection_attempt = a->projection_attempt;
        s->job.item_attempt = a->item_attempt;
        s->job.carried_from_attempt = a->carried_from_attempt;
        s->identity =
            (tny_image_preview_identity){.path = s->path,
                                         .sha256 = a->sha256,
                                         .manifest = s->manifest ? s->manifest->path : NULL,
                                         .operation_id = a->operation_id,
                                         .record = s->manifest,
                                         .job = &s->job};
        return s;
    }
    s->manifest = tny_image_manifest_load(path, err, len);
    tny_image_manifest *m = s->manifest;
    if (!m || !m->committed || !m->status || strcmp(m->status, "succeeded") != 0 ||
        !m->artifact_path || !tny_image_preview_hash_valid(m->artifact_sha256)) {
        if (m && err && len) snprintf(err, len, "preview requires a successful committed artifact");
        tny_image_preview_selection_free(s);
        return NULL;
    }
    s->path = tny_image_manifest_resolve(m, m->artifact_path);
    if (!s->path) {
        tny_image_preview_selection_free(s);
        return NULL;
    }
    s->identity = (tny_image_preview_identity){
        s->path, m->artifact_sha256, m->path, m->operation_id, m->derived, m, NULL};
    return s;
}

int tny_image_preview_options(int argc, char **argv, const char **manifest, const char **job,
                              int *item, bool *json) {
    if (!argc || strcmp(argv[0], "preview") != 0) return 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            *json = true;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) return -1;
        if (i + 1 >= argc || !*argv[i + 1]) return 1;
        if (strcmp(argv[i], "--manifest") == 0 && !*manifest) *manifest = argv[++i];
        else if (strcmp(argv[i], "--job") == 0 && !*job) *job = argv[++i];
        else if (strcmp(argv[i], "--item") == 0 && *item < 0) {
            unsigned int value = 0;
            for (const char *p = argv[++i]; *p; p++) {
                if (*p < '0' || *p > '9' || value >= TNY_JOBS_MAX_ITEMS) return 1;
                value = value * 10 + (unsigned int)(*p - '0');
            }
            if (value >= TNY_JOBS_MAX_ITEMS) return 1;
            *item = (int)value;
        } else return 1;
    }
    return (!!*manifest != !!*job && (*job ? (*item >= 0 && tny_jobs_valid_id(*job)) : *item < 0))
               ? 0
               : 1;
}

void tny_image_preview_selection_free(tny_image_preview_selection *s) {
    if (!s) return;
    tny_image_manifest_free(s->manifest);
    free(s->path);
    tny_jobs_artifact_free(s->artifact);
    free(s);
}
