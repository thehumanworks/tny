#ifndef TNY_BENCH_CODE_MODE_H
#define TNY_BENCH_CODE_MODE_H
#include "yyjson.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define BENCH_OUTPUT_LIMIT 65536u
#define BENCH_CALL_LIMIT 64u
typedef struct {
    yyjson_val *fixture;
    yyjson_mut_doc *result;
    yyjson_mut_val *calls, *writes;
    const char *catalog;
    char output[BENCH_OUTPUT_LIMIT + 1];
    size_t output_len;
    unsigned call_count;
    bool invalid_call;
    int64_t deadline;
} bench_state;
char *bench_call(void *ud, const char *name, const char *args);
char *bench_describe(bench_state *s, const char *name);
bool bench_append(bench_state *s, const char *text, size_t len);
int64_t bench_clock_ns(void);
bool bench_execute(bench_state *s, const char *code);
#endif
