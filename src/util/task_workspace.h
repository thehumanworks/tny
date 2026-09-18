/* Local Git file isolation, not a permission boundary or job authority. */
#ifndef TNY_TASK_WORKSPACE_H
#define TNY_TASK_WORKSPACE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct task_workspace task_workspace;
typedef struct {
    const char *run; /* existing job ID: exactly 32 lowercase hex digits */
    int task;        /* stable item index */
    int attempt;     /* positive job attempt */
} task_workspace_id;

typedef struct {
    char *run;
    int task, attempt;
    char *path, *branch, *base, *origin, *origin_branch, *revision;
    char *patch;  /* binary-capable tracked diff from base, including dirty tracked files */
    char *status; /* porcelain, including untracked/ignored names; NOT their contents */
    bool dirty;   /* includes ignored files: cleanup must not delete these either */
} task_workspace_result;

bool task_workspace_id_valid(task_workspace_id id);
/* base=NULL requires a clean launch tree. An explicit Git commit-ish acknowledges
 * exclusion of launch edits. Remote URI and non-Git cwd fail before state creation.
 * Success holds a nonblocking per-attempt lock until close. No existing path or
 * branch is adopted. Existing completed preparation is opened with open instead. */
int task_workspace_prepare(const char *cwd, task_workspace_id id, const char *base,
                           task_workspace **out, char *err, size_t cap);
int task_workspace_open(const char *cwd, task_workspace_id id, task_workspace **out, char *err,
                        size_t cap);
const char *task_workspace_path(const task_workspace *w);
/* Persist and return provenance + snapshot. Refuses oversized (>~16KB) output,
 * never returns a silently truncated patch. Caller must stop workers first. */
int task_workspace_inspect(task_workspace *w, task_workspace_result *out, char *err, size_t cap);
void task_workspace_result_free(task_workspace_result *r);
/* Explicit committed-result merge into recorded launch branch only. Requires
 * both trees clean. 0=merged (may need commit), 1=conflict preserved at origin,
 * -1=refused/error. Never resets or aborts. No validation/acceptance implied. */
int task_workspace_integrate(task_workspace *w, char *err, size_t cap);
/* Clean owned tree only; branch, metadata and final snapshot are retained.
 * Repeated cleanup is successful. Never use on an active worker. */
int task_workspace_cleanup(task_workspace *w, char *err, size_t cap);
void task_workspace_close(task_workspace *w);
#ifdef __cplusplus
}
#endif
#endif
