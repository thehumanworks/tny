/* Standalone web operations share the native tools and permission identities. */
#include "cli/cli.h"
#include "core/tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_web(tny_ctx *ctx, const cli_globals *g, int argc, char **argv) {
    bool json = g->json;
    const char *verb = argc > 0 ? argv[0] : "";
    const char *name = strcmp(verb, "search") == 0  ? "web_search"
                       : strcmp(verb, "fetch") == 0 ? "web_fetch"
                                                    : NULL;
    if (!name || argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--json") != 0)) {
        fputs("tny: usage: tny web search|fetch TEXT [--json]\n", stderr);
        return 1;
    }
    if (argc == 3) json = true;
    perm_engine *perm = perm_new(ctx);
    if (!perm) return 1;
    if (perm_check(perm, name, argv[1]) != PERM_ALLOW) {
        perm_free(perm);
        fputs("tny: web operation requires permission\n", stderr);
        return 2;
    }
    buf_t args;
    buf_init(&args);
    buf_appendf(&args, "{\"%s\":", strcmp(verb, "search") == 0 ? "query" : "url");
    jescape(&args, argv[1]);
    buf_appends(&args, "}");
    yyjson_doc *d = jparse(args.data, args.len);
    tools_env env = {.ctx = ctx, .perm = perm};
    bool handled = false;
    char *result = d ? tool_web_execute(&env, name, yyjson_doc_get_root(d), &handled) : NULL;
    int code = !result || str_starts(result, "error:") ? 2 : 0;
    if (json) {
        buf_t out;
        buf_init(&out);
        buf_appendf(&out, "{\"kind\":\"web\",\"ok\":%s,\"result\":", code ? "false" : "true");
        jescape(&out, result ? result : "error: web operation failed");
        buf_appends(&out, "}\n");
        fputs(out.data, stdout);
        buf_free(&out);
    } else fprintf(code ? stderr : stdout, "%s\n", result ? result : "error: web operation failed");
    free(result);
    yyjson_doc_free(d);
    buf_free(&args);
    perm_free(perm);
    return code;
}
