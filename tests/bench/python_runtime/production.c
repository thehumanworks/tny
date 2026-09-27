/* Replay executor: the preserved benchmark host over the PRODUCTION code-cell
 * path (tny_code_run spawns this executable's --code-cell entry with the OS
 * sandbox). Built by `make python-cell-bench`; benchmark only. */
#include "bench.h"
#include "core/code_runtime.h"
#include <stdlib.h>
#include <string.h>

int bench_host_main(void);

bool bench_execute(bench_state *s, const char *code) {
    char *out = tny_code_run(code, 2000, s->catalog, bench_call, s);
    if (!out) return false;
    bool ok = strncmp(out, "error: code: ", 13) != 0;
    ok = bench_append(s, out, strlen(out)) && ok;
    free(out);
    return ok;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--code-cell") == 0) return tny_code_cell_main();
    return bench_host_main();
}
