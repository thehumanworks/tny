/* Embedded CPython 3.14 for one confined code cell (docs/adr/0179).
 *
 * This file shapes the Python environment; it is NOT the security boundary.
 * Object introspection can reach modules that initialization loads (posix,
 * _io), so containment comes from the cell process's OS sandbox and from the
 * parent re-checking every nested call. Budgets here are sticky: once a heap,
 * output, call or callback limit trips, the cell is terminal — every later
 * tools/print use raises again, a pending call re-raises at the next bytecode
 * check, and the final result is the limit error whatever the code caught. */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "core/code_python.h"
#include "core/code_policy.h"
#include "yyjson.h"
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "tny_frozen_encodings.h"
#include "tny_frozen_encodings_aliases.h"
#include "tny_frozen_encodings_utf_8.h"

/* The interpreter decodes its filesystem/stdio encodings at startup; only
 * these pure-Python codec modules are needed and they are frozen in. */
static const struct _frozen frozen_modules[] = {
    {"encodings", _Py_M__encodings, (int)sizeof(_Py_M__encodings), 1},
    {"encodings.aliases", _Py_M__encodings_aliases, (int)sizeof(_Py_M__encodings_aliases), 0},
    {"encodings.utf_8", _Py_M__encodings_utf_8, (int)sizeof(_Py_M__encodings_utf_8), 0},
    {NULL, NULL, 0, 0},
};

/* Heap accounting ------------------------------------------------------- */

typedef union {
    max_align_t alignment;
    size_t size;
} allocation;

/* After the budget trips, unwinding (exception objects, tracebacks, frame
 * teardown) may still allocate from this reserve; nothing may use it to
 * continue work, because the cell is already terminal. */
#define HEAP_UNWIND_RESERVE (1024u * 1024u)

static struct {
    _Atomic unsigned long long used;
    _Atomic bool exhausted;
} heap;

static void budget_exhausted(void);

static bool heap_charge(size_t previous, size_t size) {
    unsigned long long used = atomic_load(&heap.used);
    for (;;) {
        unsigned long long base = used - previous;
        bool within = tny_code_memory_admit(base, size, sizeof(allocation), TNY_CODE_MEMORY_BYTES);
        if (!within) {
            if (!atomic_exchange(&heap.exhausted, true)) budget_exhausted();
            if (!tny_code_memory_admit(base, size, sizeof(allocation),
                                       TNY_CODE_MEMORY_BYTES + HEAP_UNWIND_RESERVE))
                return false;
        }
        unsigned long long next = base + sizeof(allocation) + size;
        if (atomic_compare_exchange_weak(&heap.used, &used, next)) return true;
    }
}

static void *heap_malloc(void *ctx, size_t size) {
    (void)ctx;
    if (!heap_charge(0, size)) return NULL;
    allocation *block = malloc(sizeof(*block) + size);
    if (!block) {
        atomic_fetch_sub(&heap.used, sizeof(*block) + size);
        return NULL;
    }
    block->size = size;
    return block + 1;
}

static void *heap_calloc(void *ctx, size_t count, size_t each) {
    if (each && count > SIZE_MAX / each) return NULL;
    void *p = heap_malloc(ctx, count * each);
    if (p) memset(p, 0, count * each);
    return p;
}

static void heap_free(void *ctx, void *ptr) {
    (void)ctx;
    if (!ptr) return;
    allocation *block = (allocation *)ptr - 1;
    atomic_fetch_sub(&heap.used, sizeof(*block) + block->size);
    free(block);
}

static void *heap_realloc(void *ctx, void *ptr, size_t size) {
    if (!ptr) return heap_malloc(ctx, size);
    allocation *block = (allocation *)ptr - 1;
    size_t previous = sizeof(*block) + block->size;
    if (!heap_charge(previous, size)) return NULL;
    allocation *next = realloc(block, sizeof(*next) + size);
    if (!next) {
        /* The old block remains valid and still charged at its old size. */
        atomic_fetch_add(&heap.used, previous);
        atomic_fetch_sub(&heap.used, sizeof(*next) + size);
        return NULL;
    }
    next->size = size;
    return next + 1;
}

static void *json_alloc(void *ctx, size_t size) { return heap_malloc(ctx, size); }
static void *json_realloc(void *ctx, void *ptr, size_t old, size_t size) {
    (void)old;
    return heap_realloc(ctx, ptr, size);
}
static void json_free(void *ctx, void *ptr) { heap_free(ctx, ptr); }
static const yyjson_alc heap_json = {json_alloc, json_realloc, json_free, NULL};

unsigned long long tny_code_python_heap_bytes(void) { return atomic_load(&heap.used); }

/* Cell state ------------------------------------------------------------ */

static struct {
    bool initialized;
    bool active;   /* code is executing */
    bool finished; /* code returned: no more tools/print, even from __del__ */
    const tny_code_python_host *host;
    const char *fatal; /* sticky terminal limit, static text */
    unsigned calls;
    char *output;
    size_t output_len;
    PyObject *limit_error; /* BaseException subclass: not caught by `except Exception` */
    PyObject *decode_error;
} cell;

static int fatal_pending(void *arg) {
    (void)arg;
    if (!cell.active || !cell.fatal) return 0;
    PyErr_SetString(cell.limit_error, cell.fatal);
    (void)Py_AddPendingCall(fatal_pending, NULL);
    return -1;
}

