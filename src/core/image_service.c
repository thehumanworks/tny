#include "core/image_provider.h"
#include "core/image.h"
#include "core/image_manifest.h"
#include "json/json.h"
#include "util/image_io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const tny_image_provider *const providers[] = {&tny_image_codex};
static const tny_image_provider *find_provider(const char *name) {
    if (!name) name = "codex";
    for (size_t i = 0; i < sizeof providers / sizeof providers[0]; i++)
        if (strcmp(name, providers[i]->name) == 0) return providers[i];
    return NULL;
}

bool tny_image_stopped(const tny_image_request *r) {
    return r->cancelled && r->cancelled(r->userdata);
}

bool tny_image_available(const tny_ctx *ctx, const char *name, bool edit, char *err, size_t len) {
    const tny_image_provider *p = find_provider(name);
    const char *why = !p                           ? "unknown image provider"
                      : edit && !p->max_references ? "image provider does not support editing"
                                                   : NULL;
    if (why) {
        if (err && len) snprintf(err, len, "%s", why);
        return false;
    }
    return p->available(ctx, err, len);
}

bool tny_image_capabilities(const tny_ctx *ctx, bool edit, buf_t *names) {
    bool found = false;
    for (size_t i = 0; i < sizeof providers / sizeof providers[0]; i++) {
        const tny_image_provider *p = providers[i];
        if ((edit && !p->max_references) || !p->available(ctx, NULL, 0)) continue;
        if (names) {
            if (found) buf_appends(names, ", ");
            buf_appends(names, p->name);
        }
        found = true;
    }
    return found;
}

static bool valid_string(const char *s, size_t max) {
    return s && *s && strlen(s) <= max && utf8_valid_bytes(s, strlen(s));
}

static bool valid_prompt(const char *s) {
    return valid_string(s, TNY_IMAGE_PROMPT_MAX) && str_ws_prefix(s, strlen(s)) != strlen(s);
}

static bool valid_quality(const char *q) {
    return !q || strcmp(q, "auto") == 0 || strcmp(q, "low") == 0 || strcmp(q, "medium") == 0 ||
           strcmp(q, "high") == 0 || strcmp(q, "xhigh") == 0 || strcmp(q, "max") == 0;
}

int tny_image_options(int argc, char **argv, tny_image_request *r, bool *json, bool *check) {
    if (argc < 1) return 1;
    r->edit = strcmp(argv[0], "edit") == 0;
    r->replay = strcmp(argv[0], "replay") == 0;
    if (!r->edit && !r->replay && strcmp(argv[0], "generate") != 0) return 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) return -1;
        if (strcmp(a, "--json") == 0) {
            *json = true;
            continue;
        }
        if (strcmp(a, "--check") == 0) {
            *check = true;
            continue;
        }
        if (strcmp(a, "--strict-size") == 0) {
            r->strict_size = true;
            continue;
        }
        if (strcmp(a, "--no-manifest") == 0) {
            r->no_manifest = true;
            continue;
        }
        const char **slot = strcmp(a, "--image-provider") == 0 ? &r->provider
                            : strcmp(a, "--model") == 0        ? &r->model
                            : strcmp(a, "--quality") == 0      ? &r->quality
                            : strcmp(a, "--size") == 0         ? &r->size
                            : strcmp(a, "--output-file") == 0  ? &r->output_file
                            : strcmp(a, "--manifest") == 0     ? &r->from_manifest
                                                               : NULL;
        /* --image and --artifact share one ordered reference list, so a mixed
         * command keeps the order the caller wrote and the same maximum. */
        if ((strcmp(a, "--image") == 0 || strcmp(a, "--artifact") == 0) &&
            r->image_count < TNY_IMAGE_REFERENCES_MAX) {
            r->image_is_artifact[r->image_count] = strcmp(a, "--artifact") == 0;
            slot = &r->images[r->image_count++];
        }
        if (!slot || *slot || i + 1 >= argc || !*argv[i + 1]) return 1;
        *slot = argv[++i];
    }
    /* `replay` reruns one recorded operation; `generate`/`edit` never take a
     * replay source, and a replay never takes new references. */
    if (r->replay != (r->from_manifest != NULL)) return 1;
    if (r->replay && r->image_count) return 1;
    return 0;
}

