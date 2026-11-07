/* tool_extensions.h — Python tool extensions (docs/extensions.md, ADR 0218).
 *
 * Extensions are Python modules in <workspace>/.tny/extensions/tools/ and
 * ~/.tny/extensions/tools/. One host process per tny process (the embedded
 * interpreter, `tny --tool-host serve`) imports them and serves their tools
 * over newline-delimited MCP. The catalog is cached on disk, keyed by a
 * fingerprint of the module files, so listing tools never starts Python
 * unless a module changed. */
#ifndef TNY_CORE_TOOL_EXTENSIONS_H
#define TNY_CORE_TOOL_EXTENSIONS_H

#include "core/config.h"

#include <stdbool.h>

#define TNY_TOOL_EXT_DEFAULT_TIMEOUT_MS 300000

/* False when switched off (settings, env, flags), in library/prompt-optimised
 * runs and on wasm. */
bool tny_tool_ext_enabled(const tny_ctx *ctx);

/* The two search directories, malloc'd; either may be NULL (project tools
 * off, or the same directory as the global one). They need not exist. */
void tny_tool_ext_dirs(const tny_ctx *ctx, char **project_dir, char **global_dir);

/* The host's catalog object: {"tools":[...],"groups":[...],"errors":[...],
 * "warnings":[...]}; "groups" names each module and sub-group with its
 * description, for help.
 * refresh bypasses the disk cache (and rewrites it). Always returns JSON when
 * enabled; an unstartable host is reported in "errors". NULL when disabled
 * or out of memory. */
char *tny_tool_ext_report(tny_ctx *ctx, bool refresh);

/* The report from the catalog cache only, when it matches the current
 * module files; NULL on a miss. Never starts the tool host, so help can
 * show extensions without running Python or contacting proxied servers. */
char *tny_tool_ext_report_cached(tny_ctx *ctx);

/* Extension module names found on disk (project first, then global, each
 * name once, sorted), using the host's discovery rules: NAME.py files and
 * NAME/ folders holding __main__.py, skipping `.` and `_` prefixes. Sets
 * *count; free with tny_tool_ext_names_free. */
char **tny_tool_ext_module_names(const tny_ctx *ctx, size_t *count);
void tny_tool_ext_names_free(char **names, size_t count);

/* Model-facing entries, `[{"type":"function","function":{...}}]`, skipping
 * names for which skip(name, ud) is true (native tools win). "[]" when
 * there are none; NULL only when out of memory. */
char *tny_tool_ext_functions_json(tny_ctx *ctx, bool (*skip)(const char *name, void *ud), void *ud);

/* The input schema (JSON text) of the extension tool `name`, or NULL when
 * there is no such tool. */
char *tny_tool_ext_schema(tny_ctx *ctx, const char *name);

/* Call one tool. Returns the result text (malloc'd) and sets *is_error.
 * cancelled is polled while waiting; a cancelled or timed-out call stops the
 * host, which restarts on the next call. */
char *tny_tool_ext_call(tny_ctx *ctx, const char *name, const char *args_json,
                        bool (*cancelled)(void *ud), void *ud, bool *is_error);

/* Where the host's stderr goes: -1 (default) appends to
 * ~/.tny/logs/tool-extensions.log; a CLI command passes 2 so prints and
 * tracebacks reach the terminal. Applies to hosts started afterwards. */
void tny_tool_ext_set_stderr(int fd);

/* Stop the host if one runs. Safe to call more than once. */
void tny_tool_ext_shutdown(void);

#endif
