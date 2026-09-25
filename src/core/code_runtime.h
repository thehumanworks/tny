/* code_runtime.h — bounded, fresh Lua state over an explicit tool callback. */
#ifndef TNY_CODE_RUNTIME_H
#define TNY_CODE_RUNTIME_H

#include <stdint.h>

#define TNY_CODE_MEMORY_BYTES       (16u * 1024u * 1024u)
#define TNY_CODE_OUTPUT_BYTES       (64u * 1024u)
#define TNY_CODE_SOURCE_BYTES       (256u * 1024u)
#define TNY_CODE_TOOL_CALLS         64u
#define TNY_CODE_INSTRUCTIONS       10000000u
#define TNY_CODE_DEFAULT_TIMEOUT_MS 5000
#define TNY_CODE_MAX_TIMEOUT_MS     30000

/* Callback retains permission/allowlist/event authority. Return malloc-owned
 * text; the runtime consumes it. NULL signals allocation/execution failure. */
typedef char *(*tny_code_call_fn)(void *userdata, const char *name, const char *arguments_json);

/* Returns malloc-owned output, or an "error: code: ..." result; NULL only on
 * host allocation failure. Catalog and userdata are borrowed for this call.
 * The host MUST additionally enforce a process deadline: Lua hooks bound VM
 * instructions, but cannot interrupt a C library operation or callback. */
char *tny_code_run(const char *code, int timeout_ms, const char *catalog_json,
                   tny_code_call_fn call, void *userdata);

/* Trusted host variant: deadline is an absolute monotonic millisecond deadline
 * borrowed for this synchronous call. Only the host may extend it to exclude
 * human prompt waits. Instruction/call/allocation limits remain cumulative.
 * Caller updates must occur on this execution thread, never concurrently. */
char *tny_code_run_with_deadline(const char *code, const int64_t *deadline,
                                 const char *catalog_json, tny_code_call_fn call, void *userdata);

#endif
