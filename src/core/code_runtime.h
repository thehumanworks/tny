/* code_runtime.h — Python code cells run in a fresh, OS-confined child
 * process over an explicit tool callback (docs/adr/0179, ADR 0174). */
#ifndef TNY_CODE_RUNTIME_H
#define TNY_CODE_RUNTIME_H

#include <stdint.h>

/* Interpreter heap, including its initialization baseline. */
#define TNY_CODE_MEMORY_BYTES       (64u * 1024u * 1024u)
#define TNY_CODE_OUTPUT_BYTES       (64u * 1024u)
/* A final result: printed output, or an error line plus bounded output. */
#define TNY_CODE_RESULT_TEXT_BYTES  (TNY_CODE_OUTPUT_BYTES + 512u)
#define TNY_CODE_SOURCE_BYTES       (256u * 1024u)
#define TNY_CODE_ARGUMENT_BYTES     (256u * 1024u)
#define TNY_CODE_NAME_BYTES         256u
#define TNY_CODE_TOOL_CALLS         64u
/* One nested result must fit a single private cell frame. */
#define TNY_CODE_TOOL_RESULT_BYTES  (8u * 1024u * 1024u - 16u)
#define TNY_CODE_DEFAULT_TIMEOUT_MS 5000
#define TNY_CODE_MAX_TIMEOUT_MS     30000

/* Callback retains permission/allowlist/event authority. Return malloc-owned
 * text; the runtime consumes it. NULL signals allocation/execution failure. */
typedef char *(*tny_code_call_fn)(void *userdata, const char *name, const char *arguments_json);

/* Returns malloc-owned output, or an "error: code: ..." result; NULL only on
 * host allocation failure. Catalog and userdata are borrowed for this call.
 * The code runs in a fresh `--code-cell` child of the current executable with
 * an empty environment, no inherited descriptors except one private socket,
 * and an OS sandbox applied before the untrusted source is read. This caller
 * re-checks every nested call and owns the deadline: the child is killed when
 * it passes, whatever the child is doing. Unsupported hosts return an error. */
char *tny_code_run(const char *code, int timeout_ms, const char *catalog_json,
                   tny_code_call_fn call, void *userdata);

/* Trusted host variant: deadline is an absolute monotonic millisecond deadline
 * borrowed for this synchronous call. Only the host may extend it to exclude
 * human prompt waits; it is re-read after every callback. Call/output limits
 * remain cumulative. Updates must occur on this thread, never concurrently. */
char *tny_code_run_with_deadline(const char *code, const int64_t *deadline,
                                 const char *catalog_json, tny_code_call_fn call, void *userdata);

/* Private `--code-cell` entry point: fd 3 is the parent's socket. */
int tny_code_cell_main(void);

#endif