/* Mark the cell terminal and raise; returns NULL for the caller to return. */
static PyObject *terminal(const char *reason) {
    if (!cell.fatal) {
        cell.fatal = reason;
        (void)Py_AddPendingCall(fatal_pending, NULL);
    }
    if (!PyErr_Occurred()) PyErr_SetString(cell.limit_error, cell.fatal);
    return NULL;
}

static void budget_exhausted(void) {
    if (!cell.fatal) cell.fatal = "memory limit exceeded";
    /* Thread-safe and allocation-free; re-raises until the cell unwinds. */
    if (cell.active) (void)Py_AddPendingCall(fatal_pending, NULL);
}

static bool refuse_after_limit(void) {
    if (cell.fatal) {
        PyErr_SetString(cell.limit_error, cell.fatal);
        return true;
    }
    if (cell.finished || !cell.active) {
        PyErr_SetString(PyExc_RuntimeError, "the code cell has finished; tools are closed");
        return true;
    }
    return false;
}

/* print ------------------------------------------------------------------ */

static bool append_output(const char *text, size_t len) {
    if (!tny_code_output_admit(cell.output_len, len)) {
        terminal("output limit exceeded");
        return false;
    }
    /* Results cross C-string frames; a NUL cannot be represented. */
    if (memchr(text, 0, len)) {
        terminal("output contains a NUL byte");
        return false;
    }
    memcpy(cell.output + cell.output_len, text, len);
    cell.output_len += len;
    cell.output[cell.output_len] = 0;
    return true;
}

static bool append_str(PyObject *value) {
    Py_ssize_t len = 0;
    const char *text = PyUnicode_AsUTF8AndSize(value, &len);
    return text && append_output(text, (size_t)len);
}

static PyObject *code_print(PyObject *self, PyObject *args, PyObject *kwargs) {
    (void)self;
    static const char *const names[] = {"sep", "end", "file", "flush", NULL};
    PyObject *sep = Py_None, *end = Py_None, *file = Py_None;
    int flush = 0;
    PyObject *empty = PyTuple_New(0);
    if (!empty) return NULL;
    int parsed = PyArg_ParseTupleAndKeywords(empty, kwargs, "|$OOOp:print", (char *const *)names,
                                             &sep, &end, &file, &flush);
    Py_DECREF(empty);
    if (!parsed) return NULL;
    (void)flush;
    if (file != Py_None) {
        PyErr_SetString(PyExc_TypeError, "print(file=...) is unavailable; output goes to the "
                                         "cell result");
        return NULL;
    }
    if ((sep != Py_None && !PyUnicode_Check(sep)) || (end != Py_None && !PyUnicode_Check(end))) {
        PyErr_SetString(PyExc_TypeError, "sep and end must be None or a string");
        return NULL;
    }
    if (refuse_after_limit()) return NULL;
    Py_ssize_t count = PyTuple_GET_SIZE(args);
    for (Py_ssize_t i = 0; i < count; ++i) {
        if (i && (sep == Py_None ? !append_output(" ", 1) : !append_str(sep))) return NULL;
        PyObject *text = PyObject_Str(PyTuple_GET_ITEM(args, i));
        bool ok = text && append_str(text);
        Py_XDECREF(text);
        if (!ok) return NULL;
    }
    if (end == Py_None ? !append_output("\n", 1) : !append_str(end)) return NULL;
    Py_RETURN_NONE;
}

/* tools ------------------------------------------------------------------ */

