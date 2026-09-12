/* Explicit local image exports and contact sheets (ADR 0094).
 *
 * Generation never runs a converter. Only `tny image export`, `tny image
 * contact-sheet`, their typed tools and the same two commands typed into the
 * terminal tool reach this service, and each of them reaches it through one
 * shared implementation: one grammar, one validation, one permission identity,
 * one commit protocol and one derived record.
 *
 * The artifact this produces is derived, never native: it carries the hashes
 * and operation ids of the exact bytes it consumed, and no replay may turn it
 * back into a provider request.
 */
#ifndef TNY_IMAGE_EXPORT_H
#define TNY_IMAGE_EXPORT_H

#include "core/image_manifest.h"
#include "core/image_service.h"
#include "util/image_transform.h"

#define TNY_IMAGE_EXPORT_SOURCES_MAX TNY_IMAGE_SOURCES_MAX
/* Every bound below is checked before a byte is read or a child is started. */
#define TNY_IMAGE_EXPORT_DIM_MAX    16384u
#define TNY_IMAGE_EXPORT_PIXELS_MAX (64u * 1024u * 1024u)
#define TNY_IMAGE_EXPORT_COLOR_MAX  16

typedef enum {
    TNY_IMAGE_POLICY_FIT = 0, /* contain, then pad to the exact canvas */
    TNY_IMAGE_POLICY_CROP,    /* cover, then crop at the chosen gravity */
    TNY_IMAGE_POLICY_PAD,     /* never enlarge; shrink only as needed, then pad */
} tny_image_policy;

typedef enum {
    TNY_IMAGE_GRAVITY_CENTER = 0,
    TNY_IMAGE_GRAVITY_NORTH,
    TNY_IMAGE_GRAVITY_SOUTH,
    TNY_IMAGE_GRAVITY_EAST,
    TNY_IMAGE_GRAVITY_WEST,
    TNY_IMAGE_GRAVITY_NORTHEAST,
    TNY_IMAGE_GRAVITY_NORTHWEST,
    TNY_IMAGE_GRAVITY_SOUTHEAST,
    TNY_IMAGE_GRAVITY_SOUTHWEST,
} tny_image_gravity;

typedef enum {
    TNY_IMAGE_FORMAT_PNG = 0,
    TNY_IMAGE_FORMAT_JPEG,
    TNY_IMAGE_FORMAT_WEBP,
} tny_image_format;

typedef enum {
    TNY_IMAGE_LABELS_NONE = 0,
    TNY_IMAGE_LABELS_NUMBERS, /* fixed numeric bitmaps; no font, no free text */
} tny_image_labels;

/* Exactly what a caller asked for, in the spelling it used. Nothing here is
 * trusted: every string is validated into the settings below before any file
 * is opened, and no string ever reaches the converter's argument vector. */
typedef struct {
    bool sheet; /* contact sheet rather than a single-image export */
    const char *sources[TNY_IMAGE_EXPORT_SOURCES_MAX];
    /* Parallel to sources: true when that entry is a manifest whose verified
     * artifact is the input, false for a plain file path. */
    bool source_is_artifact[TNY_IMAGE_EXPORT_SOURCES_MAX];
    size_t source_count;
    const char *output_file;
    const char *size;       /* WIDTHxHEIGHT; required */
    const char *policy;     /* fit | crop | pad; default fit */
    const char *gravity;    /* nine directions; default center */
    const char *background; /* transparent | #RRGGBB | #RRGGBBAA */
    const char *format;     /* png | jpeg | webp; default png */
    const char *columns;    /* sheet only; default ceil(sqrt(sources)) */
    const char *labels;     /* sheet only: none | numbers; default none */
    bool preview;           /* explicit conversation upload request; adapters only */
    bool overwrite;
    bool no_manifest;
    bool (*cancelled)(void *);
    void *userdata;
} tny_image_export_request;

/* The validated operation: exact numbers, closed enumerations and the derived
 * grid. Everything the converter is told comes from here. */
typedef struct {
    bool sheet;
    uint32_t width, height; /* the exact canvas, always */
    tny_image_policy policy;
    tny_image_gravity gravity;
    char background[TNY_IMAGE_EXPORT_COLOR_MAX]; /* "transparent" or "#rrggbbaa" */
    tny_image_format format;
    tny_image_labels labels;
    /* Sheet geometry; zero for a single-image export. Cells are floored, so
     * the unused remainder of the canvas stays background. */
    uint32_t columns, rows, cell_width, cell_height;
    uint32_t label_width, label_height, label_scale;
} tny_image_export_settings;

