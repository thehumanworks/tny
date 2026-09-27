/* Equal-host footprint probe for the actual native Python tools/JSON facade,
 * including the statically linked Unicode parser dependency. This direct
 * adapter is benchmark-only, not the production process-isolation boundary:
 * generated input MUST be run with the replay helper's mandatory OS sandbox. */
#include "bench.h"
#include "core/code_python.h"
#include <stdlib.h>
#include <string.h>

bool bench_execute(bench_state *s, const char *code) {
    if (tny_code_python_init()) return false;
    tny_code_python_host host = {.catalog = s->catalog, .call = bench_call, .userdata = s};
    char *out = tny_code_python_run(code, &host);
    bool ok = out && strncmp(out, "error: code: ", 13) != 0;
    if (out) ok = bench_append(s, out, strlen(out)) && ok;
    free(out);
    tny_code_python_fini();
    return ok;
}
