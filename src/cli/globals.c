/* Pure leading-argument grammar shared by the CLI and tool interception.
 * No settings, provider resolution, workspace creation or execution here. */
#include "cli/cli.h"
#include "core/swarm.h"
#include "core/swarm_manifest.h"
#include "util/image_io.h"
#include "util/jobs_host.h"
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
    if (strcmp(name, "optimise") == 0 || strcmp(name, "score") == 0 || strcmp(name, "choose") == 0)
        return true;
    static const char *const names[] = {
        "ask",    "edit",     "speak",          "dictate",   "image",     "ask-user", "resume",
        "acp",    "sessions", "session",        "provider",  "providers", "backends", "models",
        "tasks",  "task",     "permissions",    "workspace", "status",    "doctor",   "usage",
        "cursor", "mcp",      "login",          "logout",    "setup",     "agents",   "web",
        "jobs",   "mailbox",  "task-workspace", "team",      "swarm",     "help",     NULL};
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
        } else if (strcmp(a, "--child-context") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            g->child_context = v;
        } else if (strcmp(a, "--swarm") == 0 || str_starts(a, "--swarm=")) {
            g->swarm_cap = tny_swarm_option(argc, argv, &i);
            if (!g->swarm_cap) {
                if (diagnostics) fputs("tny: --swarm count must be 1..16\n", stderr);
                return -1;
            }
        } else if (strcmp(a, "--swarm-file") == 0) {
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            if (g->swarm_file) {
                if (diagnostics) fputs("tny: --swarm-file may be specified only once\n", stderr);
                return -1;
            }
            g->swarm_file = v;
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
        } else if (strcmp(a, "--no-self-improve") == 0) {
            g->no_self_improve = true;
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
        } else if (strcmp(a, "--acp-agent-argv") == 0) {
            /* Private counted snapshot grammar preserves literal delimiter arguments. */
            if (!(v = need_val(argc, argv, &i, a, diagnostics))) return -1;
            unsigned n = 0;
            bool valid = *v != '\0';
            for (const char *p = v; *p && valid; ++p) {
                if (*p < '0' || *p > '9') valid = false;
                else {
                    n = n * 10u + (unsigned)(*p - '0');
                    if (n > 128) valid = false;
                }
            }
            if (!valid || !n || n > (unsigned)(argc - i - 1) || !*argv[i + 1]) {
                if (diagnostics)
                    fputs("tny: invalid counted ACP command (1..128 arguments required)\n", stderr);
                return -1;
            }
            const char **command = calloc((size_t)n + 1, sizeof *command);
            if (!command) return -1;
            for (unsigned k = 0; k < n; ++k) command[k] = argv[++i];
            free(g->agent_argv);
            g->agent_argv = command;
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
            if (n > 128) {
                if (diagnostics) fputs("tny: ACP command exceeds 128 arguments\n", stderr);
                return -1;
            }
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
    free(parsed.swarm_definition);
    free(parsed.swarm_source);
    return index;
}

int cli_swarm_preflight(cli_globals *g, const char *command, int argc, char **argv) {
    bool requested = g->swarm_cap != 0 || g->swarm_file, ephemeral = g->ephemeral;
    const char *file = g->swarm_file;
    bool numeric = g->swarm_cap != 0;
    if (command && strcmp(command, "ask") == 0) {
        for (int i = 0; i < argc; i++) {
            const char *a = argv[i];
            if (strcmp(a, "--") == 0) break;
            if (strcmp(a, "--swarm") == 0 || str_starts(a, "--swarm=")) {
                if (!tny_swarm_option(argc, argv, &i)) {
                    fputs("tny: --swarm count must be 1..16\n", stderr);
                    return -1;
                }
                requested = true;
                numeric = true;
            } else if (strcmp(a, "--swarm-file") == 0) {
                if (++i >= argc || file) {
                    fputs("tny: --swarm-file requires one PATH selection\n", stderr);
                    return -1;
                }
                file = argv[i];
                requested = true;
            } else if (strcmp(a, "--ephemeral") == 0 || strcmp(a, "--no-save") == 0)
                ephemeral = true;
            else if (strcmp(a, "--task") == 0 || strcmp(a, "--resume") == 0 ||
                     strcmp(a, "--resume-id") == 0 || strcmp(a, "--output-schema") == 0 ||
                     strcmp(a, "--image") == 0 || strcmp(a, "--events") == 0 ||
                     strcmp(a, "--progress") == 0)
                ++i;
        }
    }
    if (file && numeric) {
        fputs("tny: --swarm-file cannot be combined with --swarm; the file defines capacity\n",
              stderr);
        return -1;
    }
    if (file) {
        char *source = path_abs(file);
        tny_swarm_manifest *manifest = NULL;
        char err[256];
        if (!source || tny_swarm_manifest_parse_file(source, TNY_SWARM_MANIFEST_MAX_PARTICIPANTS,
                                                     &manifest, err, sizeof err) != 0) {
            fprintf(stderr, "tny: invalid swarm file: %s\n", source ? err : "invalid path");
            free(source);
            return -1;
        }
        char digest[65];
        bool hashed =
            tny_image_io_sha256_hex(manifest->canonical_json, manifest->canonical_len, digest);
        char *definition = hashed ? xstrdup(manifest->canonical_json) : NULL;
        if (!definition) {
            fputs("tny: could not retain validated swarm definition\n", stderr);
            tny_swarm_manifest_free(manifest);
            free(source);
            return -1;
        }
        free(g->swarm_definition);
        free(g->swarm_source);
        g->swarm_definition = definition;
        g->swarm_source = source;
        snprintf(g->swarm_definition_digest, sizeof g->swarm_definition_digest, "%s", digest);
        g->swarm_participants = (int)manifest->participant_count;
        g->swarm_file = file;
        tny_swarm_manifest_free(manifest);
    }
    if (requested && (ephemeral || g->ssh || getenv("TNY_TEAM_RUN") ||
                      !tny_jobs_host_execution_supported() || !tny_jobs_host_watch_supported())) {
        fputs("tny: swarm requires a saved native local lead session\n", stderr);
        return -1;
    }
    return 0;
}