static bool json_object_text(const char *text, size_t len) {
    yyjson_doc *doc = yyjson_read_opts((char *)text, len, 0, &heap_json, NULL);
    bool object = doc && yyjson_is_obj(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    return object;
}

static PyObject *tools_call(PyObject *self, PyObject *const *args, Py_ssize_t nargs) {
    (void)self;
    if (nargs != 2 || !PyUnicode_Check(args[0]) || !PyUnicode_Check(args[1])) {
        PyErr_SetString(PyExc_TypeError,
                        "tools.call(name, arguments_json) takes two strings: a tool name and "
                        "JSON object text such as json.dumps({...})");
        return NULL;
    }
    if (refuse_after_limit()) return NULL;
    Py_ssize_t name_len = 0, args_len = 0;
    const char *name = PyUnicode_AsUTF8AndSize(args[0], &name_len);
    const char *arguments = name ? PyUnicode_AsUTF8AndSize(args[1], &args_len) : NULL;
    if (!arguments) return NULL;
    if (memchr(name, 0, (size_t)name_len) || memchr(arguments, 0, (size_t)args_len)) {
        PyErr_SetString(PyExc_ValueError, "tool name and arguments cannot contain NUL");
        return NULL;
    }
    bool recursive = strcmp(name, "run_code") == 0;
    if (!name_len || name_len > TNY_CODE_NAME_BYTES || memchr(name, '\n', (size_t)name_len)) {
        PyErr_SetString(PyExc_ValueError, "invalid tool name");
        return NULL;
    }
    if (recursive) {
        PyErr_SetString(PyExc_ValueError, "recursive run_code is forbidden");
        return NULL;
    }
    if ((size_t)args_len > TNY_CODE_ARGUMENT_BYTES) {
        PyErr_SetString(PyExc_ValueError, "tool arguments limit exceeded");
        return NULL;
    }
    bool object = json_object_text(arguments, (size_t)args_len);
    if (cell.fatal) return terminal(cell.fatal);
    if (!object) {
        PyErr_SetString(PyExc_ValueError, "tool arguments must be JSON object text");
        return NULL;
    }
    if (!tny_code_call_admit(cell.calls, (uint64_t)name_len, recursive, (uint64_t)args_len, object))
        return terminal("tool call limit exceeded");
    ++cell.calls;
    const tny_code_python_host *host = cell.host;
    char *result = host && host->call ? host->call(host->userdata, name, arguments) : NULL;
    if (!result) {
        const char *reason = host && host->failure ? host->failure(host->userdata) : NULL;
        return terminal(reason ? reason : "tool callback failed");
    }
    size_t len = strlen(result);
    if (!tny_code_result_admit(len)) {
        free(result);
        return terminal("tool result limit exceeded");
    }
    PyObject *text = PyUnicode_DecodeUTF8(result, (Py_ssize_t)len, "replace");
    free(result);
    return text;
}

static PyObject *tools_list(PyObject *self, PyObject *unused) {
    (void)self;
    (void)unused;
    if (refuse_after_limit()) return NULL;
    return PyUnicode_FromString(cell.host && cell.host->catalog ? cell.host->catalog : "[]");
}

static PyObject *tools_describe(PyObject *self, PyObject *name_obj) {
    (void)self;
    if (!PyUnicode_Check(name_obj)) {
        PyErr_SetString(PyExc_TypeError, "tools.describe(name) takes a tool name string");
        return NULL;
    }
    if (refuse_after_limit()) return NULL;
    const char *name = PyUnicode_AsUTF8(name_obj);
    if (!name) return NULL;
    const char *catalog = cell.host && cell.host->catalog ? cell.host->catalog : "[]";
    yyjson_doc *doc = yyjson_read_opts((char *)catalog, strlen(catalog), 0, &heap_json, NULL);
    if (!doc) return cell.fatal ? terminal(cell.fatal) : PyErr_NoMemory();
    yyjson_val *entry;
    size_t index, count;
    PyObject *found = NULL;
    yyjson_arr_foreach(yyjson_doc_get_root(doc), index, count, entry) {
        yyjson_val *fn = yyjson_obj_get(entry, "function");
        const char *candidate = yyjson_get_str(yyjson_obj_get(fn ? fn : entry, "name"));
        if (!candidate || strcmp(candidate, name) != 0) continue;
        size_t len = 0;
        char *text = yyjson_val_write_opts(entry, 0, &heap_json, &len, NULL);
        found = text ? PyUnicode_DecodeUTF8(text, (Py_ssize_t)len, "strict") : PyErr_NoMemory();
        if (text) json_free(NULL, text);
        break;
    }
    yyjson_doc_free(doc);
    if (found || PyErr_Occurred()) return found;
    Py_RETURN_NONE;
}

static PyMethodDef tools_methods[] = {
    {"call", (PyCFunction)(void (*)(void))tools_call, METH_FASTCALL,
     "call(name, arguments_json) -> str: run a permitted nested tool"},
    {"list", tools_list, METH_NOARGS, "list() -> str: JSON catalog of permitted tools"},
    {"describe", tools_describe, METH_O, "describe(name) -> str | None: one tool's JSON schema"},
    {NULL, NULL, 0, NULL},
};

/* json.loads ------------------------------------------------------------- */

static PyObject *decode_value(yyjson_val *v) {
    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_NULL: Py_RETURN_NONE;
    case YYJSON_TYPE_BOOL: return PyBool_FromLong(yyjson_get_bool(v));
    case YYJSON_TYPE_RAW: {
        /* Big numbers stay exact: integers of any size (subject to the
         * interpreter's int_max_str_digits guard) and non-finite doubles. */
        const char *raw = yyjson_get_raw(v);
        if (strpbrk(raw, ".eE")) {
            PyObject *text = PyUnicode_FromString(raw);
            PyObject *value = text ? PyFloat_FromString(text) : NULL;
            Py_XDECREF(text);
            return value;
        }
        return PyLong_FromString(raw, NULL, 10);
    }
    case YYJSON_TYPE_NUM:
        if (yyjson_is_sint(v)) return PyLong_FromLongLong(yyjson_get_sint(v));
        if (yyjson_is_uint(v)) return PyLong_FromUnsignedLongLong(yyjson_get_uint(v));
        return PyFloat_FromDouble(yyjson_get_real(v));
    case YYJSON_TYPE_STR:
        return PyUnicode_DecodeUTF8(yyjson_get_str(v), (Py_ssize_t)yyjson_get_len(v), "strict");
    case YYJSON_TYPE_ARR: {
        if (Py_EnterRecursiveCall(" while decoding a JSON array")) return NULL;
        PyObject *list = PyList_New((Py_ssize_t)yyjson_arr_size(v));
        size_t index, count;
        yyjson_val *item;
        yyjson_arr_foreach(v, index, count, item) {
            PyObject *value = list ? decode_value(item) : NULL;
            if (!value) {
                Py_CLEAR(list);
                break;
            }
            PyList_SET_ITEM(list, (Py_ssize_t)index, value);
        }
        Py_LeaveRecursiveCall();
        return list;
    }
    case YYJSON_TYPE_OBJ: {
        if (Py_EnterRecursiveCall(" while decoding a JSON object")) return NULL;
        PyObject *dict = PyDict_New();
        size_t index, count;
        yyjson_val *key, *item;
        yyjson_obj_foreach(v, index, count, key, item) {
            PyObject *k = dict ? decode_value(key) : NULL;
            PyObject *value = k ? decode_value(item) : NULL;
            /* A repeated key keeps its first position and last value. */
            if (!value || PyDict_SetItem(dict, k, value) < 0) Py_CLEAR(dict);
            Py_XDECREF(k);
            Py_XDECREF(value);
            if (!dict) break;
        }
        Py_LeaveRecursiveCall();
        return dict;
    }
    default: PyErr_SetString(PyExc_ValueError, "unsupported JSON value"); return NULL;
    }
}

