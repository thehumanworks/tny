/* Builds without the embedded interpreter (libtny, wasm, MSYS2/Cygwin): code
 * cells fail explicitly. Library-hosted and wasm tool execution are already
 * refused before reaching this seam (ADR 0174); this keeps the refusal
 * explicit if a caller does reach it. */
#include "core/code_python.h"
#include <stdlib.h>
#include <string.h>

bool tny_code_python_available(void) { return false; }

int tny_code_python_init(void) { return -1; }

char *tny_code_python_run(const char *code, const tny_code_python_host *host) {
    (void)code;
    (void)host;
    static const char text[] = "error: code: Python code cells are unavailable in this build";
    char *out = malloc(sizeof text);
    return out ? memcpy(out, text, sizeof text) : NULL;
}

unsigned long long tny_code_python_heap_bytes(void) { return 0; }

void tny_code_python_fini(void) {}
