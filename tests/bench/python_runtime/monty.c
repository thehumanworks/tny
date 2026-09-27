/* Benchmark-only Monty adapter; Rust staticlib from monty_probe/. */
#include "bench.h"
#include <stdlib.h>
#include <string.h>
int tny_monty_probe_run(const char *code, const char *catalog,
                        char *(*call)(void *, const char *, const char *),
                        char *(*describe)(void *, const char *), void *ud,
                        unsigned long long timeout_ms, size_t max_output, char **out);
void tny_monty_probe_free(char *text);
static char *describe(void *ud, const char *name) { return bench_describe(ud, name); }
bool bench_execute(bench_state *s, const char *code) {
    char *out = NULL;
    int ok = tny_monty_probe_run(code, s->catalog, bench_call, describe, s, 2000,
                                 BENCH_OUTPUT_LIMIT, &out);
    bool appended = out && bench_append(s, out, strlen(out));
    tny_monty_probe_free(out);
    return ok && appended;
}
