/* CPython isolated initialization is NOT a sandbox. The runner requires bwrap. */
#include <Python.h>
#include "bench.h"
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
    (void)self; (void)args;
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
    Py_ssize_t count = PyTuple_Size(args);
    for (Py_ssize_t i = 0; i < count; ++i) {
        PyObject *value = PyObject_Str(PyTuple_GetItem(args, i));
        if (!value) return NULL;
        Py_ssize_t len = 0;
        const char *str = PyUnicode_AsUTF8AndSize(value, &len);
        bool ok = str && (!i || bench_append(active, "\t", 1)) &&
                  bench_append(active, str, (size_t)len);
        Py_DECREF(value);
        if (!ok) { PyErr_SetString(PyExc_RuntimeError, "output limit exceeded"); return NULL; }
    }
    if (!bench_append(active, "\n", 1)) {
        PyErr_SetString(PyExc_RuntimeError, "output limit exceeded"); return NULL;
    }
    Py_RETURN_NONE;
}
static PyMethodDef methods[] = {
    {"call", call, METH_VARARGS, NULL}, {"list", list, METH_NOARGS, NULL},
    {"describe", describe, METH_VARARGS, NULL}, {"output", output, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}
};
static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT, "_tnybench", NULL, -1, methods, NULL, NULL, NULL, NULL
};
static PyObject *init_module(void) { return PyModule_Create(&module); }
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
    const char *setup =
        "import json, types, builtins, _tnybench\n"
        "tools = types.SimpleNamespace(call=_tnybench.call, list=_tnybench.list, describe=_tnybench.describe)\n"
        "names = 'abs all any bool dict enumerate float int isinstance len list map max min next range reversed round set sorted str sum tuple zip Exception ValueError TypeError'.split()\n"
        "limited = {name: getattr(builtins, name) for name in names}\n"
        "scope = {'__builtins__': limited, 'tools': tools, 'json': json, 'print': _tnybench.output}\n";
    PyObject *ready = scope ? PyRun_String(setup, Py_file_input, scope, scope) : NULL;
    PyObject *globals = ready ? PyDict_GetItemString(scope, "scope") : NULL;
    PyObject *result = globals ? PyRun_String(code, Py_file_input, globals, globals) : NULL;
    bool ok = result != NULL;
    if (!ok) {
        PyObject *error = PyErr_GetRaisedException();
        PyObject *message = error ? PyObject_Str(error) : NULL;
        const char *str = message ? PyUnicode_AsUTF8(message) : NULL;
        if (str) {
            (void)bench_append(s, "error: ", 7);
            (void)bench_append(s, str, strlen(str));
        }
        Py_XDECREF(message);
        Py_XDECREF(error);
    }
    Py_XDECREF(result);
    Py_XDECREF(ready);
    Py_XDECREF(scope);
    if (Py_FinalizeEx() < 0) ok = false;
    active = NULL;
    return ok;
}
