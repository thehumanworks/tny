#include "core/instructions.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static void remember_path(tny_ctx *ctx, const char *path, bool record) {
    if (!record) return;
    char **next = realloc(ctx->instruction_paths,
                          sizeof(char *) * (size_t)(ctx->n_instruction_paths + 1));
    if (!next) return;
    ctx->instruction_paths = next;
    char *copy = xstrdup(path);
    if (!copy) return;
    ctx->instruction_paths[ctx->n_instruction_paths++] = copy;
}

static void append_dir_instructions(tny_ctx *ctx, const char *dir, buf_t *out,
                                    bool record) {
    char *f = path_join(dir, "AGENTS.md");
    if (!f) return;
    if (!file_exists(f)) {
        free(f);
        f = path_join(dir, "CLAUDE.md");
        if (!f) return;
        if (!file_exists(f)) { free(f); return; }
    }
    size_t len = 0;
    char *data = file_slurp(f, &len);
    if (data && len) {
        remember_path(ctx, f, record);
        buf_appendf(out, "\n# Project instructions from %s\n\n", f);
        buf_append(out, data, len);
        buf_appends(out, "\n");
    }
    free(data);
    free(f);
}

static void collect_raw(tny_ctx *ctx, buf_t *out, bool record) {
    if (!ctx->context_enabled) return;

    /* Embedders authorize exactly the supplied workspace. Never import
     * HOME-level or ancestor instructions from the host application. */
    if (ctx->library_mode) {
        append_dir_instructions(ctx, ctx->cwd, out, record);
        return;
    }

    char *home = path_home();
    if (!home) return;
    size_t home_len = strlen(home);
    char *tny_home = path_join(home, ".tny");
    append_dir_instructions(ctx, tny_home, out, record);
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
            if (prefix) append_dir_instructions(ctx, prefix, out, record);
            free(prefix);
        }
    }
    free(home);
}

int instructions_refresh(tny_ctx *ctx) {
    if (!ctx) return -1;
    for (int i = 0; i < ctx->n_instruction_paths; i++)
        free(ctx->instruction_paths[i]);
    free(ctx->instruction_paths);
    ctx->instruction_paths = NULL;
    ctx->n_instruction_paths = 0;
    buf_t out;
    buf_init(&out);
    collect_raw(ctx, &out, true);
    free(ctx->instructions_snapshot);
    ctx->instructions_snapshot = buf_detach(&out);
    snprintf(ctx->instructions_digest, sizeof ctx->instructions_digest,
             "%016llx", (unsigned long long)fnv1a(
                 ctx->instructions_snapshot ? ctx->instructions_snapshot : "",
                 ctx->instructions_snapshot ? strlen(ctx->instructions_snapshot) : 0));
    ctx->instructions_snapshot_ready = true;
    return 0;
}

void instructions_collect(tny_ctx *ctx, buf_t *out) {
    if (!ctx || !out) return;
    if (ctx->instructions_snapshot_ready) {
        if (ctx->instructions_snapshot) buf_appends(out, ctx->instructions_snapshot);
        return;
    }
    collect_raw(ctx, out, false);
}
