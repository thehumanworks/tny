/* Standalone native tools: `tny tool list|describe|call`, plus the documented
 * `tny memory` and `tny skill` shortcuts. Each call runs the same nested tool
 * the model reaches through tools.call(), with the same permission engine,
 * workspace roots and --ssh routing, without a provider or a turn
 * (docs/cli.md, ADR 0217). */
#include "cli/cli.h"
#include "core/tool_extensions.h"
#include "core/tools.h"
#include "json/json.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TOOL_ARGS_MAX (16u * 1024u * 1024u)

static volatile sig_atomic_t interrupted;
static void tool_signal(int sig) {
    (void)sig;
    interrupted = 1;
}
static bool tool_cancelled(void *ud) {
    (void)ud;
    return interrupted != 0;
}

static int usage(void) {
    fputs("tny: usage: tny tool list [--json] | describe NAME | call NAME [ARGS_JSON|-] [--json]\n"
          "       tny tool check [--json] | new NAME [--global]\n"
          "Example: tny tool call read_file '{\"path\":\"README.md\"}'\n",
          stderr);
    return 1;
}

/* Reads all of stdin; NULL on read error, oversize or OOM. */
static char *read_stdin(void) {
    buf_t b;
    buf_init(&b);
    for (;;) {
        char chunk[8192];
        ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR && !interrupted) continue;
        if (n < 0 || interrupted || (size_t)n > TOOL_ARGS_MAX - b.len) {
            buf_free(&b);
            return NULL;
        }
        if (!n) break;
        buf_append(&b, chunk, (size_t)n);
    }
    if (buf_oom(&b)) {
        buf_free(&b);
        return NULL;
    }
    return b.data ? buf_detach(&b) : xstrdup("");
}

/* Finds `name` in a catalog array; borrowed from doc. */
static yyjson_val *catalog_find(yyjson_val *root, const char *name) {
    if (!name) return NULL;
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(root, idx, max, item) {
        const char *n = jget_str(jget(item, "function"), "name");
        if (n && strcmp(n, name) == 0) return jget(item, "function");
    }
    return NULL;
}

static int tool_list(tools_env *env, bool json) {
    char *catalog = tools_catalog_json(env);
    yyjson_doc *doc = catalog ? jparse(catalog, strlen(catalog)) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!root || !yyjson_is_arr(root)) {
        yyjson_doc_free(doc);
        free(catalog);
        fputs("tny: tool: cannot build the tool catalog\n", stderr);
        return 1;
    }
    if (json) {
        /* The catalog minus turn-bound tools, in the model-facing shape. */
        yyjson_mut_doc *mut = yyjson_mut_doc_new(jallocator());
        yyjson_mut_val *arr = mut ? yyjson_mut_arr(mut) : NULL;
        size_t idx, max;
        yyjson_val *item;
        bool ok = arr != NULL;
        yyjson_arr_foreach(root, idx, max, item) {
            const char *name = jget_str(jget(item, "function"), "name");
            if (!ok || !name || tools_turn_bound(name)) continue;
            ok = yyjson_mut_arr_add_val(arr, yyjson_val_mut_copy(mut, item));
        }
        if (ok) yyjson_mut_doc_set_root(mut, arr);
        char *text = ok ? jwrite(mut) : NULL;
        if (text) puts(text);
        free(text);
        yyjson_mut_doc_free(mut);
        if (!text) {
            yyjson_doc_free(doc);
            free(catalog);
            fputs("tny: tool: cannot build the tool catalog\n", stderr);
            return 1;
        }
    } else {
        size_t idx, max;
        yyjson_val *item;
        yyjson_arr_foreach(root, idx, max, item) {
            yyjson_val *fn = jget(item, "function");
            const char *name = jget_str(fn, "name");
            const char *desc = jget_str(fn, "description");
            if (!name || tools_turn_bound(name)) continue;
            /* First sentence: up to ". " or a newline, bounded for a terminal. */
            size_t dl = desc ? strcspn(desc, "\n") : 0;
            for (const char *stop = desc ? strstr(desc, ". ") : NULL; stop;
                 stop = strstr(stop + 2, ". ")) {
                if ((size_t)(stop - desc) >= dl) break;
                /* "e.g. x" is not a sentence end; "Read. Then" is. */
                if (stop[2] >= 'A' && stop[2] <= 'Z') {
                    dl = (size_t)(stop - desc);
                    break;
                }
            }
            if (dl > 0 && desc[dl - 1] == '.') dl--;
            printf("%-23s %.*s\n", name, (int)(dl > 96 ? 96 : dl), desc ? desc : "");
        }
    }
    yyjson_doc_free(doc);
    free(catalog);
    return 0;
}

