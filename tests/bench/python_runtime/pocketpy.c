/* Benchmark-only PocketPy adapter. Runtime json module; outer watchdog only. */
#include "bench.h"
#include "pocketpy.h"
#include <stdlib.h>
#include <string.h>
static bench_state *active;
static bool overflow;
static void out(const char *text) {
    if (!bench_append(active, text, strlen(text))) overflow = true;
}
static bool tools_call(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    PY_CHECK_ARG_TYPE(0, tp_str);
    PY_CHECK_ARG_TYPE(1, tp_str);
    char *result = bench_call(active, py_tostr(py_arg(0)), py_tostr(py_arg(1)));
    if (!result) return RuntimeError("tool callback failed");
    py_newstr(py_retval(), result);
    free(result);
    return true;
}
static bool tools_list(int argc, py_Ref argv) {
    PY_CHECK_ARGC(0);
    py_newstr(py_retval(), active->catalog);
    return true;
}
static bool tools_describe(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    PY_CHECK_ARG_TYPE(0, tp_str);
    char *result = bench_describe(active, py_tostr(py_arg(0)));
    if (result) py_newstr(py_retval(), result);
    else py_newnone(py_retval());
    free(result);
    return true;
}
bool bench_execute(bench_state *s, const char *code) {
    active = s;
    py_initialize();
    py_callbacks()->print = out;
    py_GlobalRef tools = py_newmodule("tools");
    py_bindfunc(tools, "call", tools_call);
    py_bindfunc(tools, "list", tools_list);
    py_bindfunc(tools, "describe", tools_describe);
    bool ok = py_exec("import json\nimport tools\n", "<setup>", EXEC_MODE, NULL) &&
              py_exec(code, "code", EXEC_MODE, NULL);
    if (!ok) {
        char *message = py_formatexc();
        (void)bench_append(s, "error: ", 7);
        if (message) (void)bench_append(s, message, strlen(message));
        free(message);
    }
    py_finalize();
    active = NULL;
    return ok && !overflow;
}
