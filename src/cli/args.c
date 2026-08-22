/* args.c — leading global flags (docs/cli.md). */
#include "cli/cli.h"
#include "core/backend.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *need_val(int argc, char **argv, int *i, const char *flag) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "tny: %s requires a value\nExample: tny %s VALUE ask \"hi\"\n",
                flag, flag);
        return NULL;
    }
    return argv[++*i];
}

int cli_parse_globals(int argc, char **argv, cli_globals *g) {
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-') break; /* subcommand */
        const char *v;
        if (strcmp(a, "--provider") == 0 || strcmp(a, "--backend") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->backend = v;
        } else if (strcmp(a, "--cwd") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->cwd = v;
        } else if (strcmp(a, "--model") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->model = v;
        } else if (strcmp(a, "--effort") == 0 || strcmp(a, "--reasoning-effort") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->effort = v;
        } else if (strcmp(a, "--add-dir") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->add_dirs = realloc(g->add_dirs, sizeof(char *) * (size_t)(g->n_add_dirs + 1));
            g->add_dirs[g->n_add_dirs++] = v;
        } else if (strcmp(a, "--permission-mode") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->perm_mode = v;
        } else if (strcmp(a, "--yolo") == 0) {
            g->perm_mode = "yolo";
        } else if (strcmp(a, "--auto") == 0) {
            g->perm_mode = "auto";
        } else if (strcmp(a, "--json") == 0) {
            g->json = true;
        } else if (strcmp(a, "-r") == 0) {
            g->resume_picker = true;
        } else if (strcmp(a, "-c") == 0 || strcmp(a, "--continue") == 0) {
            g->resume_last = true;
        } else if (strcmp(a, "--resume") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->resume = v;
        } else if (str_starts(a, "--resume-")) {
            g->resume = a + strlen("--resume-");
            if (strcmp(g->resume, "last") == 0) g->resume = "last";
        } else if (strcmp(a, "--bridge-bin") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->bridge_bin = v;
        } else if (strcmp(a, "--codex-ws") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->codex_ws = v;
        } else if (strcmp(a, "--codex-bin") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->codex_bin = v;
        } else if (strcmp(a, "--ws-token-file") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->ws_token_file = v;
        } else if (strcmp(a, "--base-url") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->base_url = v;
        } else if (strcmp(a, "--api-key-env") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->api_key_env = v;
        } else if (strcmp(a, "--agent") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            /* collect: CMD plus everything after `--` */
            int n = 0;
            g->agent_argv = malloc(sizeof(char *) * (size_t)(argc - i + 2));
            g->agent_argv[n++] = v;
            if (i + 1 < argc && strcmp(argv[i + 1], "--") == 0) {
                i += 2;
                /* agent args run until a terminating bare `--` or end of argv:
                 *   tny --agent gemini -- acp -- ask "hi" */
                while (i < argc && strcmp(argv[i], "--") != 0)
                    g->agent_argv[n++] = argv[i++];
                if (i >= argc) i--; /* loop increment lands past the end */
                /* else: leave i on the terminating "--"; increment skips it */
            }
            g->agent_argv[n] = NULL;
        } else {
            fprintf(stderr,
                    "tny: unknown flag '%s'\nGlobal flags come before the command:\n"
                    "  tny --provider codex ask \"hi\"\n", a);
            return -1;
        }
    }
    return i;
}

tny_ctx *cli_make_ctx(const cli_globals *g) {
    tny_ctx *ctx = tny_ctx_load(g->cwd);
    if (!ctx) return NULL;

    if (g->model) {
        free(ctx->model);
        ctx->model = xstrdup(g->model);
        ctx->model_from_flag = true;
    }
    if (g->effort) {
        if (!*g->effort) {
            fprintf(stderr, "tny: --effort must be " TNY_EFFORT_LEVELS
                            " or a value from `tny models`\n");
            tny_ctx_free(ctx);
            return NULL;
        }
        free(ctx->reasoning_effort);
        ctx->reasoning_effort = strcmp(g->effort, "default") == 0
                                    ? NULL : xstrdup(g->effort);
    }
    if (g->perm_mode) {
        if (strcmp(g->perm_mode, "ask") == 0) ctx->perm_mode = TNY_MODE_ASK;
        else if (strcmp(g->perm_mode, "auto") == 0) ctx->perm_mode = TNY_MODE_AUTO;
        else if (strcmp(g->perm_mode, "yolo") == 0) ctx->perm_mode = TNY_MODE_YOLO;
        else {
            fprintf(stderr, "tny: --permission-mode must be ask|auto|yolo\n");
            tny_ctx_free(ctx);
            return NULL;
        }
    }
    ctx->json_out = g->json;
    if (g->bridge_bin) { free(ctx->bridge_bin); ctx->bridge_bin = xstrdup(g->bridge_bin); }
    if (g->codex_ws) { free(ctx->codex_ws); ctx->codex_ws = xstrdup(g->codex_ws); }
    if (g->codex_bin) { free(ctx->codex_bin); ctx->codex_bin = xstrdup(g->codex_bin); }
    if (g->ws_token_file) { free(ctx->ws_token_file); ctx->ws_token_file = xstrdup(g->ws_token_file); }
    if (g->agent_argv) {
        int n = 0;
        while (g->agent_argv[n]) n++;
        ctx->agent_argv = malloc(sizeof(char *) * (size_t)(n + 1));
        for (int k = 0; k < n; k++) ctx->agent_argv[k] = xstrdup(g->agent_argv[k]);
        ctx->agent_argv[n] = NULL;
    }
    /* process-only extra dirs */
    for (int k = 0; k < g->n_add_dirs; k++) {
        char *abs = path_abs(g->add_dirs[k]);
        if (!abs || !dir_exists(abs)) {
            fprintf(stderr, "tny: --add-dir %s: not a directory\n", g->add_dirs[k]);
            free(abs);
            tny_ctx_free(ctx);
            return NULL;
        }
        ctx->extra_dirs = realloc(ctx->extra_dirs,
                                  sizeof(char *) * (size_t)(ctx->n_extra_dirs + 1));
        ctx->extra_dirs[ctx->n_extra_dirs++] = abs;
    }

    if (tny_resolve_backend(ctx, g->backend) < 0) {
        tny_ctx_free(ctx);
        return NULL;
    }
    /* after resolve: flags beat whatever provider profile was applied */
    if (g->base_url) { free(ctx->base_url); ctx->base_url = xstrdup(g->base_url); }
    if (g->api_key_env) {
        const char *k = getenv(g->api_key_env);
        if (k && *k) { free(ctx->api_key); ctx->api_key = xstrdup(k); }
        else {
            fprintf(stderr, "tny: --api-key-env %s: variable is empty\n", g->api_key_env);
            tny_ctx_free(ctx);
            return NULL;
        }
    }
    return ctx;
}