static int base64_digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int tny_image_decode(const char *s, size_t n, buf_t *out, char *err, size_t len) {
    if (!s || !n || n % 4 || n > ((TNY_IMAGE_OUTPUT_MAX + 2u) / 3u) * 4u) goto invalid;
    size_t padding = (s[n - 1] == '=') + (size_t)(s[n - 2] == '=');
    size_t decoded = n / 4 * 3 - padding;
    if (decoded > TNY_IMAGE_OUTPUT_MAX) goto invalid;
    for (size_t i = 0; i < n - padding; i++)
        if (base64_digit((unsigned char)s[i]) < 0) goto invalid;
    if ((padding == 1 && (base64_digit((unsigned char)s[n - 2]) & 3)) ||
        (padding == 2 && (base64_digit((unsigned char)s[n - 3]) & 15)))
        goto invalid;
    /* b64_decode is deliberately permissive elsewhere; the checks above make
     * it safe here without changing existing protocol consumers. */
    char *data = malloc(decoded + 1);
    if (!data) goto invalid;
    size_t got = b64_decode(s, (uint8_t *)data, decoded);
    const char *mime = image_mime((const uint8_t *)data, got);
    if (got != decoded || !mime || strcmp(mime, "image/gif") == 0) {
        free(data);
        goto invalid;
    }
    buf_append(out, data, got);
    free(data);
    if (out->oom) goto invalid;
    return 0;
invalid:
    snprintf(err, len, "invalid or oversized base64 image response (PNG/JPEG/WebP required)");
    return 1;
}

/* ---- reference and replay resolution ------------------------------------
 *
 * Everything below reads records only. No referenced image is opened here, so
 * parsing a manifest can never make tny read or upload an arbitrary file: the
 * bytes are loaded later, by the operation the caller explicitly asked for.
 */

void tny_image_plan_free(tny_image_plan *plan) {
    if (!plan) return;
    free(plan->prompt);
    free(plan->provider);
    free(plan->model);
    free(plan->quality);
    free(plan->size);
    for (size_t i = 0; i < TNY_IMAGE_REFERENCES_MAX; i++) {
        free(plan->references[i].path);
        free(plan->references[i].source_manifest);
    }
    tny_image_manifest_free(plan->source);
    memset(plan, 0, sizeof *plan);
}

/* Settings are copied, never borrowed: a plan outlives the request struct and
 * the JSON document its strings came from. NULL stays NULL — the absence of a
 * setting is itself part of the approved plan. */
static bool plan_setting(char **slot, const char *value) {
    free(*slot);
    *slot = value ? xstrdup(value) : NULL;
    return !value || *slot;
}

/* A record's own successful output becomes a reference only after its status
 * and hashed artifact path are both present. */
static int reference_from_record(tny_image_manifest *m, tny_image_reference *ref, char *err,
                                 size_t len) {
    const char *status = tny_image_manifest_observed_status(m);
    if (strcmp(status, "succeeded") != 0 || !m->committed || !m->artifact_path) {
        snprintf(err, len, "image manifest records no usable artifact (status %s)", status);
        return 1;
    }
    ref->path = tny_image_manifest_resolve(m, m->artifact_path);
    ref->source_manifest = xstrdup(m->path);
    snprintf(ref->source_operation, sizeof ref->source_operation, "%s", m->operation_id);
    memcpy(ref->expected, m->artifact_sha256, sizeof ref->expected);
    if (!ref->path || !ref->source_manifest) {
        snprintf(err, len, "out of memory resolving an image reference");
        return 1;
    }
    return 0;
}

