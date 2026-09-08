/* Bounded, argv-only Git subprocesses in the host OS seam. */
#ifndef TNY_GIT_H
#define TNY_GIT_H
#include "util/util.h"

/* args excludes git and -C cwd; NULL-terminated. Captures combined output.
 * No terminal input, shell expansion, or inherited Git repository overrides. */
int git_run(const char *cwd, const char *const *args, buf_t *out);
#endif
