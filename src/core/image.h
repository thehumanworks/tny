/* image.h — magic-byte MIME, data-URL encoding, session image parts.
 * Used by --image, clipboard-image validation and the native read_image tool. */
#ifndef TNY_IMAGE_H
#define TNY_IMAGE_H

#include "core/session.h"
#include "util/util.h"

#include <stddef.h>
#include <stdint.h>

#define IMAGE_MAX_BYTES (8u * 1024u * 1024u)

/* png / jpeg / gif / webp from magic bytes. NULL if unrecognized. */
const char *image_mime(const uint8_t *data, size_t n);

/* Append "data:<mime>;base64,<payload>" to out. */
void image_data_url(const uint8_t *data, size_t n, const char *mime, buf_t *out);

/* Load a file, reject non-images and files over IMAGE_MAX_BYTES.
 * Returns malloc'd bytes (caller frees). *mime_out is a static string. */
uint8_t *image_load(const char *path, size_t *len_out, const char **mime_out, char *err,
                    size_t errlen);

/* user message: text part + one image_url part per path. 0 ok, -1 on error. */
int session_add_user_images(tny_session_state *s, const char *text, const char **paths, char *err,
                            size_t errlen);

#endif