int tny_image_plan_resolve(const tny_image_request *r, tny_image_plan *plan, char *err,
                           size_t errlen) {
    memset(plan, 0, sizeof *plan);
    plan->edit = r->edit;
    if (!plan_setting(&plan->prompt, r->prompt) || !plan_setting(&plan->provider, r->provider) ||
        !plan_setting(&plan->model, r->model) || !plan_setting(&plan->quality, r->quality) ||
        !plan_setting(&plan->size, r->size)) {
        snprintf(err, errlen, "out of memory resolving the image request");
        return 1;
    }
    if (r->from_manifest) {
        if (!valid_string(r->from_manifest, TNY_IMAGE_PATH_MAX)) {
            snprintf(err, errlen, "invalid image manifest path");
            return 1;
        }
        tny_image_manifest *m = tny_image_manifest_load(r->from_manifest, err, errlen);
        if (!m) return 1;
        plan->source = m;
        const char *status = tny_image_manifest_observed_status(m);
        if (strcmp(status, "succeeded") != 0) {
            snprintf(err, errlen, "cannot rerun an image operation recorded as %s", status);
            return 1;
        }
        bool recorded_edit = strcmp(m->operation, "edit") == 0;
        /* `tny image replay` adopts the recorded operation. A typed call names
         * its own operation, so a generate must not quietly become an upload. */
        if (r->replay) plan->edit = recorded_edit;
        else if (recorded_edit != r->edit) {
            snprintf(err, errlen, "this manifest records an image %s, not an image %s",
                     m->operation, r->edit ? "edit" : "generate");
            return 1;
        }
        /* Recorded defaults are copied in too, so the retained plan says what
         * this call will actually do without the record staying readable. */
        if ((!plan->prompt || !*plan->prompt) && !plan_setting(&plan->prompt, m->prompt)) goto oom;
        if (!plan->provider && !plan_setting(&plan->provider, m->requested_provider)) goto oom;
        if (!plan->model && !plan_setting(&plan->model, m->requested_model)) goto oom;
        if (!plan->quality && !plan_setting(&plan->quality, m->requested_quality)) goto oom;
        if (!plan->size && !plan_setting(&plan->size, m->requested_size)) goto oom;
        for (size_t i = 0; i < m->reference_count; i++) {
            tny_image_reference *ref = &plan->references[plan->reference_count++];
            ref->path = tny_image_manifest_resolve(m, m->references[i].path);
            ref->source_manifest =
                m->references[i].source_manifest ? xstrdup(m->references[i].source_manifest) : NULL;
            snprintf(ref->source_operation, sizeof ref->source_operation, "%s",
                     m->references[i].source_operation);
            memcpy(ref->expected, m->references[i].sha256, sizeof ref->expected);
            if (!ref->path || (m->references[i].source_manifest && !ref->source_manifest)) {
                snprintf(err, errlen, "out of memory resolving an image reference");
                return 1;
            }
        }
        return 0;
    }
    if (r->image_count > TNY_IMAGE_REFERENCES_MAX) {
        snprintf(err, errlen, "too many image references");
        return 1;
    }
    for (size_t i = 0; i < r->image_count; i++) {
        tny_image_reference *ref = &plan->references[plan->reference_count++];
        if (!valid_string(r->images[i], TNY_IMAGE_PATH_MAX)) {
            snprintf(err, errlen, "invalid reference path");
            return 1;
        }
        if (!r->image_is_artifact[i]) {
            ref->path = path_abs(r->images[i]);
            if (!ref->path) {
                snprintf(err, errlen, "invalid reference path");
                return 1;
            }
            continue;
        }
        tny_image_manifest *m = tny_image_manifest_load(r->images[i], err, errlen);
        if (!m) return 1;
        int rc = reference_from_record(m, ref, err, errlen);
        tny_image_manifest_free(m);
        if (rc) return rc;
    }
    return 0;
oom:
    snprintf(err, errlen, "out of memory resolving the image request");
    return 1;
}

/* Bounded read even if a file grows after fstat; avoid blocking on FIFOs. */
static int load_input(const tny_image_request *r, const char *path, tny_image_input *image,
                      char *err, size_t len) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    struct stat st;
    int rc = 1;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > TNY_IMAGE_INPUT_MAX)
        goto done;
    for (;;) {
        if (tny_image_stopped(r)) {
            rc = 130;
            goto done;
        }
        char chunk[8192];
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (n > 0 && (size_t)n > TNY_IMAGE_INPUT_MAX - image->data.len)) goto done;
        if (!n) break;
        buf_append(&image->data, chunk, (size_t)n);
        if (image->data.oom) goto done;
    }
    image->mime = image_mime((const uint8_t *)image->data.data, image->data.len);
    if (image->mime && strcmp(image->mime, "image/gif") != 0) rc = 0;
done:
    if (fd >= 0) close(fd);
    if (rc)
        snprintf(
            err, len,
            rc == 130
                ? "image operation interrupted"
                : "cannot load reference image (regular PNG/JPEG/WebP, at most 8 MiB required)");
    return rc;
}

/* Read each reference exactly once and hash those very bytes, so a pinned
 * hash cannot be checked against a preflight read and then replaced by a file
 * that changed before the upload. */
static int load_references(const tny_image_request *r, tny_image_plan *plan,
                           tny_image_input *inputs, char *err, size_t len) {
    for (size_t i = 0; i < plan->reference_count; i++) {
        tny_image_reference *ref = &plan->references[i];
        int rc = load_input(r, ref->path, &inputs[i], err, len);
        if (rc) return rc;
        if (!tny_image_io_sha256_hex(inputs[i].data.data, inputs[i].data.len, ref->sha256)) {
            snprintf(err, len, "cannot hash reference image");
            return 1;
        }
        if (*ref->expected && strcmp(ref->expected, ref->sha256) != 0) {
            snprintf(err, len,
                     "recorded reference no longer matches its hash; supply the reference "
                     "explicitly if this replacement is intended");
            return 1;
        }
    }
    return 0;
}

