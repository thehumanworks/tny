/* Native process cleanup, within the host OS seam (ADR 0081). */
#ifndef TNY_PROCESS_H
#define TNY_PROCESS_H

#include <stdbool.h>
#include <sys/types.h>

/* A CLI gives its session this long to cancel before a verified hard stop.
 * Launchers must allow that deadline to elapse before killing the CLI. */
#define TNY_PROCESS_CANCEL_GRACE_MS 5000

/* Kill a detached runner and its descendants, including separate process
 * groups created by hosts/tools. Never targets this process or its group.
 * macOS/Linux enumerate descendants; other native hosts retain group kill.
 * Returns 0 on success, -1 if enumeration or signalling failed. Even on
 * error the known descendants and root are killed. wasm: unsupported. */
int tny_process_kill_tree(pid_t root);
/* Read-only probe for the required native identity/signalling ABI. */
bool tny_process_tree_supported(void);

/* Jobs cancellation: capture generation-safe references, freeze, then kill
 * all captured owned nodes (including children of auto-reaping parents).
 * Caller must retain its direct child unreaped until this call. This function
 * reaps that child itself and sets the reaped/status outputs on an observed wait result.
 * 0 requires kill(pid,0)==ESRCH for EVERY captured PID, not just zombie state.
 * -1 means identity, signalling, reaping or strict absence was not established. */
int tny_process_stop_owned_tree(pid_t root, int *status, bool *reaped);

/* Absolute path of the running executable, malloc'd. NULL when the host
 * cannot say (errno ENOTSUP on wasm); callers must not fall back to a PATH
 * lookup, which could start a different binary. */
char *tny_process_self_path(void);

/* Start argv[0] — an absolute path; no shell, no PATH search — with exactly
 * envp, stdin from in_fd, stdout into out_fd and stderr discarded. The child
 * leads a new process group with INT/TERM/HUP/PIPE at their defaults and an
 * empty signal mask. The caller owns it: reap *pid, and signal it or
 * tny_process_kill_tree() it to cancel. Returns 0, or an errno value
 * from a negative path preflight or spawn (ENOTSUP on wasm). The preflight
 * diagnoses known path failures; it is not authorization or an atomic exec check. */
int tny_process_spawn(char *const argv[], char *const envp[], int in_fd, int out_fd, pid_t *pid);

/* One explicit descriptor handover: the caller's `source` becomes `target` in
 * the child. Nothing else is inherited. */
typedef struct {
    int source;
    int target;
} tny_fd_mapping;

#define TNY_PROCESS_MAX_FD_MAPPINGS 4

/* tny_process_spawn with explicit descriptor mappings instead of the fixed
 * stdin/stdout pair (docs/adr/0093). Every source is first staged into a
 * collision-free high range with F_DUPFD_CLOEXEC, so a source that happens to
 * sit on another mapping's target is never clobbered, whatever the caller's
 * descriptor allocation looks like. Targets must be distinct and below 10;
 * any of 0/1/2 without a mapping is opened on /dev/null. A staged copy is
 * closed in the parent before returning, so the caller keeps owning exactly
 * the descriptors it passed in. Same return contract as tny_process_spawn;
 * EINVAL for a malformed mapping set. */
int tny_process_spawn_mapped(char *const argv[], char *const envp[], const tny_fd_mapping *maps,
                             int n_maps, pid_t *pid);

/* Durable-job ownership and admission (ADR 0099). These APIs launch only the
 * trusted current executable; arbitrary tools keep using spawn_mapped above. */
typedef struct tny_process_scope tny_process_scope;
#define TNY_JOB_SCOPE_PREFIX     "TNY_JOB_SCOPE_"
#define TNY_JOB_SCOPE_TIMEOUT_MS 5000

/* Dedicated detached supervisor only. Establishes its single-reaper SIGCHLD
 * policy and, on MSYS2, a lifetime Job retained until process teardown. */
int tny_process_supervisor_init(void);
/* Read-only platform choice; no process is attached by this function. */
bool tny_process_scope_native_jobs(void);
/* Accepts an environment name or NAME=value. Reserved fields cannot be lent
 * through ask_env or inherited by unrelated child launches. */
bool tny_process_scope_env_reserved(const char *entry);
/* First action in main: no fields -> no-op; malformed/partial fields -> error.
 * A requested MSYS child joins its scope, sends ACK on fd3 and consumes one GO
 * byte from stdin before returning. Every private field is removed first. */
int tny_process_scope_admit(void);
/* Successful spawn transfers the sole consuming wait authority to the scope.
 * Parent owns in_fd/out_fd; scope owns its private admission reader. No wait
 * for admission occurs here. Returns an errno value, zero on success. */
int tny_process_scope_spawn(char *const argv[], char *const envp[], int in_fd, int out_fd,
                            tny_process_scope **out);
pid_t tny_process_scope_pid(const tny_process_scope *scope);
/* Nonblocking ACK progress: 1 ready, 0 pending, -1 malformed/timeout/I/O. */
int tny_process_scope_ack(tny_process_scope *scope);
/* Call under the current attempt/cancel transaction. One nonblocking GO write:
 * 1 released, 0 would block, -1 error. Never send prompt bytes before 1. */
int tny_process_scope_go(tny_process_scope *scope, int prompt_fd);
/* Sole consuming reaper: 1 observed exit (also on subsequent calls), 0 live,
 * -1 unknown. Observed status survives later cancellation/cleanup failures. */
int tny_process_scope_reap(tny_process_scope *scope, int *status);
/* Request termination, not proof of cleanup. Before GO, only the unreaped
 * direct child can be signalled; after GO, MSYS uses only retained Job authority.
 * The caller must separately prove reaping, accounting and strict absence. */
int tny_process_scope_terminate(tny_process_scope *scope);
/* MSYS accounting only: 1 zero, 0 nonzero, -1 unknown. This does NOT prove
 * captured POSIX identity absence or log drainage. */
int tny_process_scope_empty(const tny_process_scope *scope);
/* Advance complete cleanup outside state locks: 1 proven, 0 pending, -1
 * unknown. force requests cancellation; after natural root exit residual
 * members are stopped too. *forced reports whether this scope needed a stop.
 * MSYS captures bounded read-only membership, closes signalled handles,
 * requires root reap, zero accounting and strict captured-PID absence. */
int tny_process_scope_cleanup(tny_process_scope *scope, bool force, bool *forced);
/* Release after the caller's complete cleanup proof. Refuses an unreaped
 * direct child or a nonempty/unknown MSYS Job. Returns an errno value. */
int tny_process_scope_destroy(tny_process_scope *scope);

/* Cooperative parent-loss watch for a detached job's child (docs/adr/0093).
 * tny_process_expect_parent() records the supervisor pid this process was
 * told to run under — the private TNY_JOB_PARENT_PID value, parsed once — and
 * tny_process_parent_lost() answers whether getppid() still reports it.
 * Without that private value the watch is disabled and the answer is always
 * false, so ordinary CLI runs are unaffected. This never signals a pid and
 * never reads one from a metadata file: the parent/child relationship is
 * kernel-maintained, so a reused pid cannot resurrect a dead supervisor. */
#define TNY_JOB_PARENT_ENV "TNY_JOB_PARENT_PID"
void tny_process_expect_parent(pid_t parent);
bool tny_process_parent_lost(void);

#endif
