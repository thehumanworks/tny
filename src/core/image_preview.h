/* image_preview.h — admission vocabulary for explicitly requested generated
 * image previews (docs/adr/0096).
 *
 * This is deliberately tiny: the status an owning native session reports when
 * an explicit preview asks to ride the existing pending-image queue, the safe
 * machine-readable refusal codes that go back over the control channel, and
 * the expected-hash check performed against the exact captured bytes. The
 * queue itself stays in core/tools.h and the user-message shape stays in
 * core/image.h; nothing here knows about transports or providers.
 *
 * `queued` is a time-local receipt: the entry was accepted for the owning tool
 * batch's next provider request. It is never a claim that a model perceived
 * pixels, and a later cancellation, policy change or failed turn can still
 * prevent delivery (A15 D2).
 */
#ifndef TNY_IMAGE_PREVIEW_H
#define TNY_IMAGE_PREVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    /* Captured and queued for this batch's next request. */
    TNY_IMAGE_PREVIEW_QUEUED = 0,
    /* The conversation provider is not configured-true for image input, or
     * this runtime has no native pending-image queue at all. Unknown is not
     * support (docs/adr/0089). */
    TNY_IMAGE_PREVIEW_UNSUPPORTED,
    /* No owning native session/turn to attach to. */
    TNY_IMAGE_PREVIEW_UNAVAILABLE_SESSION,
    /* An owning native session exists, but it is not inside a tool batch that
     * can still make another request (streaming only, cancelled, denied, or
     * out of step budget). */
    TNY_IMAGE_PREVIEW_TURN_NOT_READY,
    /* The image itself was refused: roots, format, size, capacity or hash. */
    TNY_IMAGE_PREVIEW_FAILED
} tny_image_preview_status;

/* Stable wire name: "queued", "unsupported", "unavailable_session",
 * "turn_not_ready" or "failed". Never NULL. */
const char *tny_image_preview_status_name(tny_image_preview_status status);

/* Safe machine-readable refusal codes. Static strings that name no user path,
 * prompt or provider text. */
#define TNY_IMAGE_PREVIEW_CODE_ROOTS      "outside_allowed_roots"
#define TNY_IMAGE_PREVIEW_CODE_HASH       "hash_mismatch"
#define TNY_IMAGE_PREVIEW_CODE_FORMAT     "unsupported_format"
#define TNY_IMAGE_PREVIEW_CODE_TOO_LARGE  "image_too_large"
#define TNY_IMAGE_PREVIEW_CODE_CAPACITY   "queue_full"
#define TNY_IMAGE_PREVIEW_CODE_UNREADABLE "unreadable_path"
#define TNY_IMAGE_PREVIEW_CODE_CAPABILITY "image_input_not_configured"
#define TNY_IMAGE_PREVIEW_CODE_NOT_READY  "no_continuable_tool_batch"
#define TNY_IMAGE_PREVIEW_CODE_NO_SESSION "no_native_session"
#define TNY_IMAGE_PREVIEW_CODE_INTERNAL   "internal_error"

/* The one sentence a not-delivered preview is reported with. The owning
 * backend emits it before ending the turn, so a preview failure can never be
 * read as visual approval. */
#define TNY_IMAGE_PREVIEW_NOT_DELIVERED "IMAGE_PREVIEW_NOT_DELIVERED"

/* Exactly 64 lowercase hex digits — the shape tny's own artifact hashes use.
 * Anything else (NULL, short, uppercase, padded) is invalid. */
bool tny_image_preview_hash_valid(const char *hex);

/* Compare `expect` against the SHA-256 of exactly these bytes. False when the
 * hash is malformed, the digest is unavailable or the bytes differ; there is
 * no second read of any path. */
bool tny_image_preview_hash_matches(const uint8_t *data, size_t len, const char *expect);

#endif