/* ---- persistence --------------------------------------------------------- */

typedef struct {
    char *canonical; /* normalized destination */
    char *manifest;  /* record path; NULL when persistence is off */
    char *workspace; /* base for relative recorded paths */
    char *started, *finished;
    tny_image_guard *guard;
    bool intent; /* a running record exists on disk */
} tny_image_operation;

static void operation_free(tny_image_operation *op) {
    tny_image_io_guard_release(op->guard);
    free(op->canonical);
    free(op->manifest);
    free(op->workspace);
    free(op->started);
    free(op->finished);
    memset(op, 0, sizeof *op);
}

static int persist(const tny_image_operation *op, tny_image_record *record, const char *status,
                   bool initial) {
    buf_t out;
    buf_init(&out);
    record->status = status;
    record->finished = initial ? NULL : op->finished;
    tny_image_manifest_serialize(record, &out);
    int rc = out.oom   ? -1
             : initial ? tny_image_io_write_new(op->manifest, out.data, out.len)
                       : tny_image_io_replace(op->manifest, out.data, out.len);
    buf_free(&out);
    return rc;
}

/* Only whitelisted, locally decided text is recorded. tny's own strict-size
 * sentence is built from sizes alone and is safe; every other failure gets a
 * fixed sentence rather than a provider or transport diagnostic. */
static void safe_failure(int rc, const tny_image_result *result, const char *err,
                         tny_image_record *record) {
    if (tny_image_strict_failure(result)) {
        record->error_code = result->code;
        record->error_message = err;
        return;
    }
    record->error_code = rc == 2     ? "IMAGE_PROVIDER_REJECTED"
                         : rc == 130 ? "IMAGE_CANCELLED"
                                     : "IMAGE_OPERATION_FAILED";
    record->error_message = rc == 2 ? "the image provider rejected this request"
                            : rc == 130
                                ? "the operation was interrupted before an image was written"
                                : "the operation failed before an image was written";
}

/* ---- run ----------------------------------------------------------------- */

static int export_image(const tny_image_request *r, int fd, const char *tmp, const buf_t *data,
                        const char *destination) {
    int rc = 0;
    size_t offset = 0;
    while (offset < data->len) {
        if (tny_image_stopped(r)) {
            rc = 130;
            break;
        }
        ssize_t n = write(fd, data->data + offset, data->len - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            rc = 1;
            break;
        }
        offset += (size_t)n;
    }
    if (close(fd) != 0 && !rc) rc = 1;
    if (!rc && tny_image_stopped(r)) rc = 130;
    if (!rc && rename(tmp, destination) != 0) rc = 1;
    return rc;
}

/* An unusable strict request is tny's own decision: it is settled locally,
 * before a destination is reserved, a record is created or quota is spent. */
static int strict_precheck(const tny_image_request *r, tny_image_result *result, char *err,
                           size_t len) {
    if (!r->strict_size || tny_image_size_parse(result->requested_size, NULL, NULL)) return 0;
    result->code = TNY_IMAGE_CODE_STRICT_INVALID;
    result->size_status =
        tny_image_size_compare(result->requested_size, TNY_IMAGE_DIM_UNVERIFIABLE, 0, 0);
    snprintf(err, len,
             "%s: strict size needs an exact WIDTHxHEIGHT request; auto and provider-specific "
             "size names cannot be verified",
             result->code);
    return 1;
}

/* Strict mode refuses the paid bytes rather than the destination: nothing is
 * written, the previous output stands, and nothing is retried or resized. */
static int strict_rejection(const tny_image_request *r, tny_image_result *result, char *err,
                            size_t len) {
    if (!r->strict_size || result->size_status == TNY_IMAGE_SIZE_MATCH) return 0;
    if (result->size_status == TNY_IMAGE_SIZE_MISMATCH) {
        result->code = TNY_IMAGE_CODE_MISMATCH;
        snprintf(err, len, "%s: requested %s but the provider returned %ux%u; no file was written",
                 result->code, result->requested_size, result->width, result->height);
    } else {
        bool unsupported = result->size_status == TNY_IMAGE_SIZE_UNSUPPORTED;
        result->code = unsupported ? TNY_IMAGE_CODE_UNSUPPORTED : TNY_IMAGE_CODE_UNVERIFIABLE;
        snprintf(err, len,
                 "%s: requested %s but the returned %s dimensions %s; no file was written",
                 result->code, result->requested_size, result->mime ? result->mime : "image",
                 unsupported ? "use an encoding this build cannot read" : "could not be read");
    }
    return 1;
}

