/* Bounded PNG/JPEG/WebP header inspection and size-request semantics (ADR 0088).
 * Header metadata only: this is deliberately not a pixel decoder and proves
 * nothing about the image data after the header it walks. */
#ifndef TNY_IMAGE_DIMENSIONS_H
#define TNY_IMAGE_DIMENSIONS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Any larger edge is treated as impossible rather than reported. */
#define TNY_IMAGE_DIMENSION_MAX 1048576u

typedef enum {
    TNY_IMAGE_DIM_OK,           /* complete in-bounds header, positive dimensions */
    TNY_IMAGE_DIM_UNVERIFIABLE, /* truncated, malformed, inconsistent or out of range */
    TNY_IMAGE_DIM_UNSUPPORTED   /* recognized container, dimension encoding not supported */
} tny_image_dim_status;

typedef enum {
    TNY_IMAGE_SIZE_AUTO,
    TNY_IMAGE_SIZE_MATCH,
    TNY_IMAGE_SIZE_MISMATCH,
    TNY_IMAGE_SIZE_UNVERIFIABLE,
    TNY_IMAGE_SIZE_UNSUPPORTED
} tny_image_size_status;

/* Reads only what the format's own signature, length, segment and chunk fields
 * allow; every field is bounds checked against n before use. Width and height
 * are set to zero unless the result is TNY_IMAGE_DIM_OK. */
tny_image_dim_status tny_image_dimensions(const uint8_t *data, size_t n, uint32_t *width,
                                          uint32_t *height);
/* Exact "<width>x<height>" literal: decimal, no sign, no leading zero, no
 * spaces, each edge 1..TNY_IMAGE_DIMENSION_MAX. Anything else is opaque. */
bool tny_image_size_parse(const char *size, uint32_t *width, uint32_t *height);
/* requested is the literal request ("auto" or NULL when none was made). */
tny_image_size_status tny_image_size_compare(const char *requested, tny_image_dim_status,
                                             uint32_t width, uint32_t height);
const char *tny_image_size_status_name(tny_image_size_status);
#endif
