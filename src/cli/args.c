/* args.c — leading global flags (docs/cli.md). */
#include "cli/cli.h"
#include "core/backend.h"
#include "core/extensions.h"
#include "core/ssh.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *need_val(int argc, char **argv, int *i, const char *flag) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "tny: %s requires a value\nExample: tny %s VALUE ask \"hi\"\n", flag, flag);
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
        if (strcmp(a, "--ssh") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->ssh = v;
        } else if (strcmp(a, "--ssh-cwd") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->ssh_cwd = v;
        } else if (strcmp(a, "--provider") == 0 || strcmp(a, "--backend") == 0) {
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
        } else if (strcmp(a, "--system-prompt") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->system_prompt = v;
        } else if (strcmp(a, "--add-dir") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            const char **dirs = realloc(g->add_dirs, sizeof(char *) * (size_t)(g->n_add_dirs + 1));
            if (!dirs) return -1;
            g->add_dirs = dirs;
            g->add_dirs[g->n_add_dirs++] = v;
        } else if (strcmp(a, "--permission-mode") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->perm_mode = v;
        } else if (strcmp(a, "--max-steps") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->max_steps = v;
        } else if (strcmp(a, "--max-extension-iterations") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->max_extension_iterations = v;
        } else if (strcmp(a, "--no-extensions") == 0) {
            g->no_extensions = true;
        } else if (strcmp(a, "--fast") == 0) {
            g->fast = true;
        } else if (strcmp(a, "--yolo") == 0) {
            g->perm_mode = "yolo";
        } else if (strcmp(a, "--auto") == 0) {
            g->perm_mode = "auto";
        } else if (strcmp(a, "--json") == 0) {
            g->json = true;
        } else if (strcmp(a, "--color") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->color = v;
        } else if (str_starts(a, "--color=")) {
            g->color = a + strlen("--color=");
        } else if (strcmp(a, "--no-color") == 0) {
            g->color = "never";
        } else if (strcmp(a, "--ephemeral") == 0 || strcmp(a, "--no-save") == 0) {
            g->ephemeral = true;
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
        } else if (strcmp(a, "--wire-api") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            g->wire_api = v;
        } else if (strcmp(a, "--agent") == 0) {
            if (!(v = need_val(argc, argv, &i, a))) return -1;
            /* collect: CMD plus everything after `--` */
            int n = 0;
            g->agent_argv = malloc(sizeof(char *) * (size_t)(argc - i + 2));
            if (!g->agent_argv) return -1;
            g->agent_argv[n++] = v;
            if (i + 1 < argc && strcmp(argv[i + 1], "--") == 0) {
                i += 2;
                /* agent args run until a terminating bare `--` or end of argv:
                 *   tny --agent gemini -- acp -- ask "hi" */
                while (i < argc && strcmp(argv[i], "--") != 0) g->agent_argv[n++] = argv[i++];
                if (i >= argc) i--; /* loop increment lands past the end */
                /* else: leave i on the terminating "--"; increment skips it */
            }
            g->agent_argv[n] = NULL;
        } else {
            fprintf(stderr,
                    "tny: unknown flag '%s'\nGlobal flags come before the command:\n"
                    "  tny --provider codex ask \"hi\"\n",
                    a);
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
            fprintf(stderr,
                    "tny: --effort must be " TNY_EFFORT_LEVELS " or a value from `tny models`\n");
            tny_ctx_free(ctx);
            return NULL;
        }
        free(ctx->reasoning_effort);
        ctx->reasoning_effort = strcmp(g->effort, "default") == 0 ? NULL : xstrdup(g->effort);
        /* "default" included: the flag must also beat a settings.json
         * default applied when the provider resolves (docs/adr/0015) */
        ctx->effort_explicit = true;
        ctx->effort_from_settings = false;
    }
    if (g->system_prompt && *g->system_prompt) {
        free(ctx->system_prompt);
        ctx->system_prompt = xstrdup(g->system_prompt);
    }
    if (g->max_steps) {
        int v = tny_parse_max_steps(g->max_steps);
        if (v < 0) {
            fprintf(stderr, "tny: --max-steps takes a positive integer or "
                            "'unlimited'\nExample: tny --max-steps 30 ask \"hi\"\n");
            tny_ctx_free(ctx);
            return NULL;
        }
        ctx->max_steps = v; /* explicit flag beats the .tny.json "steps" cap */
    }
    if (g->max_extension_iterations) {
        int v = tny_parse_max_steps(g->max_extension_iterations);
        if (v < 0) {
            fprintf(stderr, "tny: --max-extension-iterations takes a positive "
                            "integer or 'unlimited'\nExample: tny "
                            "--max-extension-iterations 8 ask \"hi\"\n");
            tny_ctx_free(ctx);
            return NULL;
        }
        ctx->max_extension_iterations = v;
    }
    if (g->no_extensions) {
        ctx->extensions_enabled = false;
        tny_extensions_free(ctx->extensions);
        ctx->extensions = NULL;
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
    ctx->no_save = g->ephemeral;
    if (g->color) {
        if (strcmp(g->color, "never") == 0) ctx->no_color = true;
        else if (strcmp(g->color, "always") == 0) ctx->force_color = true;
        else if (strcmp(g->color, "auto") != 0) {
            fprintf(stderr, "tny: --color must be auto|always|never\n"
                            "Example: NO_COLOR= tny --color=always\n");
            tny_ctx_free(ctx);
            return NULL;
        }
    }
    if (g->bridge_bin) {
        free(ctx->bridge_bin);
        ctx->bridge_bin = xstrdup(g->bridge_bin);
    }
    if (g->codex_ws) {
        free(ctx->codex_ws);
        ctx->codex_ws = xstrdup(g->codex_ws);
    }
    if (g->codex_bin) {
        free(ctx->codex_bin);
        ctx->codex_bin = xstrdup(g->codex_bin);
    }
    if (g->ws_token_file) {
        free(ctx->ws_token_file);
        ctx->ws_token_file = xstrdup(g->ws_token_file);
    }
    /* Mark an explicit speed choice before provider resolution so a settings
     * default cannot run first. Capability validation stays after resolve. */
    if (g->fast) ctx->service_tier_explicit = true;
    if (g->agent_argv) {
        int n = 0;
        while (g->agent_argv[n]) n++;
        ctx->agent_argv = malloc(sizeof(char *) * (size_t)(n + 1));
        if (!ctx->agent_argv) {
            tny_ctx_free(ctx);
            return NULL;
        }
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
        char **dirs = realloc(ctx->extra_dirs, sizeof(char *) * (size_t)(ctx->n_extra_dirs + 1));
        if (!dirs) {
            free(abs);
            tny_ctx_free(ctx);
            return NULL;
        }
        ctx->extra_dirs = dirs;
        ctx->extra_dirs[ctx->n_extra_dirs++] = abs;
    }

    if (tny_resolve_backend(ctx, g->backend) < 0) {
        tny_ctx_free(ctx);
        return NULL;
    }
    if (g->ssh && cli_ssh_attach(ctx, g->ssh, g->ssh_cwd) != 0) {
        tny_ctx_free(ctx);
        return NULL;
    }
    /* after resolve: flags beat whatever provider profile was applied */
    if (g->base_url) {
        free(ctx->base_url);
        ctx->base_url = xstrdup(g->base_url);
    }
    if (g->wire_api) {
        if (strcmp(g->wire_api, "responses") != 0 && strcmp(g->wire_api, "chat") != 0) {
            fprintf(stderr, "tny: --wire-api must be responses|chat\n"
                            "Example: tny --wire-api chat ask \"hi\"\n");
            tny_ctx_free(ctx);
            return NULL;
        }
        free(ctx->wire_api);
        ctx->wire_api = xstrdup(g->wire_api);
    }
    if (g->api_key_env) {
        const char *k = getenv(g->api_key_env);
        if (k && *k) {
            free(ctx->api_key);
            ctx->api_key = xstrdup(k);
        } else {
            fprintf(stderr, "tny: --api-key-env %s: variable is empty\n", g->api_key_env);
            tny_ctx_free(ctx);
            return NULL;
        }
    }
    /* --fast needs the resolved provider: it is a capability, not a knob
     * every backend has. Capable providers map it to their own wire field. */
    if (g->fast) {
        if (!(tny_backend_caps((tny_backend_id)ctx->backend) & TNY_CAP_FAST)) {
            fprintf(stderr,
                    "tny: --fast is not supported by provider '%s'\n"
                    "Providers with a fast tier:",
                    tny_backend_name((tny_backend_id)ctx->backend));
            for (int b = 0; b < TNY_BK_COUNT; b++)
                if (tny_backend_caps((tny_backend_id)b) & TNY_CAP_FAST)
                    fprintf(stderr, " %s", tny_backend_name((tny_backend_id)b));
            fprintf(stderr, "\nExample: tny --provider codex --fast ask \"hi\"\n");
            tny_ctx_free(ctx);
            return NULL;
        }
        free(ctx->service_tier);
        ctx->service_tier = xstrdup("fast");
        ctx->service_tier_from_settings = false;
    }
    return ctx;
}

/* --ssh: tny stays local; the native loop's workspace tools run on TARGET
 * (docs/adr/0022). Host backends own their own tool loops and cannot be
 * redirected, so they are refused rather than silently running locally. */
int cli_ssh_attach(tny_ctx *ctx, const char *target, const char *remote_cwd) {
    if (ctx->backend != TNY_BK_OPENAI) {
        fprintf(stderr,
                "tny: --ssh runs tools through tny's native loop; provider '%s' "
                "executes its own tools on this machine.\n"
                "Use an openai-compatible provider: tny --ssh %s --provider claude\n",
                tny_backend_name((tny_backend_id)ctx->backend), target);
        return -1;
    }
    char err[256];
    if (ssh_target_set(ctx, target, err, sizeof err) < 0) {
        fprintf(stderr, "tny: --ssh: %s\n", err);
        return -1;
    }
    free(ctx->ssh_cwd);
    ctx->ssh_cwd = remote_cwd && *remote_cwd ? xstrdup(remote_cwd) : NULL;
    if (ssh_connect(ctx, err, sizeof err) < 0) {
        fprintf(stderr, "tny: --ssh: %s\n", err);
        ssh_disconnect(ctx);
        return -1;
    }
    return 0;
}