/* The destination must not be a record tny writes, and must not alias a
 * reference that came from one: replaying into the very artifact being read
 * would destroy the lineage the replay depends on. An ordinary `--image`
 * in-place edit stays supported, because every reference is fully loaded
 * before anything is written (docs/images.md). */
static int check_destination(const tny_image_plan *plan, const tny_image_request *r,
                             const char *canonical, char *err, size_t len) {
    if (tny_image_manifest_reserved_name(canonical)) {
        snprintf(err, len, "image output may not use a reserved tny manifest file name");
        return 1;
    }
    if (r->from_manifest && tny_image_io_same_file(canonical, r->from_manifest)) {
        snprintf(err, len, "image output may not replace the manifest being rerun");
        return 1;
    }
    if (plan->source && plan->source->artifact_path) {
        char *artifact = tny_image_manifest_resolve(plan->source, plan->source->artifact_path);
        if (!artifact) {
            snprintf(err, len, "out of memory resolving the replay source artifact");
            return 1;
        }
        bool alias = tny_image_io_same_file(canonical, artifact);
        free(artifact);
        if (alias) {
            snprintf(err, len,
                     "replay needs a new output; it may not overwrite the artifact it reruns, "
                     "choose a new path");
            return 1;
        }
    }
    for (size_t i = 0; i < plan->reference_count; i++) {
        if (!*plan->references[i].expected) continue;
        if (tny_image_io_same_file(canonical, plan->references[i].path)) {
            snprintf(err, len,
                     "image output may not be the recorded source artifact; choose a new path");
            return 1;
        }
    }
    return 0;
}

/* One body for every caller. `prepared` is the plan a permission decision
 * already approved (ADR 0095): it is used exactly as it stands, so nothing is
 * re-resolved from a record that may have changed since. A normal call passes
 * NULL and resolves into its own local plan. */
