#ifndef TNY_LIB_CUSTOM_TOOLS_H
#define TNY_LIB_CUSTOM_TOOLS_H

#include "tny/tny.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct custom_tool_registry custom_tool_registry;

custom_tool_registry *custom_tools_new(void);
void custom_tools_free(custom_tool_registry *registry);
bool custom_tools_in_callback(const custom_tool_registry *registry);
size_t custom_tools_active_count(custom_tool_registry *registry);

int32_t custom_tools_register(custom_tool_registry *registry, void *runtime,
                              const tny_tool_spec_v1 *spec,
                              tny_tool_registration **out);
int32_t custom_tools_unregister(tny_tool_registration *registration);
void *custom_tool_registration_runtime(tny_tool_registration *registration);

tny_tool_registration *custom_tools_find(custom_tool_registry *registry,
                                         const char *name);
const char *custom_tool_name(const tny_tool_registration *registration);
const char *custom_tool_schema(const tny_tool_registration *registration);
bool custom_tool_sensitive(const tny_tool_registration *registration);
uint64_t custom_tool_argument_limit(const tny_tool_registration *registration);
char *custom_tools_schema_json(custom_tool_registry *registry);

/* 0 synchronous result, 1 pending async result, negative stable status. */
int32_t custom_tool_invoke(tny_tool_registration *registration,
                           const char *arguments_json,
                           tny_tool_call **out_call, char **out_result,
                           bool *out_is_error);
/* 1 completed result taken, 0 still pending, -1 invalidated. */
int custom_tool_take(tny_tool_call *call, char **out_result,
                     bool *out_is_error);
void custom_tool_invalidate(tny_tool_call *call);
void custom_tools_invalidate_all(custom_tool_registry *registry);
int custom_tools_wake_fd(custom_tool_registry *registry);
void custom_tools_wake_drain(custom_tool_registry *registry);

int32_t custom_tool_complete(tny_tool_call *call, uint64_t generation,
                             const tny_tool_result_v1 *result);

#endif
