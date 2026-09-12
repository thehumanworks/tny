/* image.h — magic-byte MIME, data-URL encoding, session image parts.
 * Used by --image, clipboard-image validation and the native read_image tool. */
#ifndef TNY_IMAGE_H
#define TNY_IMAGE_H

#include "core/session.h"
#include "util/util.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Read a borrowed, opened stream to EOF with an actual IMAGE_MAX_BYTES bound.
 * NULL on I/O/allocation failure or oversize; never closes the stream. */
uint8_t *image_read_bounded(FILE *file, size_t *len_out, bool *too_large);

#define IMAGE_MAX_BYTES (8u * 1024u * 1024u)

/* png / jpeg / gif / webp from magic bytes. NULL if unrecognized. */
const char *image_mime(const uint8_t *data, size_t n);

/* Append "data:<mime>;base64,<payload>" to out. */
void image_data_url(const uint8_t *data, size_t n, const char *mime, buf_t *out);

/* Load a file, reject non-images and files over IMAGE_MAX_BYTES.
 * Returns malloc'd bytes (caller frees). *mime_out is a static string. */
uint8_t *image_load(const char *path, size_t *len_out, const char **mime_out, char *err,
                    size_t errlen);
/* Same, plus the stable machine-readable reason from core/image_preview.h for
 * the failure (*code_out is a static string, NULL on success). */
uint8_t *image_load_ex(const char *path, size_t *len_out, const char **mime_out,
                       const char **code_out, char *err, size_t errlen);

/* One already-loaded image for the shared user-message builder. The bytes are
 * borrowed for the duration of the call. */
typedef struct {
    const uint8_t *data;
    size_t len;
    const char *mime; /* static; NULL falls back to the data-URL default */
} tny_image_part;

/* user message: text part + one image_url part per path. 0 ok, -1 on error.
 * The path-loading wrapper around the builder below: `tny ask --image`, the
 * runner's initial images and every other existing caller keep this shape. */
int session_add_user_images(tny_session_state *s, const char *text, const char **paths, char *err,
                            size_t errlen);
/* The same established user message built from bytes that were already loaded
 * and retained elsewhere — the captured pending-image queue (docs/adr/0096).
 * No path is read here, so a file rewritten after capture cannot substitute
 * another version. Nothing is appended to the transcript unless every part
 * was built. 0 ok, -1 on error. */
int session_add_user_loaded_images(tny_session_state *s, const char *text,
                                   const tny_image_part *parts, int n, char *err, size_t errlen);

#endif
