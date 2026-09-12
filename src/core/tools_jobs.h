/* tools_jobs.h — the typed job_* tools and the `tny jobs …` interception, on
 * the one durable-job service (docs/jobs.md, docs/adr/0093). Reading status or
 * logs never authorizes submitting, cancelling, retrying or removing. */
#ifndef TNY_TOOLS_JOBS_H
#define TNY_TOOLS_JOBS_H

#include "core/jobs.h"
#include "core/tools.h"

/* True for job_submit / job_control / job_status. */
bool tool_jobs_is_tool(const char *name);
/* The operation a typed call names, or TNY_JOBS_OP_NONE when the action is
 * unknown for that tool. No side effect. */
tny_jobs_op tool_jobs_op(const char *name, yyjson_val *args);
/* False where jobs cannot run at all (embedded runtimes, --ssh, and for the
 * execution tools any build that cannot own a child process). */
bool tool_jobs_available(const tny_ctx *ctx, const char *name);
/* Shared adapter: runs one already permitted call and appends the service's
 * result JSON to `out`. Returns 0, or the service's nonzero exit code with a
 * safe message in `err`. */
int tool_jobs_run(tools_env *env, tny_jobs_op op, yyjson_val *args, buf_t *out, char *err,
                  size_t errlen);

#endif
