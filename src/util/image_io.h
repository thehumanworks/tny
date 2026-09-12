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

/* Private paid-image publication. Caller holds the canonical writer guard.
 * Preflight requires an absent leaf (including dangling links) and probes
 * atomic hard-link capability in its directory before any provider request.
 * Unsupported platforms refuse here, never via a check-then-rename fallback. */
int tny_image_io_no_replace_preflight(const char *canonical, char *err, size_t errlen);
/* Install a validated same-directory temporary. -1 means not committed,
 * 0 committed and cleaned, 1 committed with a retained temporary name.
 * The final output is never removed after a successful link. */
int tny_image_io_publish(const char *tmp, const char *canonical, bool no_replace);

/* Read a regular file of at most max bytes. -1 when it is missing, not
 * regular, larger than max, or unreadable. out is left empty on failure. */
int tny_image_io_read_bounded(const char *path, size_t max, buf_t *out);
/* Read acquired bytes beneath one absolute directory authority. No component
 * may be a symlink or traversal segment, including the leaf. The bounded read
 * uses the same acquired descriptor; path replacement cannot redirect it. */
int tny_image_io_read_confined(const char *root, const char *path, size_t max, buf_t *out);

/* Lowercase hex SHA-256 of len bytes. False when the digest is unavailable. */
bool tny_image_io_sha256_hex(const void *data, size_t len, char out[65]);

/* ---- inputs and the destination commit protocol (ADR 0094) ---------------
 *
 * An export reads approved sources and installs one derived artifact. Two
 * properties are load-bearing, and they are enforced here rather than by each
 * caller: the bytes that are hashed are the bytes that were read, and no
 * commit step ever follows the destination name for writing.
 *
 * The owner of the protocol is the caller that already holds the canonical
 * writer guard for the same destination (tny_image_io_guard_acquire). That
 * guard serializes cooperating tny operations; it is not, and is not claimed
 * to be, exclusion against an arbitrary external writer. What this protocol
 * does guarantee against such a writer is narrower and checkable: the
 * destination identity observed at open is rechecked with
 * fstatat(AT_SYMLINK_NOFOLLOW) immediately before the install, an unexpected
 * change is refused, and the install itself is a directory-entry operation
 * (linkat or renameat) relative to a retained parent-directory fd, never an
 * open-and-truncate of the target. A symlink or hard link substituted after
 * the check is therefore never written through, so the bytes of a source
 * inode cannot be modified by an export. */

/* The identity of one file, or its documented absence. */
typedef struct {
    bool present;
    uint64_t dev, ino;
} tny_image_io_id;

/* Read a regular file of at most max bytes and record the identity of the
 * very file descriptor those bytes came from, so a later alias comparison
 * cannot be defeated by replacing the path between the read and the check.
 * -1 leaves out empty and fills err. */
int tny_image_io_read_input(const char *path, size_t max, buf_t *out, tny_image_io_id *id,
                            char *err, size_t errlen);

typedef struct tny_image_commit tny_image_commit;

/* Hold the destination's parent directory open and capture what the leaf is
 * right now. An existing leaf without `overwrite` fails here; with it, only an
 * ordinary single-link regular file may be replaced. NULL fills err. */
tny_image_commit *tny_image_io_commit_open(const char *canonical, bool overwrite, char *err,
                                           size_t errlen);
/* The identity captured at open, for alias checks against every source. */
tny_image_io_id tny_image_io_commit_target(const tny_image_commit *);
/* Create this operation's stage file with
 * openat(parent_fd, generated-name, O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, 0600),
 * write the complete bytes and fsync them. Never a full-path mkstemp, so a
 * swapped parent symlink cannot redirect the stage. */
int tny_image_io_commit_stage(tny_image_commit *, const void *data, size_t len, char *err,
                              size_t errlen);
/* Recheck the target identity and install the staged bytes under the final
 * name: linkat + unlink for a fresh destination, so a competing creator loses
 * atomically with EEXIST, or renameat for an explicit overwrite. */
int tny_image_io_commit_finish(tny_image_commit *, char *err, size_t errlen);
/* Release the protocol. An unfinished stage file is removed, so a failed or
 * cancelled export leaves neither debris nor a partial artifact. */
void tny_image_io_commit_close(tny_image_commit *);

#endif
