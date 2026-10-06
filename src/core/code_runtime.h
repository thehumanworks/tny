/* code_runtime.h — Python code cells run in a fresh child process with the
 * OS user's host authority plus an explicit tool callback (docs/adr/0180,
 * ADR 0179, ADR 0174). */
#ifndef TNY_CODE_RUNTIME_H
#define TNY_CODE_RUNTIME_H

#include <stdint.h>

/* Metered interpreter heap (Python objects and the stdlib codecs that use
 * CPython's allocators), including its initialization baseline. It stops a
 * runaway allocation loop; subprocesses are not metered. */
#define TNY_CODE_MEMORY_BYTES (1024ull * 1024u * 1024u)
/* Captured stdout/stderr kept in a result; the excess is summarized. */
#define TNY_CODE_OUTPUT_BYTES (64u * 1024u)
/* A final result: captured output, or an error line plus bounded output. */
#define TNY_CODE_RESULT_TEXT_BYTES (TNY_CODE_OUTPUT_BYTES + 512u)
#define TNY_CODE_SOURCE_BYTES      (256u * 1024u)
#define TNY_CODE_ARGUMENT_BYTES    (256u * 1024u)
#define TNY_CODE_NAME_BYTES        256u
#define TNY_CODE_TOOL_CALLS        64u
/* One nested result must fit a single private cell frame. */
#define TNY_CODE_TOOL_RESULT_BYTES (8u * 1024u * 1024u - 16u)
/* The terminal tool's ceiling also covers synchronous delegated inference.
 * Omitted deadlines use this bound; callers can request a shorter budget. */
#define TNY_CODE_MAX_TIMEOUT_MS     600000
#define TNY_CODE_DEFAULT_TIMEOUT_MS TNY_CODE_MAX_TIMEOUT_MS

/* Callback retains permission/allowlist/event authority. Return malloc-owned
 * text; the runtime consumes it. NULL signals allocation/execution failure. */
typedef char *(*tny_code_call_fn)(void *userdata, const char *name, const char *arguments_json);

/* Returns malloc-owned output, or an "error: code: ..." result; NULL only on
 * host allocation failure. Catalog and userdata are borrowed for this call.
 * The code runs in a fresh `--code-cell` child of the current executable
 * with this process's environment (minus tny's reserved process-scope
 * fields), stdin on /dev/null and stdout/stderr captured into the result.
 * Direct Python effects (files, sockets, subprocesses) have the OS user's
 * authority and are not tool-permission mediated. This caller re-checks
 * every nested call and owns the deadline: the child and its owned
 * descendants are stopped when it passes, whatever the child is doing.
 * Hosts without process support return an error. */
char *tny_code_run(const char *code, int timeout_ms, const char *catalog_json,
                   tny_code_call_fn call, void *userdata);

/* Trusted host variant: deadline is an absolute monotonic millisecond deadline
 * borrowed for this synchronous call. Only the host may extend it to exclude
 * human prompt waits; it is re-read after every callback. Call/output limits
 * remain cumulative. Updates must occur on this thread, never concurrently.
 * cwd is the cell's working directory (NULL: inherit this process's). */
char *tny_code_run_with_deadline(const char *code, const char *cwd, const int64_t *deadline,
                                 const char *catalog_json, tny_code_call_fn call, void *userdata);

/* Private `--code-cell` entry point: fd 3 is the parent's socket. */
int tny_code_cell_main(void);

#endif
