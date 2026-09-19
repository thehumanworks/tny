/* Source-level file-tool probe for bench_edit_feedback.py. No provider, user
 * configuration, session, or persistent result store is opened. Unit tests in
 * test_core.c separately cover the full permission/schema tools_execute path. */
#include "core/tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include the selected baseline/A/B implementation without exporting a new
 * application API. Unused functions are discarded by the benchmark link. */
#ifndef TNY_BENCH_TOOLS_FS
#define TNY_BENCH_TOOLS_FS "core/tools_fs.c"
#endif
#include TNY_BENCH_TOOLS_FS

/* tool_bound_result never stores without a session. Keep accidental changes
 * to that assumption observable rather than opening the user's state. */
char *session_store_result(tny_session_state *session, const char *data, size_t len) {
    (void)session;
    (void)data;
    (void)len;
    abort();
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: probe WORKSPACE TOOL REQUEST_JSON_FILE\n");
        return 2;
    }
    size_t len = 0;
    char *input = file_slurp(argv[3], &len);
    yyjson_doc *doc = input ? jparse(input, len) : NULL;
    if (!doc) {
        free(input);
        return 2;
    }
    tny_ctx ctx = {0};
    ctx.cwd = argv[1];
    ctx.max_tool_result_bytes = 32768;
    tools_env env = {.ctx = &ctx};
    bool handled = false;
    char *result = tool_fs_execute(&env, argv[2], yyjson_doc_get_root(doc), &handled);
    int rc = handled && result ? 0 : 1;
    if (result) fputs(result, stdout);
    free(result);
    yyjson_doc_free(doc);
    free(input);
    return rc;
}
