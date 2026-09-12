/* Private artifact persistence primitives for the image service (ADR 0088).
 *
 * This is a host OS seam, like util/process.c and util/worktree.c: the one
 * place allowed to know whether the platform has advisory file locks. Native
 * builds own a destination through a live flock(2) handle; wasm owns it in a
 * per-instance table, because its filesystem has no cross-instance lock and
 * pretending otherwise would be a lie (docs/images.md). Nothing here knows
 * about images, JSON or manifests.
 */
#ifndef TNY_IMAGE_IO_H
#define TNY_IMAGE_IO_H

#include "util/util.h"
#include <stddef.h>

#define TNY_IMAGE_IO_PATH_MAX 4096
#define TNY_IMAGE_IO_ID_MAX   33 /* operation id plus NUL */

typedef struct tny_image_guard tny_image_guard;

/* Canonical absolute destination: the parent directory must already exist and
 * is resolved by the host, and the basename must be a plain component. An
 * existing leaf must be a regular file with exactly one link — a symlink,
 * directory, device, FIFO or hard-linked name is refused with a safe reason,
 * so an alias can never be written through or silently made ambiguous for the
 * manifest's path/hash record. Returns malloc'd; NULL fills err. */
char *tny_image_io_canonical(const char *path, char *err, size_t errlen);

/* True when both paths currently name the same file (device and inode), which
 * is how a hard link or a symlinked reference is recognised as an alias. */
bool tny_image_io_same_file(const char *a, const char *b);

/* Nonblocking exclusive writer guard for one canonical destination, tagged
 * with operation_id. NULL means another live owner holds it (err explains) or
 * the guard could not be created at all. Ownership is the returned live
 * handle, never a stored process id: normal exit, cancellation and process
 * death all release it. Release unlinks the file while still holding the lock
 * and a contender revalidates the inode it locked, so removing the file can
 * never leave two owners of one destination. */
tny_image_guard *tny_image_io_guard_acquire(const char *canonical, const char *operation_id,
                                            char *err, size_t errlen);
void tny_image_io_guard_release(tny_image_guard *);

/* Observe the current owner without taking it. Returns true when some live
 * owner holds this destination, and copies its operation id into out (empty
 * when the record could not be read). Never blocks. */
bool tny_image_io_guard_owner(const char *canonical, char out[TNY_IMAGE_IO_ID_MAX]);

/* Create path exclusively (O_EXCL, no symlink follow) with mode 0600 and write
 * len bytes. -1 when the path already exists or the write failed. */
int tny_image_io_write_new(const char *path, const void *data, size_t len);

/* Replace path atomically through a unique private temporary in the same
 * directory, mode 0600. The temporary name is never derived from the process
 * id, so two operations can never collide on it. */
int tny_image_io_replace(const char *path, const void *data, size_t len);

/* Read a regular file of at most max bytes. -1 when it is missing, not
 * regular, larger than max, or unreadable. out is left empty on failure. */
int tny_image_io_read_bounded(const char *path, size_t max, buf_t *out);

/* Lowercase hex SHA-256 of len bytes. False when the digest is unavailable. */
bool tny_image_io_sha256_hex(const void *data, size_t len, char out[65]);

#endif
