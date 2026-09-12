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
 * anything larger is rejected before it is parsed, let alone acted on. */
#define TNY_IMAGE_MANIFEST_MAX (256u * 1024u)

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
void tny_image_manifest_free(tny_image_manifest *);

/* "running" resolves to "interrupted" when no live writer guard holds the
 * recorded destination for this operation: an abandoned intent is never
 * represented as success merely because a file exists. */
const char *tny_image_manifest_observed_status(const tny_image_manifest *);

/* Resolve a recorded relative path against the record's own workspace, never
 * against an unrelated caller directory. malloc'd. */
char *tny_image_manifest_resolve(const tny_image_manifest *, const char *path);

#endif
