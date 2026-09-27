/* Benchmark-only stock-CPython adapter mirroring the PROPOSED production policy:
 * every builtin except the import/code-loading/interactive ones, stdlib json as
 * a stand-in for the production native facade, print(sep=, end=), and
 * "error: Type: message (line N)" results. NOT a sandbox: runs under bwrap. */
#include <Python.h>
#include "bench.h"
#include <frameobject.h>
#include <stdlib.h>
#include <string.h>
static bench_state *active;
static PyObject *call(PyObject *self, PyObject *args) {
    (void)self;
    const char *name, *arguments;
    if (!PyArg_ParseTuple(args, "ss", &name, &arguments)) return NULL;
    char *result = bench_call(active, name, arguments);
    if (!result) return PyErr_NoMemory();
    PyObject *out = PyUnicode_FromString(result);
    free(result);
    return out;
}
static PyObject *list(PyObject *self, PyObject *args) {
    (void)self;
    (void)args;
    return PyUnicode_FromString(active->catalog);
}
static PyObject *describe(PyObject *self, PyObject *args) {
    (void)self;
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return NULL;
    char *result = bench_describe(active, name);
    if (!result) Py_RETURN_NONE;
    PyObject *out = PyUnicode_FromString(result);
    free(result);
    return out;
}
static PyObject *output(PyObject *self, PyObject *args) {
    (void)self;
    const char *text;
    Py_ssize_t len;
    if (!PyArg_ParseTuple(args, "s#", &text, &len)) return NULL;
    if (!bench_append(active, text, (size_t)len)) {
        PyErr_SetString(PyExc_RuntimeError, "output limit exceeded");
        return NULL;
    }
    Py_RETURN_NONE;
}
static PyMethodDef methods[] = {{"call", call, METH_VARARGS, NULL},
                                {"list", list, METH_NOARGS, NULL},
                                {"describe", describe, METH_VARARGS, NULL},
                                {"output", output, METH_VARARGS, NULL},
                                {NULL, NULL, 0, NULL}};
static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT, "_tnybench", NULL, -1, methods, NULL, NULL, NULL, NULL};
static PyObject *init_module(void) { return PyModule_Create(&module); }
static const char SETUP[] =
    "import json, types, builtins, _tnybench\n"
    "tools = types.SimpleNamespace(call=_tnybench.call, list=_tnybench.list, "
    "describe=_tnybench.describe)\n"
    "removed = {'__import__', 'open', 'eval', 'exec', 'compile', 'input', 'breakpoint', "
    "'help', 'exit', 'quit', 'copyright', 'credits', 'license', '__loader__', '__spec__'}\n"
    "limited = {k: v for k, v in vars(builtins).items() if k not in removed}\n"
    "def _print(*args, sep=' ', end='\\n', file=None, flush=False):\n"
    "    if file is not None:\n"
    "        raise TypeError('print(file=...) is not available')\n"
    "    _tnybench.output((' ' if sep is None else sep).join(map(str, args)) + "
    "('\\n' if end is None else end))\n"
    "limited['print'] = _print\n"
    "scope = {'__builtins__': limited, '__name__': '__main__', 'tools': tools, 'json': json}\n";
bool bench_execute(bench_state *s, const char *code) {
    active = s;
    if (PyImport_AppendInittab("_tnybench", init_module) == -1) return false;
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.site_import = 0;
    config.write_bytecode = 0;
    config.install_signal_handlers = 0;
    PyStatus status = PyConfig_SetBytesString(&config, &config.home, PYHOME);
    if (!PyStatus_Exception(status)) status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) return false;
    PyObject *scope = PyDict_New();
    PyObject *ready = scope ? PyRun_String(SETUP, Py_file_input, scope, scope) : NULL;
    PyObject *globals = ready ? PyDict_GetItemString(scope, "scope") : NULL;
    PyObject *compiled = globals ? Py_CompileString(code, "<code>", Py_file_input) : NULL;
    PyObject *result = compiled ? PyEval_EvalCode(compiled, globals, globals) : NULL;
    bool ok = result != NULL;
    if (!ok) {
        PyObject *error = PyErr_GetRaisedException();
        PyObject *message = error ? PyObject_Str(error) : NULL;
        const char *type = error ? Py_TYPE(error)->tp_name : "Error";
        const char *str = message ? PyUnicode_AsUTF8(message) : NULL;
        int line = -1;
        PyObject *tb = error ? PyException_GetTraceback(error) : NULL;
        for (PyTracebackObject *t = (PyTracebackObject *)tb; t; t = t->tb_next) line = t->tb_lineno;
        Py_XDECREF(tb);
        if (line < 0 && error && PyErr_GivenExceptionMatches(error, PyExc_SyntaxError)) {
            PyObject *lineno = PyObject_GetAttrString(error, "lineno");
            line = lineno ? (int)PyLong_AsLong(lineno) : -1;
            Py_XDECREF(lineno);
            PyErr_Clear();
        }
        char buffer[64];
        (void)bench_append(s, "error: ", 7);
        (void)bench_append(s, type, strlen(type));
        if (str && *str) {
            (void)bench_append(s, ": ", 2);
            (void)bench_append(s, str, strlen(str));
        }
        if (line > 0) {
            int n = snprintf(buffer, sizeof buffer, " (line %d)", line);
            (void)bench_append(s, buffer, (size_t)n);
        }
        Py_XDECREF(message);
        Py_XDECREF(error);
    }
    Py_XDECREF(result);
    Py_XDECREF(compiled);
    Py_XDECREF(ready);
    Py_XDECREF(scope);
    if (Py_FinalizeEx() < 0) ok = false;
    active = NULL;
    return ok;
}