static PyObject *decode_error(const char *message, const char *text, size_t byte_pos) {
    Py_ssize_t pos = 0, line = 1, column = 1;
    for (size_t i = 0; i < byte_pos && text[i]; ++i) {
        if (((unsigned char)text[i] & 0xC0) == 0x80) continue;
        ++pos;
        if (text[i] == '\n') {
            ++line;
            column = 1;
        } else ++column;
    }
    PyObject *error = PyObject_CallFunction(cell.decode_error, "s", message);
    if (error) {
        PyObject *formatted =
            PyUnicode_FromFormat("%s: line %zd column %zd (char %zd)", message, line, column, pos);
        PyObject *args = formatted ? PyTuple_Pack(1, formatted) : NULL;
        bool ok = args && PyObject_SetAttrString(error, "args", args) == 0 &&
                  PyObject_SetAttrString(error, "msg", PyTuple_GET_ITEM(args, 0)) == 0;
        PyObject *numbers[] = {PyLong_FromSsize_t(pos), PyLong_FromSsize_t(line),
                               PyLong_FromSsize_t(column)};
        const char *names[] = {"pos", "lineno", "colno"};
        for (int i = 0; i < 3; ++i) {
            ok = ok && numbers[i] && PyObject_SetAttrString(error, names[i], numbers[i]) == 0;
            Py_XDECREF(numbers[i]);
        }
        Py_XDECREF(args);
        Py_XDECREF(formatted);
        if (ok) PyErr_SetObject(cell.decode_error, error);
        Py_DECREF(error);
    }
    return NULL;
}

static PyObject *json_loads(PyObject *self, PyObject *args, PyObject *kwargs) {
    (void)self;
    static const char *const names[] = {"s", NULL};
    PyObject *source = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:loads", (char *const *)names, &source))
        return NULL;
    PyObject *owned = NULL;
    if (PyBytes_Check(source) || PyByteArray_Check(source)) {
        owned = PyUnicode_FromEncodedObject(source, "utf-8", "strict");
        if (!owned) return NULL;
        source = owned;
    } else if (!PyUnicode_Check(source)) {
        PyErr_Format(PyExc_TypeError, "the JSON object must be str, bytes or bytearray, not %s",
                     Py_TYPE(source)->tp_name);
        return NULL;
    }
    Py_ssize_t len = 0;
    const char *text = PyUnicode_AsUTF8AndSize(source, &len);
    PyObject *result = NULL;
    if (text && len >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0)
        decode_error("Unexpected UTF-8 BOM (decode using utf-8-sig)", text, 0);
    else if (text) {
        yyjson_read_err err = {0};
        yyjson_doc *doc = yyjson_read_opts((char *)text, (size_t)len, YYJSON_READ_BIGNUM_AS_RAW,
                                           &heap_json, &err);
        if (!doc) {
            if (cell.fatal) terminal(cell.fatal);
            else if (err.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION) PyErr_NoMemory();
            else decode_error(err.msg ? err.msg : "Invalid JSON", text, err.pos);
        } else {
            result = decode_value(yyjson_doc_get_root(doc));
            yyjson_doc_free(doc);
        }
    }
    Py_XDECREF(owned);
    return result;
}

/* json.dumps ------------------------------------------------------------- */

typedef struct {
    PyUnicodeWriter *out;
    bool skipkeys, ensure_ascii, check_circular, allow_nan, sort_keys;
    PyObject *indent; /* str or NULL */
    PyObject *item_separator, *key_separator, *fallback, *markers;
    Py_ssize_t level;
} encoder;

static int write_ascii(encoder *e, const char *text) {
    return PyUnicodeWriter_WriteUTF8(e->out, text, (Py_ssize_t)strlen(text));
}

static int write_string(encoder *e, PyObject *s) {
    static const char hex[] = "0123456789abcdef";
    if (PyUnicodeWriter_WriteChar(e->out, '"') < 0) return -1;
    Py_ssize_t len = PyUnicode_GET_LENGTH(s);
    int kind = PyUnicode_KIND(s);
    const void *data = PyUnicode_DATA(s);
    for (Py_ssize_t i = 0; i < len; ++i) {
        Py_UCS4 c = PyUnicode_READ(kind, data, i);
        const char *escape = c == '"'    ? "\\\""
                             : c == '\\' ? "\\\\"
                             : c == '\n' ? "\\n"
                             : c == '\r' ? "\\r"
                             : c == '\t' ? "\\t"
                             : c == '\b' ? "\\b"
                             : c == '\f' ? "\\f"
                                         : NULL;
        int rc;
        if (escape) rc = write_ascii(e, escape);
        else if (c < 0x20 || (e->ensure_ascii && c > 0x7E)) {
            char buf[13];
            Py_UCS4 units[2] = {c, 0};
            int n = 1;
            if (c > 0xFFFF) {
                units[0] = 0xD800 | ((c - 0x10000) >> 10);
                units[1] = 0xDC00 | ((c - 0x10000) & 0x3FF);
                n = 2;
            }
            char *p = buf;
            for (int j = 0; j < n; ++j) {
                *p++ = '\\';
                *p++ = 'u';
                for (int shift = 12; shift >= 0; shift -= 4) *p++ = hex[(units[j] >> shift) & 0xF];
            }
            *p = 0;
            rc = write_ascii(e, buf);
        } else rc = PyUnicodeWriter_WriteChar(e->out, c);
        if (rc < 0) return -1;
    }
    return PyUnicodeWriter_WriteChar(e->out, '"');
}

