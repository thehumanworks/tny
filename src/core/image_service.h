/* Provider-independent image operations (ADR 0074, ADR 0088). */
#ifndef TNY_IMAGE_SERVICE_H
#define TNY_IMAGE_SERVICE_H
#include "core/config.h"
#include "core/image_dimensions.h"

#define TNY_IMAGE_PROMPT_MAX     (16u * 1024u)
#define TNY_IMAGE_INPUT_MAX      (8u * 1024u * 1024u)
#define TNY_IMAGE_OUTPUT_MAX     (32u * 1024u * 1024u)
#define TNY_IMAGE_REFERENCES_MAX 5
#define TNY_IMAGE_PATH_MAX       4096
/* Lowercase hex SHA-256 plus NUL, and tny's 16-hex operation ids with room
 * for a longer identifier in a record written by a future build. */
#define TNY_IMAGE_SHA256_HEX        65
#define TNY_IMAGE_OPERATION_ID_MAX  33
#define TNY_IMAGE_REQUEST_ID_MAX    128
#define TNY_IMAGE_SIZE_MAX          64
#define TNY_IMAGE_MODEL_MAX         160
#define TNY_IMAGE_MANIFEST_PATH_MAX (TNY_IMAGE_PATH_MAX + 64)

typedef struct {
    bool edit;
    bool replay;          /* `tny image replay`: adopt the recorded operation */
    const char *prompt;   /* NULL only when a replay supplies it */
    const char *provider; /* NULL: codex, independent of conversation provider */
    const char *model;    /* NULL: adapter default */
    const char *quality;  /* NULL: provider default (codex: high) */
    const char *size;     /* NULL: auto */
    const char *output_file;
    bool strict_size;          /* require a concrete WxH request the bytes actually match */
    bool no_manifest;          /* --no-manifest / persist_manifest:false */
    const char *from_manifest; /* replay/rerun source record */
    const char *images[TNY_IMAGE_REFERENCES_MAX];
    /* Parallel to images: true when that entry is a manifest whose verified
     * successful output becomes the reference, false for a plain file path. */
    bool image_is_artifact[TNY_IMAGE_REFERENCES_MAX];
    size_t image_count;
    bool (*cancelled)(void *);
    void *userdata;
} tny_image_request;

/* One uploaded reference: the path read, the hash of the exact bytes sent to
 * the provider, and — when it came from an earlier record — that record's
 * identity plus the hash those bytes must still have. */
typedef struct {
    char *path;
    char sha256[TNY_IMAGE_SHA256_HEX];
    char *source_manifest; /* NULL for a plain --image path */
    char source_operation[TNY_IMAGE_OPERATION_ID_MAX];
    char expected[TNY_IMAGE_SHA256_HEX]; /* empty when nothing was pinned */
} tny_image_reference;

typedef struct tny_image_manifest tny_image_manifest;

/* What a request would actually do, resolved from records alone. Every string
 * is owned here (ADR 0095): a plan retained from a permission decision until
 * the call runs must not borrow the caller's short-lived JSON document. NULL
 * settings mean "use the default", never "look it up again". */
typedef struct tny_image_plan {
    bool edit;
    char *prompt;
    char *provider, *model, *quality, *size;
    tny_image_reference references[TNY_IMAGE_REFERENCES_MAX];
    size_t reference_count;
    tny_image_manifest *source; /* owned; NULL unless replayed */
} tny_image_plan;

/* Every string here is either a static constant or owned inline. Settings can
 * come from a replayed record whose parsed copy is released before the caller
 * reads the result, so nothing may borrow one. Empty strings mean "not
 * available", never a guess, and no caller has to free a result. */
typedef struct {
    bool edit;            /* resolved operation, including replay and failure detail */
    const char *provider; /* static adapter name */
    const char *mime;     /* static; bytes are always sniffed */
    size_t bytes;
    char model[TNY_IMAGE_MODEL_MAX];
    char requested_size[TNY_IMAGE_SIZE_MAX]; /* literal request; "auto" when none */
    char effective_size[TNY_IMAGE_SIZE_MAX]; /* exact literal sent; empty if none */
    uint32_t width, height;                  /* read from the returned bytes; 0 when unknown */
    tny_image_size_status size_status;
    const char *code; /* stable failure code; NULL while nothing failed */
    char operation_id[TNY_IMAGE_OPERATION_ID_MAX];
    char manifest_path[TNY_IMAGE_MANIFEST_PATH_MAX];
    char request_id[TNY_IMAGE_REQUEST_ID_MAX]; /* provider supplied only */
    bool have_seed;
    int64_t seed;
    bool committed; /* the destination now holds this operation's bytes */
} tny_image_result;

