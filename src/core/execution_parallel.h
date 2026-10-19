#ifndef TNY_EXECUTION_PARALLEL_H
#define TNY_EXECUTION_PARALLEL_H
#include "core/tools.h"
#include "util/execution_host.h"
#define TNY_TOOL_PARALLELISM 4
/* Only audited snapshot-isolated read effects may overlap. Everything else is
 * an ordered barrier; hooks and permissions always stay on the owner. */
bool tny_execution_parallel_safe(const tools_env *env, const tools_call *call);
int tny_execution_parallel_start(tny_exec_host *host, tools_env *env, const tools_call *call,
                                 int64_t deadline);
char *tny_execution_parallel_collect(tny_exec_host *host, tools_env *env, int64_t deadline,
                                     tny_exec_cancel_fn cancel, void *ud);
int tny_execution_tool_main(void);
#endif