static int tool_describe(tools_env *env, const char *name) {
    char *catalog = tools_catalog_json(env);
    yyjson_doc *doc = catalog ? jparse(catalog, strlen(catalog)) : NULL;
    yyjson_val *fn =
        doc && !tools_turn_bound(name) ? catalog_find(yyjson_doc_get_root(doc), name) : NULL;
    char *text = fn ? jwrite_val(fn) : NULL;
    int rc = 0;
    if (text) {
        puts(text);
    } else {
        fprintf(stderr, "tny: tool: unknown or unavailable tool '%s' (see tny tool list)\n", name);
        rc = 2;
    }
    free(text);
    yyjson_doc_free(doc);
    free(catalog);
    return rc;
}

/* Runs one call and prints it. Exit 0 ok, 2 tool error, 130 interrupted. */
static int tool_run(tools_env *env, const char *name, const char *args, bool json) {
    char *result = tools_turn_bound(name)
                       ? tool_err("%s belongs to a conversation turn; use it from tny ask or the "
                                  "TUI (images: tny ask --image PATH)",
                                  name)
                       : tools_execute(env, name, args);
    int code = interrupted ? 130 : !result || str_starts(result, "error:") ? 2 : 0;
    if (json) {
        buf_t out;
        buf_init(&out);
        buf_appends(&out, "{\"kind\":\"tool\",\"name\":");
        jescape(&out, name);
        buf_appendf(&out, ",\"ok\":%s,\"result\":", code ? "false" : "true");
        jescape(&out, result ? result : "error: tool call failed");
        buf_appends(&out, "}\n");
        if (!buf_oom(&out)) fputs(out.data, stdout);
        buf_free(&out);
    } else {
        const char *text = result ? result : "error: tool call failed";
        FILE *f = code ? stderr : stdout;
        fputs(text, f);
        if (!*text || text[strlen(text) - 1] != '\n') fputc('\n', f);
    }
    free(result);
    return code;
}

/* `tny memory get KEY | set KEY [VALUE|stdin] | list` → memory tool args. */
static char *memory_args(int argc, char **argv) {
    if (argc < 1) return NULL;
    const char *action = argv[0];
    bool get = strcmp(action, "get") == 0, set = strcmp(action, "set") == 0,
         list = strcmp(action, "list") == 0;
    if ((!get && !set && !list) || (list && argc != 1) || (get && argc != 2) ||
        (set && (argc < 2 || argc > 3)))
        return NULL;
    char *piped = NULL;
    if (set && argc == 2) {
        if (isatty(STDIN_FILENO)) return NULL;
        piped = read_stdin();
        if (!piped) return NULL;
    }
    buf_t b;
    buf_init(&b);
    buf_appends(&b, "{\"action\":");
    jescape(&b, action);
    if (argc >= 2) {
        buf_appends(&b, ",\"key\":");
        jescape(&b, argv[1]);
    }
    if (set) {
        buf_appends(&b, ",\"value\":");
        jescape(&b, argc == 3 ? argv[2] : piped);
    }
    buf_appends(&b, "}");
    free(piped);
    if (buf_oom(&b)) {
        buf_free(&b);
        return NULL;
    }
    return buf_detach(&b);
}