static int write_float(encoder *e, PyObject *o) {
    double value = PyFloat_AS_DOUBLE(o);
    if (!isfinite(value)) {
        if (!e->allow_nan) {
            PyObject *repr = PyFloat_Type.tp_repr(o);
            if (repr)
                PyErr_Format(PyExc_ValueError,
                             "Out of range float values are not JSON compliant: %U", repr);
            Py_XDECREF(repr);
            return -1;
        }
        return write_ascii(e, isnan(value) ? "NaN" : value > 0 ? "Infinity" : "-Infinity");
    }
    PyObject *repr = PyFloat_Type.tp_repr(o);
    int rc = repr ? PyUnicodeWriter_WriteStr(e->out, repr) : -1;
    Py_XDECREF(repr);
    return rc;
}

static int write_int(encoder *e, PyObject *o) {
    PyObject *repr = PyLong_Type.tp_repr(o);
    int rc = repr ? PyUnicodeWriter_WriteStr(e->out, repr) : -1;
    Py_XDECREF(repr);
    return rc;
}

static int newline_indent(encoder *e) {
    if (!e->indent) return 0;
    if (PyUnicodeWriter_WriteChar(e->out, '\n') < 0) return -1;
    for (Py_ssize_t i = 0; i < e->level; ++i)
        if (PyUnicodeWriter_WriteStr(e->out, e->indent) < 0) return -1;
    return 0;
}

static int encode(encoder *e, PyObject *o);

static int enter_container(encoder *e, PyObject *o, PyObject **marker) {
    *marker = NULL;
    if (!e->check_circular) return 0;
    *marker = PyLong_FromVoidPtr(o);
    if (!*marker) return -1;
    int seen = PySet_Contains(e->markers, *marker);
    if (seen) {
        if (seen > 0) PyErr_SetString(PyExc_ValueError, "Circular reference detected");
        Py_CLEAR(*marker);
        return -1;
    }
    if (PySet_Add(e->markers, *marker) < 0) {
        Py_CLEAR(*marker);
        return -1;
    }
    return 0;
}

static void leave_container(encoder *e, PyObject *marker) {
    if (!marker) return;
    (void)PySet_Discard(e->markers, marker);
    Py_DECREF(marker);
}

static int encode_array(encoder *e, PyObject *o) {
    PyObject *items = PySequence_Fast(o, "expected a sequence");
    if (!items) return -1;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(items);
    if (!n) {
        Py_DECREF(items);
        return write_ascii(e, "[]");
    }
    PyObject *marker;
    int rc = enter_container(e, o, &marker);
    if (!rc) rc = PyUnicodeWriter_WriteChar(e->out, '[');
    ++e->level;
    for (Py_ssize_t i = 0; !rc && i < n; ++i) {
        if (i) rc = PyUnicodeWriter_WriteStr(e->out, e->item_separator);
        if (!rc) rc = newline_indent(e);
        if (!rc) rc = encode(e, PySequence_Fast_GET_ITEM(items, i));
    }
    --e->level;
    if (!rc) rc = newline_indent(e);
    if (!rc) rc = PyUnicodeWriter_WriteChar(e->out, ']');
    leave_container(e, marker);
    Py_DECREF(items);
    return rc;
}

/* Keys: str; float/True/False/None/int are coerced exactly as CPython's json. */
static PyObject *object_key(encoder *e, PyObject *key) {
    if (PyUnicode_Check(key)) return Py_NewRef(key);
    if (PyFloat_Check(key)) {
        PyUnicodeWriter *saved = e->out;
        e->out = PyUnicodeWriter_Create(0);
        if (!e->out) {
            e->out = saved;
            return NULL;
        }
        if (write_float(e, key) < 0) {
            PyUnicodeWriter_Discard(e->out);
            e->out = saved;
            return NULL;
        }
        PyObject *text = PyUnicodeWriter_Finish(e->out);
        e->out = saved;
        return text;
    }
    if (key == Py_True) return PyUnicode_FromString("true");
    if (key == Py_False) return PyUnicode_FromString("false");
    if (key == Py_None) return PyUnicode_FromString("null");
    if (PyLong_Check(key)) return PyLong_Type.tp_repr(key);
    if (e->skipkeys) return Py_NewRef(Py_None); /* sentinel: skip this item */
    PyErr_Format(PyExc_TypeError, "keys must be str, int, float, bool or None, not %s",
                 Py_TYPE(key)->tp_name);
    return NULL;
}

