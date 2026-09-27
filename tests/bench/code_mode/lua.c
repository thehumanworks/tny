/* Link the production runtime unchanged. */
#include "bench.h"
#include "core/code_runtime.h"
#include <stdlib.h>
#include <string.h>
bool bench_execute(bench_state *s, const char *code) {
    char *out = tny_code_run(code, 2000, s->catalog, bench_call, s);
    if (!out) return false;
    bool ok = strncmp(out, "error: code: ", 13) != 0;
    ok = bench_append(s, out, strlen(out)) && ok;
    free(out);
    return ok;
}
