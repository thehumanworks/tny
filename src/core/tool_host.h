#ifndef TNY_CORE_TOOL_HOST_H
#define TNY_CORE_TOOL_HOST_H

#include <stdint.h>

/* `tny --tool-host MODE [--project DIR] [--global DIR]`: the embedded
 * interpreter running python/tny/_host.py (ADR 0218). MODE is `catalog`
 * (print the catalog JSON and exit) or `serve` (newline-delimited MCP on
 * stdin/stdout). */
int tny_tool_host_main(int argc, char **argv);
/* Hash of the embedded python/tny sources, part of the catalog cache key. */
uint64_t tny_tool_host_package_hash(void);

#endif