static int encode_object(encoder *e, PyObject *o) {
    if (!PyDict_GET_SIZE(o)) return write_ascii(e, "{}");
    PyObject *items = PyMapping_Items(o);
    if (!items || (e->sort_keys && PyList_Sort(items) < 0)) {
        Py_XDECREF(items);
        return -1;
    }
    PyObject *marker;
    int rc = enter_container(e, o, &marker);
    if (!rc) rc = PyUnicodeWriter_WriteChar(e->out, '{');
    ++e->level;
    bool first = true;
    for (Py_ssize_t i = 0; !rc && i < PyList_GET_SIZE(items); ++i) {
        PyObject *pair = PyList_GET_ITEM(items, i);
        PyObject *key = object_key(e, PyTuple_GET_ITEM(pair, 0));
        if (!key) {
            rc = -1;
            break;
        }
        if (key == Py_None) {
            Py_DECREF(key);
            continue;
        }
        if (!first) rc = PyUnicodeWriter_WriteStr(e->out, e->item_separator);
        first = false;
        if (!rc) rc = newline_indent(e);
        if (!rc) rc = write_string(e, key);
        if (!rc) rc = PyUnicodeWriter_WriteStr(e->out, e->key_separator);
        if (!rc) rc = encode(e, PyTuple_GET_ITEM(pair, 1));
        Py_DECREF(key);
    }
    --e->level;
    if (!rc && !first) rc = newline_indent(e);
    if (!rc) rc = PyUnicodeWriter_WriteChar(e->out, '}');
    leave_container(e, marker);
    Py_DECREF(items);
    return rc;
}

static int encode(encoder *e, PyObject *o) {
    int kind = tny_code_json_kind(o == Py_None, PyBool_Check(o), PyLong_Check(o), PyFloat_Check(o),
                                  PyUnicode_Check(o), PyList_Check(o) || PyTuple_Check(o),
                                  PyDict_Check(o));
    if (Py_EnterRecursiveCall(" while encoding a JSON object")) return -1;
    int rc;
    switch (kind) {
    case TNY_CODE_JSON_NULL: rc = write_ascii(e, "null"); break;
    case TNY_CODE_JSON_BOOL: rc = write_ascii(e, o == Py_True ? "true" : "false"); break;
    case TNY_CODE_JSON_INT: rc = write_int(e, o); break;
    case TNY_CODE_JSON_FLOAT: rc = write_float(e, o); break;
    case TNY_CODE_JSON_STRING: rc = write_string(e, o); break;
    case TNY_CODE_JSON_ARRAY: rc = encode_array(e, o); break;
    case TNY_CODE_JSON_OBJECT: rc = encode_object(e, o); break;
    default:
        if (!e->fallback || e->fallback == Py_None) {
            PyErr_Format(PyExc_TypeError, "Object of type %s is not JSON serializable",
                         Py_TYPE(o)->tp_name);
            rc = -1;
            break;
        }
        PyObject *marker;
        rc = enter_container(e, o, &marker);
        PyObject *replacement = rc ? NULL : PyObject_CallOneArg(e->fallback, o);
        rc = replacement ? encode(e, replacement) : -1;
        Py_XDECREF(replacement);
        leave_container(e, marker);
    }
    Py_LeaveRecursiveCall();
    return rc;
}

static PyObject *json_dumps(PyObject *self, PyObject *args, PyObject *kwargs) {
    (void)self;
    static const char *const names[] = {
        "obj",    "skipkeys",   "ensure_ascii", "check_circular", "allow_nan", "cls",
        "indent", "separators", "default",      "sort_keys",      NULL};
    PyObject *obj, *cls = Py_None, *indent = Py_None, *separators = Py_None, *fallback = Py_None;
    int skipkeys = 0, ensure_ascii = 1, check_circular = 1, allow_nan = 1, sort_keys = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|$ppppOOOOp:dumps", (char *const *)names, &obj,
                                     &skipkeys, &ensure_ascii, &check_circular, &allow_nan, &cls,
                                     &indent, &separators, &fallback, &sort_keys))
        return NULL;
    if (cls != Py_None) {
        PyErr_SetString(PyExc_TypeError, "json.dumps(cls=...) is unavailable; use default=");
        return NULL;
    }
    encoder e = {.skipkeys = skipkeys,
                 .ensure_ascii = ensure_ascii,
                 .check_circular = check_circular,
                 .allow_nan = allow_nan,
                 .sort_keys = sort_keys,
                 .fallback = fallback};
    PyObject *result = NULL;
    if (indent != Py_None) {
        if (PyLong_Check(indent)) {
            Py_ssize_t width = PyLong_AsSsize_t(indent);
            if (width == -1 && PyErr_Occurred()) return NULL;
            PyObject *space = PyUnicode_FromString(" ");
            e.indent = space ? PySequence_Repeat(space, width < 0 ? 0 : width) : NULL;
            Py_XDECREF(space);
        } else if (PyUnicode_Check(indent)) e.indent = Py_NewRef(indent);
        else PyErr_SetString(PyExc_TypeError, "indent must be None, an int or a string");
        if (!e.indent) return NULL;
    }
    if (separators != Py_None) {
        PyObject *item = NULL, *key = NULL;
        if (!PyArg_ParseTuple(separators, "UU:separators", &item, &key)) goto done;
        e.item_separator = Py_NewRef(item);
        e.key_separator = Py_NewRef(key);
    } else {
        e.item_separator = PyUnicode_FromString(e.indent ? "," : ", ");
        e.key_separator = PyUnicode_FromString(": ");
    }
    e.markers = check_circular ? PySet_New(NULL) : NULL;
    e.out = PyUnicodeWriter_Create(0);
    if (!e.item_separator || !e.key_separator || (check_circular && !e.markers) || !e.out)
        goto done;
    if (encode(&e, obj) < 0) goto done;
    result = PyUnicodeWriter_Finish(e.out);
    e.out = NULL;
