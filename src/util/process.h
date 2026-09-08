/* Native process cleanup, within the host OS seam (ADR 0081). */
#ifndef TNY_PROCESS_H
#define TNY_PROCESS_H

#include <sys/types.h>

/* Kill a detached runner and its descendants, including separate process
 * groups created by hosts/tools. Never targets this process or its group.
 * macOS/Linux enumerate descendants; other native hosts retain group kill.
 * Returns 0 on success, -1 if enumeration or signalling failed. Even on
 * error the known descendants and root are killed. wasm: unsupported. */
int tny_process_kill_tree(pid_t root);

#endif