static int image_run(const tny_ctx *ctx, const tny_image_request *r, tny_image_result *result,
                     tny_image_plan *prepared, char *err, size_t len) {
    *err = 0;
    memset(result, 0, sizeof *result);
    result->edit = prepared ? prepared->edit : r->edit;
    snprintf(result->requested_size, sizeof result->requested_size, "%s",
             r->size ? r->size : "auto");
    tny_image_plan local = {0};
    tny_image_plan *plan = prepared ? prepared : &local;
    tny_image_operation op = {0};
    tny_image_record record = {0};
    tny_image_input inputs[TNY_IMAGE_REFERENCES_MAX] = {0};
    buf_t image, tmp;
    buf_init(&image);
    buf_init(&tmp);
    char output_hash[TNY_IMAGE_SHA256_HEX] = "";
    int rc = 1, fd = -1;
    bool reserved = false, terminal = false;
    /* Literal option shapes are settled before any file is opened. */
    if (!valid_string(r->output_file, TNY_IMAGE_PATH_MAX) ||
        (r->model && !valid_string(r->model, 128)) || !valid_quality(r->quality) ||
        (r->size && !valid_string(r->size, 32)) ||
        (r->provider && !valid_string(r->provider, 128)) ||
        r->image_count > TNY_IMAGE_REFERENCES_MAX ||
        (!r->from_manifest && (!valid_prompt(r->prompt) || r->edit != (r->image_count > 0)))) {
        snprintf(err, len,
                 "images need UTF-8 prompt (1-16384 bytes), output file, valid options; edit needs "
                 "1-5 references, generate none");
        goto done;
    }
    /* A directly supplied size is judged here, before anything else happens. A
     * replayed size is judged the moment its record is read, below — still
     * before a destination is reserved or a paid request is made. */
    if (!r->from_manifest && strict_precheck(r, result, err, len)) goto done;
    if (tny_image_stopped(r)) {
        rc = 130;
        goto done;
    }
    if (!prepared && tny_image_plan_resolve(r, &local, err, len)) goto done;
    result->edit = plan->edit;
    snprintf(result->requested_size, sizeof result->requested_size, "%s",
             plan->size ? plan->size : "auto");
    if (strict_precheck(r, result, err, len)) goto done;
    if (!valid_prompt(plan->prompt) || !valid_quality(plan->quality) ||
        plan->edit != (plan->reference_count > 0)) {
        snprintf(err, len,
                 "images need UTF-8 prompt (1-16384 bytes), output file, valid options; edit needs "
                 "1-5 references, generate none");
        goto done;
    }
    if (!tny_image_available(ctx, plan->provider, plan->edit, err, len)) goto done;
    const tny_image_provider *p = find_provider(plan->provider);
    if (!p) {
        snprintf(err, len, "unknown image provider");
        goto done;
    }
    if (plan->reference_count > p->max_references) {
        snprintf(err, len, "too many references for image provider");
        goto done;
    }
    if (tny_image_stopped(r)) {
        rc = 130;
        goto done;
    }
    op.canonical = tny_image_io_canonical(r->output_file, err, len);
    if (!op.canonical) goto done;
    if (check_destination(plan, r, op.canonical, err, len)) goto done;
    char *id = gen_id();
    if (!id) {
        snprintf(err, len, "cannot start an image operation");
        goto done;
    }
    snprintf(result->operation_id, sizeof result->operation_id, "%s", id);
    free(id);
    /* Own the destination before spending anything. A second operation on the
     * same normalized path fails here rather than racing a paid request. */
    op.guard = tny_image_io_guard_acquire(op.canonical, result->operation_id, err, len);
    if (!op.guard) goto done;
    rc = load_references(r, plan, inputs, err, len);
    if (rc) goto done;
    rc = 1;
    op.workspace = path_abs(ctx && ctx->cwd && *ctx->cwd == '/' ? ctx->cwd : ".");
    op.started = now_iso8601();
    op.manifest =
        r->no_manifest ? NULL : tny_image_manifest_path(op.canonical, result->operation_id);
    if (!op.workspace || !op.started || (!r->no_manifest && !op.manifest)) {
        snprintf(err, len, "cannot start an image operation");
        goto done;
    }
    record.operation_id = result->operation_id;
    record.edit = plan->edit;
    record.workspace = op.workspace;
    record.started = op.started;
    record.prompt = plan->prompt;
    record.output = op.canonical;
    record.references = plan->references;
    record.reference_count = plan->reference_count;
    record.requested_provider = p->name;
    record.requested_model = plan->model;
    record.requested_quality = plan->quality;
    record.requested_size = result->requested_size;
    record.source_manifest = plan->source ? plan->source->path : NULL;
    record.source_operation = plan->source ? plan->source->operation_id : NULL;
    if (op.manifest) {
        if (persist(&op, &record, "running", true) != 0) {
            snprintf(err, len,
                     "cannot create the image manifest beside the output; no request was made "
                     "(use --no-manifest to skip provenance)");
            goto done;
        }
        op.intent = true;
        snprintf(result->manifest_path, sizeof result->manifest_path, "%s", op.manifest);
    }
    buf_appendf(&tmp, "%s.XXXXXX", op.canonical);
    fd = tmp.oom ? -1 : mkstemp(tmp.data);
    if (fd < 0) {
        snprintf(err, len, "cannot create image output file");
        goto done;
    }
    reserved = true;
    tny_image_request request = *r;
    request.edit = plan->edit;
    request.prompt = plan->prompt;
    request.provider = plan->provider;
    request.quality = plan->quality;
    request.size = plan->size;
    request.model = plan->model ? plan->model : p->default_model;
    request.image_count = plan->reference_count;
    tny_image_wire wire = {0};
    result->provider = p->name;
    snprintf(result->model, sizeof result->model, "%s", request.model);
    record.effective_provider = p->name;
    record.effective_model = result->model;
    rc = p->render(ctx, &request, inputs, &image, &wire, err, len);
    snprintf(result->effective_size, sizeof result->effective_size, "%s",
             wire.size ? wire.size : "");
    record.effective_size = *result->effective_size ? result->effective_size : NULL;
    result->have_seed = wire.have_seed;
    result->seed = wire.seed;
    snprintf(result->request_id, sizeof result->request_id, "%s", wire.request_id);
    record.have_seed = wire.have_seed;
    record.seed = wire.seed;
    record.request_id = *wire.request_id ? wire.request_id : NULL;
    if (!rc && tny_image_stopped(r)) rc = 130;
    const char *mime = image_mime((const uint8_t *)image.data, image.len);
    if (!rc && (!mime || image.len > TNY_IMAGE_OUTPUT_MAX || strcmp(mime, "image/gif") == 0)) {
        snprintf(err, len, "invalid image provider output");
        rc = 1;
    }
    if (!rc) {
        /* Dimensions come from the returned bytes, never from the request. */
        tny_image_dim_status dimensions = tny_image_dimensions(
            (const uint8_t *)image.data, image.len, &result->width, &result->height);
        result->mime = mime;
        result->size_status = tny_image_size_compare(result->requested_size, dimensions,
                                                     result->width, result->height);
        record.mime = mime;
        record.width = result->width;
        record.height = result->height;
        record.size_status = tny_image_size_status_name(result->size_status);
        rc = strict_rejection(r, result, err, len);
    }
    if (!rc) {
        rc = export_image(r, fd, tmp.data, &image, op.canonical);
        fd = -1;
        reserved = rc != 0;
        if (!rc) {
            result->bytes = image.len;
            result->committed = true;
            record.bytes = image.len;
            record.committed = true;
        } else if (rc != 130) snprintf(err, len, "cannot write image output file");
    }
    if (op.manifest) {
        if (!rc) (void)tny_image_io_sha256_hex(image.data, image.len, output_hash);
        op.finished = now_iso8601();
        record.output_sha256 = *output_hash ? output_hash : NULL;
        if (rc) safe_failure(rc, result, err, &record);
        const char *status = !rc ? "succeeded" : rc == 130 ? "cancelled" : "failed";
        bool wrote = op.finished && (!record.committed || *output_hash) &&
                     persist(&op, &record, status, false) == 0;
        terminal = wrote;
        if (!wrote && !rc) {
            /* The artifact really is in place. Say exactly that: deleting paid
             * work to make a metadata failure tidy is never the right trade.
             * The running intent stays behind, so a later reader sees an
             * unfinished operation rather than a fabricated success. */
            result->code = TNY_IMAGE_CODE_MANIFEST;
            snprintf(err, len,
                     "image was written to the requested output and kept, but its manifest "
                     "could not be finalized");
            rc = 1;
        }
    }
done:
    if (fd >= 0) close(fd);
    if (reserved && tmp.data) unlink(tmp.data);
    /* A record created for this run must not be left looking alive — unless
     * the artifact was committed and only its finalization failed, where the
     * unfinished intent is the truthful state and "failed" would not be. */
    if (op.intent && !terminal && !result->committed) {
        if (!op.finished) op.finished = now_iso8601();
        safe_failure(rc, result, err, &record);
        (void)persist(&op, &record, rc == 130 ? "cancelled" : "failed", false);
    }
    for (size_t i = 0; i < TNY_IMAGE_REFERENCES_MAX; i++) buf_free(&inputs[i].data);
    buf_free(&image);
    buf_free(&tmp);
    tny_image_plan_free(&local);
    operation_free(&op);
    if (rc == 130) snprintf(err, len, "image operation interrupted");
    else if (rc && !*err) snprintf(err, len, "image operation failed");
    return rc;
}

