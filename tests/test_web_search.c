/* test_web_search.c — web_search gating and providers (docs/adr/0055).
 *
 * The tool is advertised only when settings name a provider; both the URL
 * and the command template accept {query} and {{query}}; the command
 * provider runs through the terminal tool's local execution path. */
#include "greatest.h"
#include "core/tools.h"
#include "core/config.h"
#include "core/perm.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_home[512], g_ws[600];

static void ensure_env(void) {
    if (g_home[0]) return;
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    snprintf(g_home, sizeof g_home, "%s%stny-websearch-home-XXXXXX", t,
             t[strlen(t) - 1] == '/' ? "" : "/");
    if (!mkdtemp(g_home)) abort();
    setenv("HOME", g_home, 1);
    unsetenv("TNY_PERMISSION_MODE");
    snprintf(g_ws, sizeof g_ws, "%s/ws", g_home);
    mkdir_p(g_ws);
}

static tny_ctx *ctx_with_settings(const char *json) {
    ensure_env();
    char path[600];
    snprintf(path, sizeof path, "%s/.tny", g_home);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/.tny/settings.json", g_home);
    file_write_atomic(path, json, strlen(json));
    return tny_ctx_load(g_ws);
}

/* true when the advertised schema names `tool` */
static bool schema_has(tools_env *env, const char *tool) {
    char *schema = tools_schema_json(env);
    if (!schema) return false;
    yyjson_doc *doc = jparse(schema, strlen(schema));
    bool found = false;
    if (doc) {
        yyjson_val *item;
        size_t idx, max;
        yyjson_arr_foreach(yyjson_doc_get_root(doc), idx, max, item) {
            const char *name = jget_str(jget(item, "function"), "name");
            if (name && strcmp(name, tool) == 0) found = true;
        }
    }
    yyjson_doc_free(doc);
    free(schema);
    return found;
}

/* No provider: web_search leaves the schema (web_fetch stays), but a direct
 * call still gets the runtime error instead of an "unavailable" refusal. */
TEST schema_omits_web_search_when_unconfigured(void) {
    tny_ctx *ctx = ctx_with_settings("{}");
    ASSERT(ctx);
    perm_engine *perm = perm_new(ctx);
    tools_env env = {.ctx = ctx, .perm = perm};
    ASSERT_FALSE(tool_web_search_configured(ctx));
    ASSERT_FALSE(schema_has(&env, "web_search"));
    ASSERT(schema_has(&env, "web_fetch"));
    ASSERT(schema_has(&env, "terminal"));
    char *r = tools_execute(&env, "web_search", "{\"query\":\"x\"}");
    ASSERT(r);
    ASSERT(strstr(r, "no web search provider configured"));
    ASSERT(strstr(r, "web_search_command"));
    ASSERT_EQ(NULL, strstr(r, "unavailable"));
    free(r);
    perm_free(perm);
    tny_ctx_free(ctx);
    PASS();
}

TEST schema_includes_web_search_with_url_provider(void) {
    tny_ctx *ctx = ctx_with_settings("{\"web_search_url\":\"https://x.test/?q={query}\"}");
    ASSERT(ctx);
    perm_engine *perm = perm_new(ctx);
    tools_env env = {.ctx = ctx, .perm = perm};
    ASSERT(tool_web_search_configured(ctx));
    ASSERT(schema_has(&env, "web_search"));
    perm_free(perm);
    tny_ctx_free(ctx);
    PASS();
}

TEST schema_includes_web_search_with_command_provider(void) {
    tny_ctx *ctx = ctx_with_settings("{\"web_search_command\":\"echo {{query}}\"}");
    ASSERT(ctx);
    perm_engine *perm = perm_new(ctx);
    tools_env env = {.ctx = ctx, .perm = perm};
    ASSERT(tool_web_search_configured(ctx));
    ASSERT(schema_has(&env, "web_search"));
    /* the rest of the schema is untouched by the gate */
    ASSERT(schema_has(&env, "terminal"));
    ASSERT(schema_has(&env, "subagent"));
    perm_free(perm);
    tny_ctx_free(ctx);
    PASS();
}

/* Both spellings, every occurrence, percent-encoded (safe in a URL and in a
 * shell word); a lone brace or an unknown placeholder passes through. */
TEST placeholder_expands_both_spellings(void) {
    char *s = tool_web_search_expand("https://s.test/?q={query}&source=web", "a b&c");
    ASSERT_STR_EQ("https://s.test/?q=a%20b%26c&source=web", s);
    free(s);
    s = tool_web_search_expand("fetch \"https://s.test/?q={{query}}\" --x {query}", "it's");
    ASSERT_STR_EQ("fetch \"https://s.test/?q=it%27s\" --x it%27s", s);
    free(s);
    s = tool_web_search_expand("no placeholder {other} {", "q");
    ASSERT_STR_EQ("no placeholder {other} {", s);
    free(s);
    s = tool_web_search_expand("{{query}}", "A-z_0.9");
    ASSERT_STR_EQ("A-z_0.9", s);
    free(s);
    s = tool_web_search_expand("{{{query}}}", "x");
    ASSERT_STR_EQ("{x}", s);
    free(s);
    PASS();
}

/* The command provider substitutes the query and runs the command locally
 * through the terminal path: exit code and bounded output come back. */
TEST command_provider_runs_fake_command(void) {
    tny_ctx *ctx =
        ctx_with_settings("{\"web_search_command\":\"printf 'result:%s' \\\"{{query}}\\\"\"}");
    ASSERT(ctx);
    perm_engine *perm = perm_new(ctx);
    tools_env env = {.ctx = ctx, .perm = perm};
    char *r = tools_execute(&env, "web_search", "{\"query\":\"tny harness\"}");
    ASSERT(r);
    ASSERT(strstr(r, "exit code: 0"));
    ASSERT(strstr(r, "result:tny%20harness"));
    free(r);
    /* shell metacharacters in the query never reach the shell unencoded */
    r = tools_execute(&env, "web_search", "{\"query\":\"$(touch pwned); `id`\"}");
    ASSERT(r);
    ASSERT(strstr(r, "result:%24%28touch%20pwned%29%3B%20%60id%60"));
    free(r);
    char probe[700];
    snprintf(probe, sizeof probe, "%s/pwned", g_ws);
    ASSERT_FALSE(file_exists(probe));
    perm_free(perm);
    tny_ctx_free(ctx);
    PASS();
}

/* When both keys are set the command provider wins; a failing command
 * reports its exit code rather than falling back to the URL. */
TEST command_provider_beats_url_provider(void) {
    tny_ctx *ctx = ctx_with_settings("{\"web_search_url\":\"http://127.0.0.1:9/?q={query}\","
                                     "\"web_search_command\":\"echo cmd:{query}; exit 3\"}");
    ASSERT(ctx);
    perm_engine *perm = perm_new(ctx);
    tools_env env = {.ctx = ctx, .perm = perm};
    char *r = tools_execute(&env, "web_search", "{\"query\":\"z\"}");
    ASSERT(r);
    ASSERT(strstr(r, "cmd:z"));
    ASSERT(strstr(r, "exit code: 3"));
    free(r);
    perm_free(perm);
    tny_ctx_free(ctx);
    PASS();
}

SUITE(web_search_suite) {
    RUN_TEST(schema_omits_web_search_when_unconfigured);
    RUN_TEST(schema_includes_web_search_with_url_provider);
    RUN_TEST(schema_includes_web_search_with_command_provider);
    RUN_TEST(placeholder_expands_both_spellings);
    RUN_TEST(command_provider_runs_fake_command);
    RUN_TEST(command_provider_beats_url_provider);
}
