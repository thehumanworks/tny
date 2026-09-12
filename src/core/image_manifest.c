#include "core/image_manifest.h"
#include "json/json.h"
#include "util/image_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MANIFEST_SUFFIX ".tny-image-"
#define MANIFEST_EXT    ".json"

char *tny_image_manifest_path(const char *output, const char *operation_id) {
    if (!output || !operation_id || !*operation_id) return NULL;
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "%s" MANIFEST_SUFFIX "%s" MANIFEST_EXT, output, operation_id);
    char *path = b.oom ? NULL : buf_detach(&b);
    buf_free(&b);
    return path;
}

static bool hex_run(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return n > 0;
}

/* Anchored on the suffix tny actually appends, not on the first marker in the
 * name: an output the caller named `shot.tny-image-1.png` still gets a real
 * record beside it, and that record's own final `.tny-image-<hex>.json` must
 * stay reserved. Trailing text after the id, an empty id and ordinary names
 * are not reserved — this protects written records, not file names. */
bool tny_image_manifest_reserved_name(const char *path) {
    if (!path) return false;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    size_t n = strlen(base);
    const size_t ext = sizeof MANIFEST_EXT - 1;
    if (n <= ext || strcmp(base + n - ext, MANIFEST_EXT) != 0) return false;
    const char *end = base + n - ext; /* one past the operation id */
    for (const char *mark = base; (mark = strstr(mark, MANIFEST_SUFFIX)) != NULL; mark++) {
        const char *id = mark + sizeof MANIFEST_SUFFIX - 1;
        if (id <= end && hex_run(id, (size_t)(end - id))) return true;
    }
    return false;
}

/* ---- serialization ---- */

static void text_field(buf_t *b, const char *key, const char *value) {
    buf_appendf(b, ",\"%s\":", key);
    if (value) jescape(b, value);
    else buf_appends(b, "null");
}

static void dimension_fields(buf_t *b, uint32_t width, uint32_t height) {
    if (width && height) buf_appendf(b, "\"width\":%u,\"height\":%u", width, height);
    else buf_appends(b, "\"width\":null,\"height\":null");
}

void tny_image_manifest_serialize(const tny_image_record *r, buf_t *out) {
    buf_appendf(out, "{\"version\":%d,\"kind\":\"image_manifest\",\"operation_id\":",
                TNY_IMAGE_MANIFEST_VERSION);
    jescape(out, r->operation_id);
    buf_appends(out, ",\"operation\":");
    jescape(out, r->edit ? "edit" : "generate");
    text_field(out, "status", r->status);
    text_field(out, "workspace", r->workspace);
    text_field(out, "started", r->started);
    text_field(out, "finished", r->finished);
    text_field(out, "prompt", r->prompt);
    text_field(out, "output", r->output);
    buf_appendf(out, ",\"committed\":%s,\"references\":[", r->committed ? "true" : "false");
    for (size_t i = 0; i < r->reference_count; i++) {
        const tny_image_reference *ref = &r->references[i];
        if (i) buf_appends(out, ",");
        buf_appends(out, "{\"path\":");
        jescape(out, ref->path);
        buf_appends(out, ",\"sha256\":");
        jescape(out, ref->sha256);
        text_field(out, "source_manifest", ref->source_manifest);
        text_field(out, "source_operation", *ref->source_operation ? ref->source_operation : NULL);
        buf_appends(out, "}");
    }
    buf_appends(out, "],\"requested\":{\"provider\":");
    jescape(out, r->requested_provider);
    text_field(out, "model", r->requested_model);
    text_field(out, "quality", r->requested_quality);
    text_field(out, "size", r->requested_size);
    buf_appends(out, "},\"effective\":{\"provider\":");
    if (r->effective_provider) jescape(out, r->effective_provider);
    else buf_appends(out, "null");
    text_field(out, "model", r->effective_model);
    text_field(out, "size", r->effective_size);
    buf_appends(out, "},\"result\":{");
    dimension_fields(out, r->width, r->height);
    text_field(out, "mime_type", r->mime);
    if (r->committed) buf_appendf(out, ",\"bytes\":%llu", (unsigned long long)r->bytes);
    else buf_appends(out, ",\"bytes\":null");
    text_field(out, "size_status", r->size_status);
    /* Only values the provider actually returned. Absence is null, never a
     * local operation id, a requested seed or a timestamp in disguise. */
    buf_appends(out, "},\"actual\":{\"seed\":");
    if (r->have_seed) buf_appendf(out, "%lld", (long long)r->seed);
    else buf_appends(out, "null");
    text_field(out, "request_id", r->request_id);
    buf_appends(out, "},\"error\":");
    if (r->error_code) {
        buf_appends(out, "{\"code\":");
        jescape(out, r->error_code);
        text_field(out, "message", r->error_message);
        buf_appends(out, "}");
    } else buf_appends(out, "null");
    buf_appends(out, ",\"source\":");
    if (r->source_manifest) {
        buf_appends(out, "{\"manifest\":");
        jescape(out, r->source_manifest);
        text_field(out, "operation_id", r->source_operation);
        buf_appends(out, "}");
    } else buf_appends(out, "null");
    /* A successful operation has exactly one artifact, and this service never
     * resizes or re-encodes, so it is always the provider's own native bytes.
     * Derived export copies arrive with the separate export slice (#125). */
    buf_appends(out, ",\"artifacts\":[");
    if (r->committed && r->output_sha256) {
        buf_appends(out, "{\"role\":\"native\",\"path\":");
        jescape(out, r->output);
        buf_appends(out, ",\"sha256\":");
        jescape(out, r->output_sha256);
        buf_appends(out, ",");
        dimension_fields(out, r->width, r->height);
        text_field(out, "mime_type", r->mime);
        buf_appendf(out, ",\"bytes\":%llu,\"transform\":null", (unsigned long long)r->bytes);
        text_field(out, "source_operation", r->source_operation);
        buf_appends(out, "}");
    }
    buf_appends(out, "]}\n");
}

