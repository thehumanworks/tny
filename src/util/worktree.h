/* Managed local Git worktrees. Closing never removes files or branches. */
#ifndef TNY_WORKTREE_H
#define TNY_WORKTREE_H
#include <stdbool.h>
#include <stddef.h>

typedef struct tny_worktree {
    char *name, *path, *origin, *origin_ref, *branch, *common, *gitdir;
    int lock_fd;
    bool created;
    struct tny_worktree *next; /* frontend-owned list of visited worktrees */
} tny_worktree;

bool worktree_name_valid(const char *name);
tny_worktree *worktree_enter(const char *cwd, const char *name, char *err, size_t errlen);
int worktree_merge(tny_worktree *w, char *err, size_t errlen);
int worktree_remove(tny_worktree *w, char *err, size_t errlen);
void worktree_close(tny_worktree *w);
#endif
