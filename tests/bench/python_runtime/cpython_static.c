/* Benchmark-only SIZE/STARTUP probe: static libpython 3.14.7, bootstrap modules
 * only, encodings frozen into the executable, no stdlib directory. It has no
 * json module (production supplies a native facade), so it cannot run the
 * corpus; it measures the self-contained interpreter footprint and init cost. */
#include <Python.h>
#include "bench.h"
#include <string.h>
#include "frozen_encodings.h"
#include "frozen_encodings_aliases.h"
#include "frozen_encodings_utf_8.h"
static const struct _frozen frozen[] = {
    {"encodings", _Py_M__encodings, (int)sizeof(_Py_M__encodings), 1},
    {"encodings.aliases", _Py_M__encodings_aliases, (int)sizeof(_Py_M__encodings_aliases), 0},
    {"encodings.utf_8", _Py_M__encodings_utf_8, (int)sizeof(_Py_M__encodings_utf_8), 0},
    {0, 0, 0, 0},
};
bool bench_execute(bench_state *s, const char *code) {
    PyImport_FrozenModules = frozen;
    PyPreConfig pre;
    PyPreConfig_InitIsolatedConfig(&pre);
    pre.utf8_mode = 1;
    if (PyStatus_Exception(Py_PreInitialize(&pre))) return false;
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.site_import = 0;
    config.write_bytecode = 0;
    config.install_signal_handlers = 0;
    config.module_search_paths_set = 1;
    config.pathconfig_warnings = 0;
    PyStatus st = PyConfig_SetString(&config, &config.home, L"/nonexistent-tny-python-home");
    if (!PyStatus_Exception(st)) st = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(st)) return false;
    PyObject *globals = PyDict_New();
    bool ok = globals && PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) == 0;
    PyObject *result = ok ? PyRun_String(code, Py_file_input, globals, globals) : NULL;
    ok = result != NULL;
    if (!ok) {
        PyErr_Clear();
        (void)bench_append(s, "error", 5);
    }
    Py_XDECREF(result);
    Py_XDECREF(globals);
    if (Py_FinalizeEx() < 0) ok = false;
    return ok;
}
