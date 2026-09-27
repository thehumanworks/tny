/* code_python.h — the embedded CPython interpreter used inside one confined
 * code cell (docs/adr/0179). Only the cell child process calls this in
 * production; it is not an isolation boundary by itself (the OS sandbox and
 * the parent's authority checks are). Native builds link the pinned static
 * CPython; library/wasm builds link code_python_unsupported.c instead. */
#ifndef TNY_CODE_PYTHON_H
#define TNY_CODE_PYTHON_H

#include "core/code_runtime.h"
#include <stdbool.h>

typedef struct {
    const char *catalog;   /* borrowed JSON array of permitted tool schemas */
    tny_code_call_fn call; /* NULL result: terminal failure, see failure() */
    void *userdata;
    /* Optional reason for the last NULL call result (borrowed text). */
    const char *(*failure)(void *userdata);
    /* Production terminal handler: no allocation and no return to Python.
     * It closes execution authority before a limit can be caught or bypassed
     * through an alias of the inherited IPC descriptor. Optional test hosts
     * may omit it; they are not the production process-isolation boundary. */
    void (*abort)(void *userdata, const char *reason);
} tny_code_python_host;

/* False when this build has no embedded interpreter. */
bool tny_code_python_available(void);
/* Initialize once per process with the heap budget in force. 0 on success. */
int tny_code_python_init(void);
/* Run one cell in fresh globals. Returns malloc-owned output, "error: code:
 * ..." text, or NULL only on host allocation failure. */
char *tny_code_python_run(const char *code, const tny_code_python_host *host);
/* Bytes currently charged to the interpreter heap budget (diagnostics). */
unsigned long long tny_code_python_heap_bytes(void);
void tny_code_python_fini(void);

#endif