done:
    if (e.out) PyUnicodeWriter_Discard(e.out);
    Py_XDECREF(e.indent);
    Py_XDECREF(e.item_separator);
    Py_XDECREF(e.key_separator);
    Py_XDECREF(e.markers);
    return result;
}

static PyMethodDef json_methods[] = {
    {"loads", (PyCFunction)(void (*)(void))json_loads, METH_VARARGS | METH_KEYWORDS,
     "loads(s) -> object: parse JSON text"},
    {"dumps", (PyCFunction)(void (*)(void))json_dumps, METH_VARARGS | METH_KEYWORDS,
     "dumps(obj, *, indent=None, separators=None, sort_keys=False, ensure_ascii=True, ...)"},
    {NULL, NULL, 0, NULL},
};

static PyMethodDef print_method = {"print", (PyCFunction)(void (*)(void))code_print,
                                   METH_VARARGS | METH_KEYWORDS,
                                   "print(*values, sep=' ', end='\\n'): append to the result"};

/* Lifecycle -------------------------------------------------------------- */

bool tny_code_python_available(void) { return true; }

int tny_code_python_init(void) {
    if (cell.initialized) return 0;
    PyMemAllocatorEx allocator = {NULL, heap_malloc, heap_calloc, heap_realloc, heap_free};
    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &allocator);
    PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &allocator);
    PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &allocator);
    PyImport_FrozenModules = frozen_modules;
    PyPreConfig pre;
    PyPreConfig_InitIsolatedConfig(&pre);
    pre.utf8_mode = 1;
    PyStatus status = Py_PreInitialize(&pre);
    if (PyStatus_Exception(status)) return -1;
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.site_import = 0;
    config.write_bytecode = 0;
    config.install_signal_handlers = 0;
    config.configure_c_stdio = 0;
    config.module_search_paths_set = 1;
    config.pathconfig_warnings = 0;
    config.use_frozen_modules = 1;
    config.faulthandler = 0;
    config.tracemalloc = 0;
    config.buffered_stdio = 0;
    status = PyConfig_SetString(&config, &config.home, L"/nonexistent-tny-python-home");
    if (!PyStatus_Exception(status)) status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) return -1;
    cell.limit_error = PyErr_NewExceptionWithDoc(
        "tny.CellLimitError", "A code-cell budget was exhausted; the cell is terminal.",
        PyExc_BaseException, NULL);
    cell.decode_error = PyErr_NewException("json.JSONDecodeError", PyExc_ValueError, NULL);
    if (!cell.limit_error || !cell.decode_error) {
        PyErr_Clear();
        return -1;
    }
    cell.initialized = true;
    return 0;
}

/* Builtins minus the import, code-loading and interactive entry points. The
 * OS sandbox, not this list, is the boundary; hiding them keeps generated code
 * on the documented interface and makes refusals immediate and explicit. */
static PyObject *restricted_builtins(void) {
    static const char *removed[] = {
        "__import__", "open", "eval",      "exec",    "compile", "input",      "breakpoint", "help",
        "exit",       "quit", "copyright", "credits", "license", "__loader__", "__spec__",   NULL};
    PyObject *builtins = PyDict_Copy(PyEval_GetBuiltins());
    if (!builtins) return NULL;
    for (size_t i = 0; removed[i]; ++i)
        if (PyDict_PopString(builtins, removed[i], NULL) < 0) {
            Py_DECREF(builtins);
            return NULL;
        }
    PyObject *print = PyCFunction_New(&print_method, NULL);
    if (!print || PyDict_SetItemString(builtins, "print", print) < 0) {
        Py_XDECREF(print);
        Py_DECREF(builtins);
        return NULL;
    }
    Py_DECREF(print);
    return builtins;
}

static PyObject *module_with(const char *name, PyMethodDef *methods) {
    PyObject *module = PyModule_New(name);
    if (module && PyModule_AddFunctions(module, methods) < 0) Py_CLEAR(module);
    return module;
}

static PyObject *cell_globals(void) {
    PyObject *globals = PyDict_New();
    PyObject *builtins = globals ? restricted_builtins() : NULL;
    PyObject *tools = builtins ? module_with("tools", tools_methods) : NULL;
    PyObject *json = tools ? module_with("json", json_methods) : NULL;
    bool ok = json && PyModule_AddObjectRef(json, "JSONDecodeError", cell.decode_error) == 0 &&
              PyDict_SetItemString(globals, "__builtins__", builtins) == 0 &&
              PyDict_SetItemString(globals, "__name__", PyUnicode_FromString("__main__")) == 0 &&
              PyDict_SetItemString(globals, "tools", tools) == 0 &&
              PyDict_SetItemString(globals, "json", json) == 0;
    Py_XDECREF(builtins);
    Py_XDECREF(tools);
    Py_XDECREF(json);
    if (!ok) Py_CLEAR(globals);
    return globals;
}

