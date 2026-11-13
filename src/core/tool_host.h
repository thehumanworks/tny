#ifndef TNY_CORE_TOOL_HOST_H
#define TNY_CORE_TOOL_HOST_H

#include <stdint.h>

/* The tool host is the user's python3 running python/tny/_host.py (ADR 0218,
 * ADR 0222). Its first stdin line is this package: one JSON object mapping
 * each module name to [is_package, source], newline-terminated. malloc'd. */
char *tny_tool_host_package_json(void);
/* Hash of the embedded python/tny sources, part of the catalog cache key. */
uint64_t tny_tool_host_package_hash(void);

#endif
