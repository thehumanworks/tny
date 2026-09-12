/* image_preview.c — see core/image_preview.h (docs/adr/0096). */
#include "core/image_preview.h"

#include "util/image_io.h"

#include <string.h>

const char *tny_image_preview_status_name(tny_image_preview_status status) {
    switch (status) {
    case TNY_IMAGE_PREVIEW_QUEUED: return "queued";
    case TNY_IMAGE_PREVIEW_UNSUPPORTED: return "unsupported";
    case TNY_IMAGE_PREVIEW_UNAVAILABLE_SESSION: return "unavailable_session";
    case TNY_IMAGE_PREVIEW_TURN_NOT_READY: return "turn_not_ready";
    case TNY_IMAGE_PREVIEW_FAILED: return "failed";
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
