#ifndef TNY_EXECUTION_H
#define TNY_EXECUTION_H
#include "core/tools.h"
/* Executes exclusively in a fresh, privately connected server. Never falls back. */
char *tny_execution_run(tools_env *env, const char *arguments_json);
int tny_execution_server_main(void);
#endif