/* ---- parsing ---- */

static char *owned(yyjson_val *v, size_t max, bool required, bool *ok) {
    if (!v || yyjson_is_null(v)) {
        if (required) *ok = false;
        return NULL;
    }
    size_t n = yyjson_get_len(v);
    const char *s = yyjson_get_str(v);
    if (!s || !n || n > max || !utf8_valid_bytes(s, n)) {
        *ok = false;
        return NULL;
    }
    char *copy = xstrndup(s, n);
    if (!copy) *ok = false;
    return copy;
}

static char *field(yyjson_val *obj, const char *key, size_t max, bool required, bool *ok) {
    return owned(jget(obj, key), max, required, ok);
}

static void hash_field(yyjson_val *obj, const char *key, char out[TNY_IMAGE_SHA256_HEX], bool *ok) {
    yyjson_val *v = jget(obj, key);
    const char *s = yyjson_get_str(v);
    out[0] = 0;
    if (!s || yyjson_get_len(v) != 64 || !hex_run(s, 64)) {
        *ok = false;
        return;
    }
    memcpy(out, s, 64);
    out[64] = 0;
}

static void id_field(yyjson_val *obj, const char *key, char out[TNY_IMAGE_OPERATION_ID_MAX],
                     bool required, bool *ok) {
    yyjson_val *v = jget(obj, key);
    out[0] = 0;
    if (!v || yyjson_is_null(v)) {
        if (required) *ok = false;
        return;
    }
    const char *s = yyjson_get_str(v);
    size_t n = yyjson_get_len(v);
    if (!s || n >= TNY_IMAGE_OPERATION_ID_MAX || !hex_run(s, n)) {
        *ok = false;
        return;
    }
    memcpy(out, s, n);
    out[n] = 0;
}

static uint32_t dimension(yyjson_val *obj, const char *key, bool *ok) {
    yyjson_val *v = jget(obj, key);
    if (!v || yyjson_is_null(v)) return 0;
    if (!yyjson_is_uint(v) || yyjson_get_uint(v) == 0 || yyjson_get_uint(v) > UINT32_MAX) {
        *ok = false;
        return 0;
    }
    return (uint32_t)yyjson_get_uint(v);
}

static bool one_of(const char *value, const char *const *names, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (value && strcmp(value, names[i]) == 0) return true;
    return false;
}