int cmd_tool(tny_ctx *ctx, const cli_globals *g, const char *command, int argc, char **argv) {
    bool json = g->json;
    /* Trailing --json is accepted anywhere after the verb, as for tny web. */
    int n = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json = true;
        else argv[n++] = argv[i];
    }
    argc = n;

    const char *verb = NULL, *name = NULL;
    char *args = NULL;
    bool memory = strcmp(command, "memory") == 0, skill = strcmp(command, "skill") == 0;
    if (memory) {
        name = "memory";
        args = memory_args(argc, argv);
        if (!args) {
            fputs("tny: usage: tny memory get KEY | set KEY [VALUE] | list\n"
                  "Example: printf 'tabs' | tny memory set indent\n",
                  stderr);
            return 1;
        }
        verb = "call";
    } else if (skill) {
        if (argc != 2 || strcmp(argv[0], "show") != 0) {
            fputs("tny: usage: tny skill show NAME\n", stderr);
            return 1;
        }
        name = "skill";
        buf_t b;
        buf_init(&b);
        buf_appends(&b, "{\"name\":");
        jescape(&b, argv[1]);
        buf_appends(&b, "}");
        if (buf_oom(&b)) {
            buf_free(&b);
            return 1;
        }
        args = buf_detach(&b);
        verb = "call";
    } else {
        verb = argc > 0 ? argv[0] : "";
        if (strcmp(verb, "list") == 0) {
            if (argc != 1) return usage();
        } else if (strcmp(verb, "describe") == 0) {
            if (argc != 2) return usage();
            name = argv[1];
        } else if (strcmp(verb, "check") == 0) {
            if (argc != 1) return usage();
            return cmd_tool_check(ctx, json);
        } else if (strcmp(verb, "new") == 0) {
            return cmd_tool_new(ctx, argc - 1, argv + 1);
        } else if (strcmp(verb, "call") == 0) {
            if (argc < 2 || argc > 3) return usage();
            name = argv[1];
            if (argc == 2 || strcmp(argv[2], "-") == 0) {
                if (argc == 2 && isatty(STDIN_FILENO)) args = xstrdup("{}");
                else args = read_stdin();
                if (!args) {
                    fputs("tny: tool: cannot read arguments from stdin\n", stderr);
                    return 1;
                }
                if (!*args) {
                    free(args);
                    args = xstrdup("{}");
                }
            } else {
                args = xstrdup(argv[2]);
            }
            if (!args) return 1;
            yyjson_doc *d = jparse(args, strlen(args));
            bool object = d && yyjson_is_obj(yyjson_doc_get_root(d));
            yyjson_doc_free(d);
            if (!object) {
                fputs("tny: tool: arguments must be one JSON object\n", stderr);
                free(args);
                return 1;
            }
        } else {
            return usage();
        }
    }

    int rc = cmd_tool_session(ctx, verb, name, args, json);
    free(args);
    return rc;
}

int cmd_tool_session(tny_ctx *ctx, const char *verb, const char *name, const char *args,
                     bool json) {
    perm_engine *perm = perm_new(ctx);
    if (!perm) return 1;
    tny_tool_ext_set_stderr(STDERR_FILENO); /* extension prints reach the terminal */
    interrupted = 0;
    struct sigaction sa = {0}, oldint = {0}, oldterm = {0};
    sa.sa_handler = tool_signal;
    sigemptyset(&sa.sa_mask);
    bool have_int = sigaction(SIGINT, &sa, &oldint) == 0;
    bool have_term = sigaction(SIGTERM, &sa, &oldterm) == 0;
    tools_env env = {.ctx = ctx, .perm = perm, .cancelled = tool_cancelled};
    int rc;
    if (strcmp(verb, "list") == 0) rc = tool_list(&env, json);
    else if (!name) rc = usage();
    else if (strcmp(verb, "describe") == 0) rc = tool_describe(&env, name);
    else rc = tool_run(&env, name, args ? args : "{}", json);
    tools_discard_pending_images(&env);
    tny_tool_ext_shutdown();
    if (have_int) sigaction(SIGINT, &oldint, NULL);
    if (have_term) sigaction(SIGTERM, &oldterm, NULL);
    perm_free(perm);
    return rc;
}
