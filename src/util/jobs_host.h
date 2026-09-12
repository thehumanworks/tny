/* jobs_host.h — the host OS seam for durable jobs (docs/adr/0093, 0081).
 *
 * Every platform-dependent operation durable jobs need lives here or in
 * util/process.c: private directories and files, advisory locks, descriptor
 * identity, canonical output paths and child reaping. core/jobs.c stays
 * portable and must not grow an ad-hoc platform fork. On wasm every operation
 * that needs a real process or an advisory lock answers ENOTSUP, which is what
 * the documented unsupported result is built from. */
#ifndef TNY_JOBS_HOST_H
#define TNY_JOBS_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* False where a job cannot actually be executed (wasm). Reading existing
 * records is still possible; claiming ownership of a process is not. */
bool tny_jobs_host_execution_supported(void);

/* mkdir(path, 0700) for the whole prefix. 0 ok, else an errno value. An
 * existing directory is accepted only when it is a real directory. */
int tny_jobs_host_mkdir_private(const char *path);

/* Atomic private write: a unique 0600 sibling temp, file fsync then rename over
 * `path`. 0 ok, else an errno value. Never follows a symlink at `path`. */
int tny_jobs_host_write_private(const char *path, const void *data, size_t len);
/* Atomic, synced write-once publication. EEXIST refuses existing history. */
int tny_jobs_host_write_once(const char *path, const void *data, size_t len);
/* Under caller's owner/state locks: publish once or verify exact existing
 * terminal snapshot bytes. Never follows links or replaces inconsistent data. */
int tny_jobs_host_snapshot(const char *path, const void *data, size_t len);

/* Open (creating, 0600, O_NOFOLLOW, O_CLOEXEC) a lock file. Returns the fd or
 * -1 with errno set. The file itself may carry a small record; callers hold
 * the lock while reading and rewriting it. */
int tny_jobs_host_lock_open(const char *path);

typedef enum {
    TNY_JOBS_LOCK_ACQUIRED = 0, /* this caller now holds it */
    TNY_JOBS_LOCK_BUSY,         /* EWOULDBLOCK: someone else holds it */
    TNY_JOBS_LOCK_ERROR         /* the answer is unknown */
} tny_jobs_lock_rc;

/* flock(LOCK_EX|LOCK_NB). Never blocks and never waits for another owner. */
tny_jobs_lock_rc tny_jobs_host_lock_try(int fd);
/* Release without closing (LOCK_UN). Closing the fd releases it too. */
void tny_jobs_host_lock_release(int fd);
void tny_jobs_host_lock_close(int fd);

typedef enum {
    TNY_JOBS_OWNER_FREE = 0, /* no live holder: the record is abandoned */
    TNY_JOBS_OWNER_HELD,     /* a live holder still owns it */
    TNY_JOBS_OWNER_UNKNOWN   /* could not be determined; treat as active */
} tny_jobs_owner_state;

/* Probe an owner lock file without waiting: LOCK_EX|LOCK_NB, released again
 * immediately on success. EWOULDBLOCK means active, and an unreadable or
 * unlockable file means unknown — never "free" (A11). */
tny_jobs_owner_state tny_jobs_host_owner_state(const char *lock_path);

/* True when `fd` is exactly the file at `path` (same device and inode). The
 * worker validates its inherited owner handle with this instead of trusting a
 * descriptor number. */
bool tny_jobs_host_fd_is_file(int fd, const char *path);

/* Admission must acquire the lock on the supplied description itself. */

/* Mark a descriptor close-on-exec (the owner handle, before any grandchild). */
int tny_jobs_host_set_cloexec(int fd);

/* Resolve an output destination to the canonical absolute path a reservation
 * is keyed on: the existing parent directory is resolved, the final component
 * is kept verbatim. malloc'd; NULL with *err set to a short reason when the
 * path cannot be used. `alias_out` reports an existing symlink or multiply
 * linked destination, which no job may claim. */
char *tny_jobs_host_canonical_output(const char *path, bool *alias_out, const char **err);

/* Consumes/closes both fds. Bounded private write + ack using tny_poll.
 * Returns an errno code; never emits payload/credential diagnostics. */
int tny_jobs_host_handshake(int payload_fd, int ack_fd, const char *payload, size_t len,
                            const char *id, int timeout_ms, bool (*cancelled)(void *), void *ud);

/* waitpid(pid, WNOHANG). 1 = reaped (status filled), 0 = still running,
 * -1 = unknown (already reaped or not ours). */
int tny_jobs_host_reap(pid_t pid, int *status);
/* kill(pid, sig) for a pid this process actually created and has not reaped. */
int tny_jobs_host_signal_owned(pid_t pid, int sig);
/* Sleep, bounded, without spinning the event loop. */
void tny_jobs_host_sleep_ms(int ms);
/* Detach from the invoking session so the supervisor outlives its submitter. */
void tny_jobs_host_detach_session(void);

#endif