/* "error: code: Type: message (line N)" for the last frame in the cell. */
static char *format_exception(PyObject *error) {
    PyObject *text = PyObject_Str(error);
    const char *message = text ? PyUnicode_AsUTF8(text) : NULL;
    if (!message) PyErr_Clear();
    const char *type = Py_TYPE(error)->tp_name;
    const char *dot = strrchr(type, '.');
    type = dot ? dot + 1 : type;
    long line = -1;
    if (PyErr_GivenExceptionMatches(error, PyExc_SyntaxError)) {
        PyObject *lineno = PyObject_GetAttrString(error, "lineno");
        line = lineno && PyLong_Check(lineno) ? PyLong_AsLong(lineno) : -1;
        Py_XDECREF(lineno);
        PyErr_Clear();
    }
    PyObject *tb = PyException_GetTraceback(error);
    for (PyObject *t = tb; t && t != Py_None;) {
        PyObject *lineno = PyObject_GetAttrString(t, "tb_lineno");
        PyObject *frame = PyObject_GetAttrString(t, "tb_frame");
        PyCodeObject *code = frame ? PyFrame_GetCode((PyFrameObject *)frame) : NULL;
        const char *file = code ? PyUnicode_AsUTF8(code->co_filename) : NULL;
        if (file && strcmp(file, "<code>") == 0 && lineno) line = PyLong_AsLong(lineno);
        Py_XDECREF((PyObject *)code);
        Py_XDECREF(frame);
        Py_XDECREF(lineno);
        PyObject *next = PyObject_GetAttrString(t, "tb_next");
        if (t != tb) Py_DECREF(t);
        t = next;
        if (!t) break;
    }
    Py_XDECREF(tb);
    PyErr_Clear();
    size_t cap = TNY_CODE_OUTPUT_BYTES + 256;
    char *out = malloc(cap);
    if (!out) {
        Py_XDECREF(text);
        return NULL;
    }
    int n = snprintf(out, cap, "error: code: %s%s%.*s", type, message && *message ? ": " : "",
                     (int)(TNY_CODE_OUTPUT_BYTES / 2), message ? message : "");
    if (line > 0 && n > 0 && (size_t)n < cap)
        snprintf(out + n, cap - (size_t)n, " (line %ld)", line);
    Py_XDECREF(text);
    return out;
}

static char *result_text(char *error) {
    /* Output printed before an error follows it, so the model sees which
     * effects already happened. The error line comes first so "error:"
     * classification of the whole result is unchanged. */
    if (!error) return NULL;
    size_t elen = strlen(error), olen = cell.output_len;
    static const char separator[] = "\n[output before the error]\n";
    size_t room = TNY_CODE_RESULT_TEXT_BYTES - elen - (sizeof separator - 1);
    if (!olen || elen + sizeof separator > TNY_CODE_RESULT_TEXT_BYTES) return error;
    if (olen > room) olen = room;
    char *out = realloc(error, elen + sizeof separator - 1 + olen + 1);
    if (!out) return error;
    memcpy(out + elen, separator, sizeof separator - 1);
    memcpy(out + elen + sizeof separator - 1, cell.output, olen);
    out[elen + sizeof separator - 1 + olen] = 0;
    return out;
}

char *tny_code_python_run(const char *code, const tny_code_python_host *host) {
    if (!code || !tny_code_source_admit(strlen(code))) {
        const char *text = "error: code: source limit exceeded";
        char *copy = malloc(strlen(text) + 1);
        return copy ? strcpy(copy, text) : NULL;
    }
    if (!cell.initialized && tny_code_python_init() != 0) {
        const char *text = "error: code: Python runtime initialization failed";
        char *copy = malloc(strlen(text) + 1);
        return copy ? strcpy(copy, text) : NULL;
    }
    char *output = malloc(TNY_CODE_OUTPUT_BYTES + 1);
    if (!output) return NULL;
    output[0] = 0;
    cell.output = output;
    cell.output_len = 0;
    cell.host = host;
    cell.fatal = NULL;
    cell.calls = 0;
    cell.finished = false;
    atomic_store(&heap.exhausted, false);
    PyObject *globals = cell_globals();
    PyObject *compiled =
        globals ? Py_CompileStringExFlags(code, "<code>", Py_file_input, NULL, -1) : NULL;
    cell.active = compiled != NULL;
    PyObject *result = compiled ? PyEval_EvalCode(compiled, globals, globals) : NULL;
    cell.active = false;
    cell.finished = true;
    char *text = NULL;
    if (cell.fatal) {
        PyErr_Clear();
        size_t cap = 64 + strlen(cell.fatal);
        text = malloc(cap);
        if (text) snprintf(text, cap, "error: code: %s", cell.fatal);
        text = result_text(text);
    } else if (!result) {
        PyObject *error = PyErr_GetRaisedException();
        text = error ? result_text(format_exception(error)) : NULL;
        if (!error && (text = malloc(40))) strcpy(text, "error: code: unknown failure");
        Py_XDECREF(error);
    } else {
        text = malloc(cell.output_len + 1);
        if (text) memcpy(text, cell.output, cell.output_len + 1);
    }
    Py_XDECREF(result);
    Py_XDECREF(compiled);
    /* Finalizers that run while globals are cleared find tools closed. */
    if (globals) PyDict_Clear(globals);
    Py_XDECREF(globals);
    (void)PyGC_Collect();
    PyErr_Clear();
    cell.host = NULL;
    cell.output = NULL;
    free(output);
    return text;
}

void tny_code_python_fini(void) {
    if (!cell.initialized) return;
    Py_CLEAR(cell.limit_error);
    Py_CLEAR(cell.decode_error);
    (void)Py_FinalizeEx();
    cell.initialized = false;
}
