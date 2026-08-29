#ifndef TNY_TASKS_H
#define TNY_TASKS_H

#include "core/config.h"
#include "util/util.h"

#define TNY_TASK_NAME_MAX        64
#define TNY_TASK_BODY_MAX        (256u * 1024u)
#define TNY_TASK_DESCRIPTION_MAX 1024
#define TNY_TASK_COUNT_MAX       256u

enum {
    TNY_TASK_OK = 0,
    TNY_TASK_INVALID = -1,
    TNY_TASK_OOM = -2,
};

typedef struct {
    char *name;
    char *source;      /* workflow | project | user | builtin */
    char *description; /* optional frontmatter/builtin summary */
    bool valid;        /* false when the winning file is malformed */
} tny_task_info;

/* Resolve a task preset for a CLI context. Discovery is deliberately absent
 * from library contexts unless an explicit body is supplied by the caller. */
int tny_task_apply(tny_ctx *ctx, const char *name);
int tny_task_set_explicit(tny_ctx *ctx, const char *name, const char *body, const char *source);
bool tny_task_name_valid(const char *name);
bool tny_task_source_valid(const char *source);
/* Deterministic resolved listing. Sources are stable categories, never paths.
 * Invalid higher-precedence files remain visible and shadow lower sources. */
int tny_task_list(tny_ctx *ctx, tny_task_info **out, size_t *count);
void tny_task_list_free(tny_task_info *items, size_t count);
char *tny_task_names_joined(tny_ctx *ctx);
const char *tny_task_builtin_body(const char *name);

/* Append the task instruction section. Callers place it after normal
 * runtime/project context and before explicit user system additions. */
void tny_task_collect(const tny_ctx *ctx, buf_t *out);

#endif
