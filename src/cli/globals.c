/* Pure leading-argument grammar shared by the CLI and tool interception.
 * No settings, provider resolution, workspace creation or execution here. */
#include "cli/cli.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *need_val(int argc, char **argv, int *i, const char *flag, bool diagnostics) {
    if (*i + 1 >= argc) {
        if (diagnostics)
            fprintf(stderr, "tny: %s requires a value\nExample: tny %s VALUE ask \"hi\"\n", flag,
                    flag);
        return NULL;
    }
    return argv[++*i];
}

bool cli_is_command(const char *name) {
    if (strcmp(name, "optimise") == 0) return true;
    static const char *const names[] = {
        "ask",      "edit",   "speak",    "dictate", "image",       "ask-user",
        "resume",   "acp",    "sessions", "session", "provider",    "providers",
        "backends", "models", "tasks",    "task",    "permissions", "workspace",
        "status",   "doctor", "usage",    "cursor",  "mcp",         "login",
        "logout",   "setup",  "jobs",     "help",    NULL};
    for (size_t i = 0; names[i]; i++)
        if (strcmp(name, names[i]) == 0) return true;
    return false;
}

static int parse_globals(int argc, char **argv, cli_globals *g, bool diagnostics) {
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) return i + 1;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0 || strcmp(a, "--version") == 0 ||
            strcmp(a, "-v") == 0)
            break;
        if (a[0] != '-') break; /* subcommand */
        const char *v;
        if (strcmp(a, "--ssh") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->ssh = v;
        } else if (strcmp(a, "--ssh-cwd") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->ssh_cwd = v;
        } else if (strcmp(a, "--provider") == 0 || strcmp(a, "--backend") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->backend = v;
        } else if (strcmp(a, "--cwd") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->cwd = v;
        } else if (strcmp(a, "--worktree") == 0) {
            g->worktree = true;
            g->worktree_name = NULL;
            if (i + 1 < argc && argv[i + 1][0] != '-' && !cli_is_command(argv[i + 1]))
                g->worktree_name = argv[++i];
        } else if (str_starts(a, "--worktree=")) {
            g->worktree = true;
            g->worktree_name = a + strlen("--worktree=");
        } else if (strcmp(a, "--model") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->model = v;
        } else if (strcmp(a, "--effort") == 0 || strcmp(a, "--reasoning-effort") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->effort = v;
        } else if (strcmp(a, "--system-prompt") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->system_prompt = v;
        } else if (strcmp(a, "--task") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->task = v;
        } else if (strcmp(a, "--add-dir") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            const char **dirs = realloc(g->add_dirs, sizeof(char *) * (size_t)(g->n_add_dirs + 1));
            if (!dirs) return -1;
            g->add_dirs = dirs;
            g->add_dirs[g->n_add_dirs++] = v;
        } else if (strcmp(a, "--permission-mode") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->perm_mode = v;
        } else if (strcmp(a, "--max-steps") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->max_steps = v;
        } else if (strcmp(a, "--max-extension-iterations") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
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
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
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
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->resume = v;
        } else if (str_starts(a, "--resume-")) {
            g->resume = a + strlen("--resume-");
            if (strcmp(g->resume, "last") == 0) g->resume = "last";
        } else if (strcmp(a, "--bridge-bin") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->bridge_bin = v;
        } else if (strcmp(a, "--xai-api-key") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            if (!*v || strpbrk(v, "\r\n")) {
                if (diagnostics)
                    fputs("tny: --xai-api-key must be nonempty and contain no CR/LF\n", stderr);
                return -1;
            }
            g->xai_api_key = v;
        } else if (strcmp(a, "--chatgpt-token") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->chatgpt_token = v;
        } else if (strcmp(a, "--chatgpt-account-id") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->chatgpt_account_id = v;
        } else if (strcmp(a, "--base-url") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->base_url = v;
        } else if (strcmp(a, "--base-url-env") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->base_url_env = v;
        } else if (strcmp(a, "--api-key-env") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->api_key_env = v;
        } else if (strcmp(a, "--wire-api") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->wire_api = v;
        } else if (strcmp(a, "--agent") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            /* collect: CMD plus everything after `--` */
            int n = 0;
            free(g->agent_argv);
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
            if (diagnostics)
                fprintf(stderr,
                        "tny: unknown flag '%s'\nGlobal flags come before the command:\n"
                        "  tny --provider codex ask \"hi\"\n",
                        a);
            return -1;
        }
    }
    return i;
}

int cli_parse_globals(int argc, char **argv, cli_globals *g) {
    return parse_globals(argc, argv, g, true);
}

int cli_command_index(int argc, char **argv) {
    cli_globals parsed = {0};
    int index = parse_globals(argc, argv, &parsed, false);
    free(parsed.add_dirs);
    free(parsed.agent_argv);
    return index;
}
