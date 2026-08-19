#include "core/instructions.h"
#include <string.h>
#include <stdlib.h>

static void append_dir_instructions(const char *dir, buf_t *out) {
    char *f = path_join(dir, "AGENTS.md");
    if (!file_exists(f)) {
        free(f);
        f = path_join(dir, "CLAUDE.md");
        if (!file_exists(f)) { free(f); return; }
    }
    size_t len = 0;
    char *data = file_slurp(f, &len);
    if (data && len) {
        buf_appendf(out, "\n# Project instructions from %s\n\n", f);
        buf_append(out, data, len);
        buf_appends(out, "\n");
    }
    free(data);
    free(f);
}

void instructions_collect(tny_ctx *ctx, buf_t *out) {
    if (!ctx->context_enabled) return;

    char *home = path_home();
    size_t home_len = strlen(home);
    char *tny_home = path_join(home, ".tny");
    append_dir_instructions(tny_home, out);
    free(tny_home);

    /* Ancestors of cwd strictly below $HOME, then cwd itself; broader paths
     * first so the narrower file is later in context and wins. */
    const char *cwd = ctx->cwd;
    size_t n = strlen(cwd);
    for (size_t i = 1; i <= n; i++) {
        if (i != n && cwd[i] != '/') continue;
        bool is_cwd = (i == n);
        bool below_home = strncmp(cwd, home, home_len) == 0 &&
                          i > home_len && cwd[home_len] == '/';
        if (is_cwd || below_home) {
            char *prefix = xstrndup(cwd, i);
            append_dir_instructions(prefix, out);
            free(prefix);
        }
    }
    free(home);
}
