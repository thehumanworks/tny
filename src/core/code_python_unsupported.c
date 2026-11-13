/* Builds that cannot start code cells (libtny, wasm, MSYS2/Cygwin): no
 * interpreter is ever found. run_code stays the model-facing tool and is
 * refused explicitly before reaching this seam (ADR 0174, ADR 0222). */
#include "core/code_python.h"
#include <stddef.h>

const char TNY_PYTHON_CELL_LOADER[] = "";

const char *tny_python_path(void) { return NULL; }

bool tny_code_python_build(void) { return false; }

bool tny_code_python_available(void) { return false; }