static bool parse_references(tny_image_manifest *m, yyjson_val *root) {
    yyjson_val *list = jget(root, "references");
    if (!list || yyjson_is_null(list)) return true;
    if (!yyjson_is_arr(list) || yyjson_arr_size(list) > TNY_IMAGE_REFERENCES_MAX) return false;
    size_t i, n;
    yyjson_val *v;
    bool ok = true;
    yyjson_arr_foreach(list, i, n, v) {
        tny_image_reference *ref = &m->references[m->reference_count];
        if (!yyjson_is_obj(v)) return false;
        ref->path = field(v, "path", TNY_IMAGE_IO_PATH_MAX, true, &ok);
        hash_field(v, "sha256", ref->sha256, &ok);
        ref->source_manifest = field(v, "source_manifest", TNY_IMAGE_IO_PATH_MAX, false, &ok);
        id_field(v, "source_operation", ref->source_operation, false, &ok);
        memcpy(ref->expected, ref->sha256, sizeof ref->expected);
        m->reference_count++;
        if (!ok) return false;
    }
    return ok;
}

static bool parse_artifacts(tny_image_manifest *m, yyjson_val *root) {
    yyjson_val *list = jget(root, "artifacts");
    if (!list || yyjson_is_null(list)) return true;
    if (!yyjson_is_arr(list) || yyjson_arr_size(list) > TNY_IMAGE_REFERENCES_MAX) return false;
    static const char *const roles[] = {"native", "derived"};
    size_t i, n;
    yyjson_val *v;
    bool ok = true;
    yyjson_arr_foreach(list, i, n, v) {
        if (!yyjson_is_obj(v)) return false;
        char *role = field(v, "role", 16, true, &ok);
        if (!ok || !one_of(role, roles, 2)) {
            free(role);
            return false;
        }
        /* Only the first native artifact is the replayable output; a derived
         * export copy is recorded but never offered as the native source. */
        if (m->artifact_path || strcmp(role, "native") != 0) {
            free(role);
            continue;
        }
        m->artifact_role = role;
        m->artifact_path = field(v, "path", TNY_IMAGE_IO_PATH_MAX, true, &ok);
        hash_field(v, "sha256", m->artifact_sha256, &ok);
        if (!ok) return false;
    }
    return ok;
}