typedef struct {
    char operation_id[TNY_IMAGE_OPERATION_ID_MAX];
    char manifest_path[TNY_IMAGE_MANIFEST_PATH_MAX];
    char output[TNY_IMAGE_PATH_MAX];   /* canonical destination */
    char sha256[TNY_IMAGE_SHA256_HEX]; /* of the committed bytes */
    char tool_version[TNY_IMAGE_TOOL_VERSION_MAX];
    const char *mime;       /* static; sniffed from the output */
    uint32_t width, height; /* read back from the output bytes */
    size_t bytes;
    tny_image_export_settings settings;
    size_t source_count;
    tny_image_source_dimensions source_dimensions[TNY_IMAGE_EXPORT_SOURCES_MAX];
    bool committed;
    const char *code; /* stable failure code; NULL while nothing failed */
} tny_image_export_result;

/* Stable machine-readable failure codes (docs/images.md). A manifest
 * finalization failure reuses TNY_IMAGE_CODE_MANIFEST: the derived artifact is
 * real and kept, and saying otherwise would destroy work at every surface. */
#define TNY_IMAGE_CODE_EXPORT_INVALID "IMAGE_EXPORT_INVALID"
#define TNY_IMAGE_CODE_EXPORT_FAILED  "IMAGE_EXPORT_FAILED"
#define TNY_IMAGE_CODE_EXPORT_CANCEL  "IMAGE_EXPORT_CANCELLED"

/* True when this host can run local transforms at all. False only means the
 * platform has no process seam (wasm); it is not a check for the optional
 * executable, which is reported with actionable guidance at call time. */
bool tny_image_export_supported(void);

/* "export" | "contact_sheet": the operation name shared by the permission
 * identity, the tool name and the derived record. */
const char *tny_image_export_operation(const tny_image_export_request *);

/* Shared CLI/interception grammar for `export` and `contact-sheet`.
 * 0 parsed, 1 invalid, -1 help. Request must be zero initialized. */
int tny_image_export_options(int argc, char **argv, tny_image_export_request *, bool *json);

/* Validate every option into the exact operation, without opening a file.
 * 0 on success, 1 with a safe reason in err. */
int tny_image_export_settings_resolve(const tny_image_export_request *, tny_image_export_settings *,
                                      char *err, size_t errlen);

/* The sensitive permission identity: the operation, the ordered canonical
 * inputs with the SHA-256 of their exact current bytes (plus the manifest path
 * and source operation id of every artifact input), the canonical destination
 * and every setting that changes what is written — canvas, policy, gravity,
 * background, format, overwrite, persistence, columns and labels.
 *
 * It is built at prepare and rebuilt at execute, so a source, a record or a
 * destination that changed after the grant is a different operation and needs
 * a new one. Reading these inputs is an ordinary read: the caller applies its
 * existing allowed-root rules to each resolved path. 0 on success. */
int tny_image_export_detail(const tny_image_export_request *, buf_t *out, char *err, size_t errlen);

/* Resolve the input paths this request would read, in order, so a caller can
 * apply its allowed-root policy before anything is opened. Each returned entry
 * is malloc'd; count is written even on failure for cleanup. */
int tny_image_export_inputs(const tny_image_export_request *,
                            char *paths[TNY_IMAGE_EXPORT_SOURCES_MAX], size_t *count, char *err,
                            size_t errlen);

/* Owned execution snapshot. Resolve records once, check the returned artifact
 * paths against allowed roots, then capture bytes and derive the grant detail.
 * Run this SAME plan after perm_check; never resolve records or reopen inputs
 * after that check. Free on all paths. Only callback userdata is borrowed.
 * Native only: callers gate unsupported runtimes before creating a plan. */
typedef struct tny_image_export_plan tny_image_export_plan;
tny_image_export_plan *tny_image_export_plan_new(const tny_image_export_request *, char *, size_t);
const char *tny_image_export_plan_input(const tny_image_export_plan *, size_t, bool *artifact);
int tny_image_export_plan_capture(tny_image_export_plan *, char *, size_t);
int tny_image_export_plan_detail(const tny_image_export_plan *, buf_t *);
int tny_image_export_plan_run(const tny_ctx *, tny_image_export_plan *, tny_image_export_result *,
                              char *, size_t);
void tny_image_export_plan_free(tny_image_export_plan *);

/* Run one export. 0 success, 1 failure, 130 cancelled. The destination is
 * replaced only by complete, twice-decoded bytes whose MIME and dimensions are
 * exactly what was asked for; sources are never modified. */
int tny_image_export_run(const tny_ctx *, const tny_image_export_request *,
                         tny_image_export_result *, char *err, size_t errlen);

/* Result objects. The retained form is the one a failed record finalization
 * uses: it reports the derived artifact that really is on disk. */
void tny_image_export_result_json(const tny_image_export_request *, const tny_image_export_result *,
                                  buf_t *);
void tny_image_export_error_json(const tny_image_export_request *, const tny_image_export_result *,
                                 const char *message, buf_t *);
void tny_image_export_retained_json(const tny_image_export_request *,
                                    const tny_image_export_result *, const char *message, buf_t *);
/* True whenever the artifact was committed. Every later failure must still
 * name its real path and hash, not only a record-finalization failure. */
bool tny_image_export_retained(const tny_image_export_result *);

#endif
