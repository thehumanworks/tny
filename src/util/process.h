/* Native process cleanup, within the host OS seam (ADR 0081). */
#ifndef TNY_PROCESS_H
#define TNY_PROCESS_H

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

#endif
