/* code_python.h — the user's own python3 for code cells and tool extensions
 * (docs/adr/0222). tny embeds no interpreter: cells and the tool host run on
 * the python3 the user would run, with their environment and packages.
 * Native builds link code_python.c; library/wasm builds link
 * code_python_unsupported.c, which never finds an interpreter. */
#ifndef TNY_CODE_PYTHON_H
#define TNY_CODE_PYTHON_H

#include <stdbool.h>

/* Oldest Python a code cell accepts; older interpreters count as absent. */
#define TNY_PYTHON_MIN_MAJOR 3
#define TNY_PYTHON_MIN_MINOR 8

/* Absolute path of a usable python3, or NULL. TNY_PYTHON names one (a path,
 * or a command looked up on PATH); TNY_PYTHON=off (or 0, none, false) means
 * none. Otherwise the first python3 on PATH, as a shell would run it: a
 * virtual environment's interpreter is kept as is, never resolved to its base.
 * The answer is probed once (it runs the interpreter) and cached until
 * TNY_PYTHON or PATH changes. The returned string is owned by the cache. */
const char *tny_python_path(void);
/* False for builds that link code_python_unsupported.c (libtny, wasm, MSYS2):
 * they keep run_code with its explicit refusal instead of direct tools. */
bool tny_code_python_build(void);
/* True when code cells can run here, so run_code is the model-facing tool. */
bool tny_code_python_available(void);

/* `python3 -u -X utf8 -c LOADER`: reads one length-prefixed source frame
 * from fd 3 and executes it (the cell bootstrap, python/tny/_cell.py). */
extern const char TNY_PYTHON_CELL_LOADER[];
/* Source of python/tny/<module>.py embedded in the binary, or NULL. */
const char *tny_python_module_source(const char *module);

#endif