tny_image_manifest *tny_image_manifest_load(const char *path, char *err, size_t errlen) {
    buf_t raw;
    buf_init(&raw);
    if (!path || !*path || tny_image_io_read_bounded(path, TNY_IMAGE_MANIFEST_MAX, &raw) != 0) {
        snprintf(err, errlen, "cannot read image manifest (regular JSON file up to 256 KiB)");
        buf_free(&raw);
        return NULL;
    }
    yyjson_doc *doc = jparse(raw.data, raw.len);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    tny_image_manifest *m = NULL;
    const char *kind = jget_str(root, "kind");
    yyjson_val *version = jget(root, "version");
    if (!yyjson_is_obj(root) || !kind || strcmp(kind, "image_manifest") != 0 ||
        !yyjson_is_uint(version)) {
        snprintf(err, errlen, "not a tny image manifest");
        goto done;
    }
    if (yyjson_get_uint(version) != TNY_IMAGE_MANIFEST_VERSION) {
        snprintf(err, errlen,
                 "image manifest schema version %llu is not supported by this build (expected %d)",
                 (unsigned long long)yyjson_get_uint(version), TNY_IMAGE_MANIFEST_VERSION);
        goto done;
    }
    m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(err, errlen, "out of memory reading image manifest");
        goto done;
    }
    static const char *const operations[] = {"generate", "edit"};
    static const char *const states[] = {"running", "succeeded", "failed", "cancelled"};
    bool ok = true;
    m->path = path_abs(path);
    id_field(root, "operation_id", m->operation_id, true, &ok);
    m->operation = field(root, "operation", 16, true, &ok);
    m->status = field(root, "status", 16, true, &ok);
    m->workspace = field(root, "workspace", TNY_IMAGE_IO_PATH_MAX, true, &ok);
    m->started = field(root, "started", 64, true, &ok);
    m->finished = field(root, "finished", 64, false, &ok);
    m->prompt = field(root, "prompt", TNY_IMAGE_PROMPT_MAX, true, &ok);
    m->output = field(root, "output", TNY_IMAGE_IO_PATH_MAX, true, &ok);
    m->committed = jget_bool(root, "committed", false);
    yyjson_val *requested = jget(root, "requested"), *effective = jget(root, "effective");
    yyjson_val *result = jget(root, "result");
    if (!ok || !m->path || !yyjson_is_obj(requested) || !yyjson_is_obj(effective) ||
        (result && !yyjson_is_obj(result)) || !one_of(m->operation, operations, 2) ||
        !one_of(m->status, states, 4) || *m->workspace != '/') {
        snprintf(err, errlen, "image manifest is missing or misdeclares a required field");
        goto invalid;
    }
    m->requested_provider = field(requested, "provider", 128, true, &ok);
    m->requested_model = field(requested, "model", 128, false, &ok);
    m->requested_quality = field(requested, "quality", 16, false, &ok);
    m->requested_size = field(requested, "size", 32, false, &ok);
    m->effective_provider = field(effective, "provider", 128, false, &ok);
    m->effective_model = field(effective, "model", 128, false, &ok);
    m->effective_size = field(effective, "size", 32, false, &ok);
    if (result) {
        m->width = dimension(result, "width", &ok);
        m->height = dimension(result, "height", &ok);
        m->mime = field(result, "mime_type", 64, false, &ok);
        m->size_status = field(result, "size_status", 32, false, &ok);
        yyjson_val *bytes = jget(result, "bytes");
        if (bytes && !yyjson_is_null(bytes)) {
            if (!yyjson_is_uint(bytes)) ok = false;
            else m->bytes = yyjson_get_uint(bytes);
        }
    }
    if (!ok || !parse_references(m, root) || !parse_artifacts(m, root)) {
        snprintf(err, errlen, "image manifest has an invalid reference, artifact or setting");
        goto invalid;
    }
    /* A record that claims a committed output must actually name and hash it. */
    if (m->committed && (!m->artifact_path || !*m->artifact_sha256)) {
        snprintf(err, errlen, "image manifest claims a committed artifact without a hashed path");
        goto invalid;
    }
    yyjson_doc_free(doc);
    buf_free(&raw);
    return m;
invalid:
    tny_image_manifest_free(m);
    m = NULL;
done:
    yyjson_doc_free(doc);
    buf_free(&raw);
    return m;
}

void tny_image_manifest_free(tny_image_manifest *m) {
    if (!m) return;
    free(m->path);
    free(m->operation);
    free(m->status);
    free(m->workspace);
    free(m->started);
    free(m->finished);
    free(m->prompt);
    free(m->output);
    for (size_t i = 0; i < TNY_IMAGE_REFERENCES_MAX; i++) {
        free(m->references[i].path);
        free(m->references[i].source_manifest);
    }
    free(m->requested_provider);
    free(m->requested_model);
    free(m->requested_quality);
    free(m->requested_size);
    free(m->effective_provider);
    free(m->effective_model);
    free(m->effective_size);
    free(m->artifact_path);
    free(m->artifact_role);
    free(m->mime);
    free(m->size_status);
    free(m);
}

const char *tny_image_manifest_observed_status(const tny_image_manifest *m) {
    if (!m || !m->status) return "unknown";
    if (strcmp(m->status, "running") != 0) return m->status;
    char owner[TNY_IMAGE_IO_ID_MAX];
    /* A live guard proves nothing unless it is held for *this* operation: a
     * later operation writing the same destination must not make an abandoned
     * intent look alive. An unreadable holder record stays "running" rather
     * than inventing either answer. */
    if (!tny_image_io_guard_owner(m->output, owner)) return "interrupted";
    if (*owner && strcmp(owner, m->operation_id) != 0) return "interrupted";
    return "running";
}

char *tny_image_manifest_resolve(const tny_image_manifest *m, const char *path) {
    if (!m || !path || !*path) return NULL;
    if (*path == '/') return xstrdup(path);
    return path_join(m->workspace, path);
}