int tny_image_run(const tny_ctx *ctx, const tny_image_request *r, tny_image_result *result,
                  char *err, size_t len) {
    return image_run(ctx, r, result, NULL, err, len);
}

int tny_image_run_prepared(const tny_ctx *ctx, const tny_image_request *r, tny_image_plan *plan,
                           tny_image_result *result, char *err, size_t len) {
    /* A permission-gated caller has no second way to decide what to run: a
     * missing plan refuses instead of quietly resolving one here. */
    if (!plan) {
        memset(result, 0, sizeof *result);
        snprintf(err, len, "this image call was not prepared; request it again");
        return 1;
    }
    return image_run(ctx, r, result, plan, err, len);
}

/* Unknown values are null, never zero and never a guess. */
static void size_metadata(const tny_image_result *result, buf_t *out) {
    buf_appends(out, ",\"requested_size\":");
    jescape(out, result->requested_size);
    buf_appends(out, ",\"effective_size\":");
    if (*result->effective_size) jescape(out, result->effective_size);
    else buf_appends(out, "null");
    if (result->width && result->height)
        buf_appendf(out, ",\"width\":%u,\"height\":%u", result->width, result->height);
    else buf_appends(out, ",\"width\":null,\"height\":null");
    buf_appends(out, ",\"size_status\":");
    jescape(out, tny_image_size_status_name(result->size_status));
}

/* Provenance is additive: an omitted manifest and an absent provider field are
 * null, and no local identifier is ever substituted for a provider's. */
static void local_provenance_metadata(const tny_image_result *result, buf_t *out) {
    buf_appends(out, ",\"operation_id\":");
    if (*result->operation_id) jescape(out, result->operation_id);
    else buf_appends(out, "null");
    buf_appends(out, ",\"manifest_path\":");
    if (*result->manifest_path) jescape(out, result->manifest_path);
    else buf_appends(out, "null");
}

static void provenance_metadata(const tny_image_result *result, buf_t *out) {
    local_provenance_metadata(result, out);
    buf_appends(out, ",\"seed\":");
    if (result->have_seed) buf_appendf(out, "%lld", (long long)result->seed);
    else buf_appends(out, "null");
    buf_appends(out, ",\"request_id\":");
    if (*result->request_id) jescape(out, result->request_id);
    else buf_appends(out, "null");
}

