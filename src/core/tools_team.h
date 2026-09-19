#ifndef TNY_TOOLS_TEAM_H
#define TNY_TOOLS_TEAM_H
#include "core/team_control.h"
#include "core/tools.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Borrowed, trusted runtime snapshot. No caller identity is read from JSON. */
void tool_team_caller(const tools_env *env, tny_team_caller *out);
bool tool_team_available(const tny_ctx *ctx);
char *tool_team_detail(tools_env *env, tny_team_op op, yyjson_val *request, char **error);
int tool_team_run(tools_env *env, tny_team_op op, yyjson_val *request, buf_t *out, char *err,
                  size_t errlen);
#ifdef __cplusplus
}
#endif
#endif
