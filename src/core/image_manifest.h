/* Version-1 image generation manifests and reference lineage (ADR 0088).
 *
 * One immutable private record per requested operation, written next to its
 * destination as <output>.tny-image-<operation-id>.json. There is no registry,
 * no mutable "latest" pointer and no content-addressed blob store: a later
 * generation at the same path creates a new record instead of overwriting the
 * earlier lineage. Reading a record never opens a referenced file and never
 * contacts a provider.
 */
#ifndef TNY_IMAGE_MANIFEST_H
#define TNY_IMAGE_MANIFEST_H

#include "core/image_service.h"

#define TNY_IMAGE_MANIFEST_VERSION 1
/* A record holds one prompt (16 KiB), five reference paths and small scalars;
 * a contact sheet adds up to 64 ordered source entries. Anything larger is
 * rejected before it is parsed, let alone acted on. */
#define TNY_IMAGE_MANIFEST_MAX (256u * 1024u)
#define TNY_IMAGE_SOURCES_MAX  64

/* Dimensions of captured transform inputs, distinct from the output canvas
 * and any provider's native generation dimensions. Zero means unknown in an
 * older version-1 record. */
typedef struct {
    uint32_t width, height;
} tny_image_source_dimensions;

/* A local derived transform (ADR 0094): an explicit export or contact sheet,
 * produced by the optional external tool from bytes tny already had. It is
 * the same version-1 record shape, with `operation` naming the transform,
 * `requested.provider` the literal "local" — never a network provider, so a
 * replay cannot be turned into a paid request — no prompt, and one artifact
 * whose role is "derived" rather than "native". */
typedef struct {
    const char *operation; /* export | contact_sheet */
    const char *policy;    /* fit | crop | pad */
    const char *gravity;
    const char *background;
    const char *format; /* png | jpeg | webp */
    const char *labels; /* none | numbers */
    uint32_t width, height;
    /* Sheet geometry; all zero for a single-image export. */
    uint32_t columns, rows, cell_width, cell_height;
    const char *tool, *tool_version;
    /* The exact ordered bytes this transform consumed. */
    const tny_image_reference *sources;
    const tny_image_source_dimensions *source_dimensions;
    size_t source_count;
} tny_image_transform;

/* Write side. Every string is borrowed for the duration of the call. */
typedef struct {
    const char *operation_id;
    bool edit;
    const char *status; /* running | succeeded | failed | cancelled */
    const char *workspace;
    const char *started;
    const char *finished; /* NULL while running */
    const char *prompt;
    const char *output; /* canonical destination, requested not claimed */
    bool committed;     /* true only once the artifact is actually in place */
    const tny_image_reference *references;
    size_t reference_count;
    const char *requested_provider, *requested_model, *requested_quality, *requested_size;
    const char *effective_provider, *effective_model, *effective_size;
    const char *output_sha256; /* NULL unless committed */
    const char *mime;
    uint32_t width, height;
    uint64_t bytes;
    const char *size_status;
    bool have_seed;
    int64_t seed;
    const char *request_id; /* provider supplied only; NULL when absent */
    const char *error_code, *error_message;
    const char *source_manifest, *source_operation; /* replay/artifact lineage */
    /* Set only by a local export or contact sheet; NULL keeps the provider
     * record byte-for-byte what it already was. */
    const tny_image_transform *transform;
} tny_image_record;

/* Read side: bounded owned copies, so no field borrows a freed document. */
struct tny_image_manifest {
    char *path; /* the manifest's own absolute path */
    char operation_id[TNY_IMAGE_OPERATION_ID_MAX];
    char *operation;
    char *status;
    char *workspace;
    char *started, *finished;
    char *prompt;
    char *output;
    bool committed;
    tny_image_reference references[TNY_IMAGE_REFERENCES_MAX];
    size_t reference_count;
    char *requested_provider, *requested_model, *requested_quality, *requested_size;
    char *effective_provider, *effective_model, *effective_size;
    /* The single native artifact of a successful operation. */
    char *artifact_path;
    char artifact_sha256[TNY_IMAGE_SHA256_HEX];
    char *artifact_role;
    uint32_t width, height;
    char *mime;
    uint64_t bytes;
    char *size_status;
    /* Derived-transform lineage; all zero/NULL for a provider record. The
     * ordered sources live here rather than in references[], which stays
     * bounded by TNY_IMAGE_REFERENCES_MAX for every existing replay caller. */
    bool derived;
    char *transform_operation, *transform_policy, *transform_gravity, *transform_background;
    char *transform_format, *transform_labels, *transform_tool, *transform_tool_version;
    uint32_t canvas_width, canvas_height, grid_columns, grid_rows, cell_width, cell_height;
    tny_image_reference *sources;
    tny_image_source_dimensions source_dimensions[TNY_IMAGE_SOURCES_MAX];
    size_t source_count;
};

/* <output>.tny-image-<operation-id>.json; malloc'd, NULL on exhaustion. */
char *tny_image_manifest_path(const char *output, const char *operation_id);
/* True when a path's own name is a manifest tny would write. Such a name is
 * refused as an image destination so an operation cannot clobber a record. */
bool tny_image_manifest_reserved_name(const char *path);

void tny_image_manifest_serialize(const tny_image_record *, buf_t *out);

/* Parse and validate one record. Bounded, version checked and strict about
 * every field replay depends on; a future version, malformed body or invalid
 * type fails closed before any file or network access. */
tny_image_manifest *tny_image_manifest_load(const char *path, char *err, size_t errlen);
/* Parse already acquired bounded metadata, retaining the supplied identity;
 * never reopen its path. Used by confined job selection. */
tny_image_manifest *tny_image_manifest_parse(const char *path, const void *, size_t, char *err,
                                             size_t errlen);
void tny_image_manifest_free(tny_image_manifest *);

/* True when this record documents a local export or contact sheet instead of
 * a provider operation. Such a record can still be edited from (its artifact
 * is real, hashed bytes), but rerunning it as a provider request would invent
 * a generation that never happened, so every surface refuses that explicitly. */
bool tny_image_manifest_derived(const tny_image_manifest *);

/* "running" resolves to "interrupted" when no live writer guard holds the
 * recorded destination for this operation: an abandoned intent is never
 * represented as success merely because a file exists. */
const char *tny_image_manifest_observed_status(const tny_image_manifest *);

/* Resolve a recorded relative path against the record's own workspace, never
 * against an unrelated caller directory. malloc'd. */
char *tny_image_manifest_resolve(const tny_image_manifest *, const char *path);

#endif