void tny_image_result_json(const tny_image_request *r, const tny_image_result *result, buf_t *out) {
    buf_appends(out, "{\"kind\":\"image\",\"ok\":true,\"operation\":");
    jescape(out, result->edit ? "edit" : "generate");
    buf_appends(out, ",\"provider\":");
    jescape(out, result->provider);
    buf_appends(out, ",\"model\":");
    jescape(out, result->model);
    buf_appends(out, ",\"path\":");
    jescape(out, r->output_file);
    buf_appends(out, ",\"mime_type\":");
    jescape(out, result->mime);
    buf_appendf(out, ",\"bytes\":%zu", result->bytes);
    size_metadata(result, out);
    /* These bytes are exactly what the provider returned: this service never
     * resizes, crops or re-encodes, so there is no derived artifact yet. */
    buf_appends(out, ",\"native\":true,\"transform\":null");
    provenance_metadata(result, out);
    buf_appends(out, "}\n");
}

bool tny_image_strict_failure(const tny_image_result *result) {
    const char *code = result ? result->code : NULL;
    return code && (strcmp(code, TNY_IMAGE_CODE_STRICT_INVALID) == 0 ||
                    strcmp(code, TNY_IMAGE_CODE_MISMATCH) == 0 ||
                    strcmp(code, TNY_IMAGE_CODE_UNVERIFIABLE) == 0 ||
                    strcmp(code, TNY_IMAGE_CODE_UNSUPPORTED) == 0);
}

bool tny_image_retained_failure(const tny_image_result *result) {
    return result && result->committed && result->code &&
           strcmp(result->code, TNY_IMAGE_CODE_MANIFEST) == 0;
}

void tny_image_error_json(const tny_image_request *r, const tny_image_result *result,
                          const char *message, buf_t *out) {
    (void)r;
    buf_appends(out, "{\"kind\":\"image\",\"ok\":false,\"operation\":");
    jescape(out, result->edit ? "edit" : "generate");
    buf_appends(out, ",\"code\":");
    jescape(out, result->code ? result->code : "IMAGE_FAILED");
    buf_appends(out, ",\"error\":");
    jescape(out, message ? message : "image operation failed");
    buf_appends(out, ",\"mime_type\":");
    if (result->mime) jescape(out, result->mime);
    else buf_appends(out, "null");
    size_metadata(result, out);
    /* No output was committed, so no path may be reported as written. */
    buf_appends(out, ",\"path\":null,\"committed\":false}\n");
}

void tny_image_retained_json(const tny_image_request *r, const tny_image_result *result,
                             const char *message, buf_t *out) {
    buf_appends(out, "{\"kind\":\"image\",\"ok\":false,\"operation\":");
    jescape(out, result->edit ? "edit" : "generate");
    buf_appends(out, ",\"code\":");
    jescape(out, TNY_IMAGE_CODE_MANIFEST);
    buf_appends(out, ",\"error\":");
    jescape(out, message ? message : "image manifest could not be finalized");
    buf_appends(out, ",\"mime_type\":");
    if (result->mime) jescape(out, result->mime);
    else buf_appends(out, "null");
    buf_appendf(out, ",\"bytes\":%zu", result->bytes);
    size_metadata(result, out);
    /* The artifact really is there: reporting it is the honest answer, and
     * deleting it to simplify this object would destroy paid work. */
    buf_appends(out, ",\"path\":");
    jescape(out, r->output_file);
    buf_appends(out, ",\"committed\":true");
    /* Failure detail is local-only. Provider request ids can contain arbitrary
     * printable response text and belong only in success/manifest metadata. */
    local_provenance_metadata(result, out);
    buf_appends(out, "}\n");
}

bool tny_image_size_warning(const char *requested_size, const char *size_status, uint32_t width,
                            uint32_t height, char *out, size_t len) {
    if (!out || !len) return false;
    *out = 0;
    if (!size_status || !tny_image_size_parse(requested_size, NULL, NULL) ||
        strcmp(size_status, "match") == 0)
        return false;
    if (width && height)
        snprintf(out, len,
                 "requested %s but the image is %ux%u (size_status=%s); nothing was resized "
                 "locally, and --strict-size fails instead of accepting this",
                 requested_size, width, height, size_status);
    else
        snprintf(out, len,
                 "requested %s but the returned image dimensions could not be read "
                 "(size_status=%s); the file is the provider's own bytes",
                 requested_size, size_status);
    return true;
}