/* Stable machine-readable failure codes (docs/images.md). */
#define TNY_IMAGE_CODE_STRICT_INVALID "IMAGE_STRICT_SIZE_INVALID"
#define TNY_IMAGE_CODE_MISMATCH       "IMAGE_SIZE_MISMATCH"
#define TNY_IMAGE_CODE_UNVERIFIABLE   "IMAGE_SIZE_UNVERIFIABLE"
#define TNY_IMAGE_CODE_UNSUPPORTED    "IMAGE_SIZE_UNSUPPORTED"
/* Distinct on purpose: the paid artifact was written and kept, only its
 * provenance record could not be finalized. It never masquerades as a strict
 * rejection, which commits nothing. */
#define TNY_IMAGE_CODE_MANIFEST "IMAGE_MANIFEST_FINALIZE_FAILED"

/* No network or refresh. Presence is not a remote entitlement check. */
bool tny_image_available(const tny_ctx *, const char *provider, bool edit, char *err,
                         size_t errlen);
/* Probe all registered adapters; optionally append available names separated by commas. */
bool tny_image_capabilities(const tny_ctx *, bool edit, buf_t *names);
/* Resolve the effective operation, settings and every reference this request
 * would upload. Reads manifests only: no referenced image is opened, no hash
 * is computed and no provider is contacted. 0 on success, 1 with a safe
 * reason in err. Always leaves the plan safe to free. */
int tny_image_plan_resolve(const tny_image_request *, tny_image_plan *, char *err, size_t errlen);
void tny_image_plan_free(tny_image_plan *);
/* 0 success, 1 local/config/protocol error, 2 HTTP rejection, 130 cancelled.
 * Output is replaced only after a complete, validated image. */
int tny_image_run(const tny_ctx *, const tny_image_request *, tny_image_result *, char *, size_t);
/* Internal: run exactly the plan a permission decision already approved
 * (ADR 0095). Same body, same checks; no record is reopened and no second
 * permission question is asked, so a one-time grant stays one-time. The plan
 * is borrowed and stays owned by the caller; reference hashes of the bytes
 * actually loaded are filled in, expected hashes and settings are not. */
int tny_image_run_prepared(const tny_ctx *, const tny_image_request *, tny_image_plan *,
                           tny_image_result *, char *, size_t);
/* Shared CLI/interception grammar. Prompt is supplied separately on stdin.
 * 0 parsed, 1 invalid, -1 help. Request must be zero initialized. */
int tny_image_options(int argc, char **argv, tny_image_request *, bool *json, bool *check);
void tny_image_result_json(const tny_image_request *, const tny_image_result *, buf_t *);
/* Stable strict-failure object: available metadata, no committed output. */
void tny_image_error_json(const tny_image_request *, const tny_image_result *, const char *message,
                          buf_t *);
/* The distinct retained-artifact object for a failed manifest finalization.
 * It reports the committed path truthfully instead of claiming nothing ran. */
void tny_image_retained_json(const tny_image_request *, const tny_image_result *,
                             const char *message, buf_t *);
/* True only when this result carries one of the four locally assigned strict
 * size codes above. It matches tny's own constants, never a provider string or
 * an "IMAGE_SIZE_" prefix, so no provider diagnostic can select this path. */
bool tny_image_strict_failure(const tny_image_result *);
/* True only when the paid artifact is in place and exactly tny's own manifest
 * finalization failed. It is the one failure that may report a committed
 * path, and it is selected by the local code plus that committed state — never
 * by provider text. */
bool tny_image_retained_failure(const tny_image_result *);
/* One actionable human warning when a concrete size request was not verifiably
 * met. Callers own the surface prefix; false means say nothing. The JSON-valued
 * form lets result readers reuse the same wording without a second rule. */
bool tny_image_size_warning(const char *requested_size, const char *size_status, uint32_t width,
                            uint32_t height, char *out, size_t len);
#endif
