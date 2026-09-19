/* Explicit workspace control. Preparation belongs exclusively to the scheduler.
 * This is a trusted service, not a permission bypass: adapters must authorize
 * the exact permission identity and detail before calling run. */
#ifndef TNY_TOOLS_WORKSPACE_H
#define TNY_TOOLS_WORKSPACE_H
#include "core/tools.h"
#ifdef __cplusplus
extern "C" {
#endif
// C/C++ boundary: retain the C enum layout. NOLINTNEXTLINE(performance-enum-size)
typedef enum {
    TNY_WORKSPACE_INSPECT = 0,
    TNY_WORKSPACE_INTEGRATE,
    TNY_WORKSPACE_CLEANUP,
    TNY_WORKSPACE_NONE
} tny_workspace_op;

tny_workspace_op tny_workspace_op_parse(const char *name);
const char *tny_workspace_permission_tool(tny_workspace_op op);
tny_workspace_op tool_workspace_op(const char *name);
bool tool_workspace_available(const tny_ctx *ctx, const char *name);
/* argv excludes "tny task-workspace". Strict grammar, no stdin or arbitrary cwd.
 * Request is {run,task,attempt}. On success caller frees *request_out. */
tny_workspace_op tny_workspace_parse_argv(int argc, char **argv, char **request_out, bool *json_out,
                                          const char **error);
/* Validates platform and request, returns allocated canonical permission detail.
 * No Git or job mutation. Error is a static safe string. */
char *tny_workspace_detail(const tny_ctx *ctx, tny_workspace_op op, yyjson_val *args,
                           const char **error);
/* Already permitted request only. Holds the job OWNER lock, never a state lock
 * across Git. Revalidates authoritative DAG state/attempt/cleanup, then binds the
 * helper to recorded launch cwd. Mutations require caller cwd == launch cwd.
 * 0=operation completed (NOT accepted), 1=refused, 2=Git failure/conflict.
 * All emitted results say verification:unverified and accepted:false. */
int tny_workspace_run(tny_ctx *ctx, tny_workspace_op op, yyjson_val *args, buf_t *out, char *err,
                      size_t cap);
int tool_workspace_run(tools_env *env, tny_workspace_op op, yyjson_val *args, buf_t *out, char *err,
                       size_t cap);
void tny_workspace_render_human(const char *json, buf_t *out);
#ifdef __cplusplus
}
#endif
#endif
