/* Search routing, logged-out fallback, templates and bounded result parsing. */
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
    char *schema = tools_catalog_json(env);
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

/* Default fallback is available without configuration; malformed and
 * challenged pages are honest errors. */
TEST schema_includes_default_search(void) {
    tny_ctx *ctx = ctx_with_settings("{}");
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};
    ASSERT(tool_web_search_configured(ctx));
    ASSERT(schema_has(&env, "web_search"));
    char *result = tool_web_search_parse_ddg(
        "<a class=\"result__a\" href=\"https://example.com/?x=1&amp;y=2\">A &amp; B</a>");
    ASSERT(result);
    ASSERT(strstr(result, "A & B"));
    ASSERT(strstr(result, "https://example.com/?x=1&y=2"));
    free(result);
    result = tool_web_search_parse_ddg("<form id=\"challenge-form\">");
    ASSERT(str_starts(result, "error:"));
    ASSERT(strstr(result, "challenge"));
    free(result);
    result = tool_web_search_parse_ddg("unrecognized page");
    ASSERT(str_starts(result, "error:"));
    free(result);
    result = tool_web_search_parse_ddg("<div class=\"no-results\">No results found</div>");
    ASSERT_STR_EQ("DuckDuckGo: no results found for this query.", result);
    free(result);
    result =
        tool_web_search_parse_ddg("<a class=\"result__a\" href=\"javascript:void(0)\">Bad</a>");
    ASSERT(str_starts(result, "error:"));
    free(result);
    result = tool_web_search_parse_ddg("<a class=\"result__a\" "
                                       "href=\"//duckduckgo.com/l/"
                                       "?uddg=https%3A%2F%2Fexample.com%2Fcaptcha%3Fa%3D1%26b%3D2&"
                                       "amp;rut=tracking\">captcha reference</a>");
    ASSERT(result);
    ASSERT(strstr(result, "https://example.com/captcha?a=1&b=2"));
    ASSERT_FALSE(strstr(result, "rut="));
    ASSERT_FALSE(str_starts(result, "error:"));
    free(result);
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

static void restore_env(const char *name, char *old) {
    if (old) setenv(name, old, 1);
    else unsetenv(name);
    free(old);
}

TEST codex_search_uses_subscription_login_not_active_provider_key(void) {
    tny_ctx *ctx = ctx_with_settings("{}");
    ASSERT(ctx);
    const char *value = getenv("CHATGPT_ACCESS_TOKEN");
    char *token = value ? xstrdup(value) : NULL;
    value = getenv("CHATGPT_ACCOUNT_ID");
    char *account = value ? xstrdup(value) : NULL;
    value = getenv("CODEX_HOME");
    char *codex_home = value ? xstrdup(value) : NULL;
    unsetenv("CHATGPT_ACCESS_TOKEN");
    unsetenv("CHATGPT_ACCOUNT_ID");
    setenv("CODEX_HOME", g_home, 1);
    tools_env env = {.ctx = ctx};
    bool handled = true;
    char *result = tool_web_search_codex(&env, "fixture", &handled);
    ASSERT_FALSE(handled);
    ASSERT_FALSE(result);
    char path[600];
    snprintf(path, sizeof path, "%s/auth.json", g_home);
    const char *apikey = "{\"auth_mode\":\"apikey\",\"OPENAI_API_KEY\":\"fixture-api-only\"}";
    ASSERT_EQ(0, file_write_atomic(path, apikey, strlen(apikey)));
    result = tool_web_search_codex(&env, "fixture", &handled);
    ASSERT(handled);
    ASSERT(result);
    free(result);
    ASSERT_EQ(0, file_write_atomic(path, "bad-json", 8));
    result = tool_web_search_codex(&env, "fixture", &handled);
    ASSERT(handled);
    ASSERT(result && strstr(result, "login is unreadable"));
    free(result);
    setenv("CHATGPT_ACCESS_TOKEN", "fixture-token-no-account", 1);
    result = tool_web_search_codex(&env, "fixture", &handled);
    ASSERT(handled);
    ASSERT(result && strstr(result, "invalid Codex login"));
    free(result);
    setenv("CHATGPT_ACCOUNT_ID", "fixture-account", 1);
    setenv("CHATGPT_ACCESS_TOKEN", "fixture\r\nInjection: bad", 1);
    result = tool_web_search_codex(&env, "fixture", &handled);
    ASSERT(handled);
    ASSERT(result && strstr(result, "invalid Codex login"));
    free(result);
    ASSERT_EQ(0, unlink(path));
    restore_env("CHATGPT_ACCESS_TOKEN", token);
    restore_env("CHATGPT_ACCOUNT_ID", account);
    restore_env("CODEX_HOME", codex_home);
    tny_ctx_free(ctx);
    PASS();
}

static bool search_already_cancelled(void *ud) {
    (void)ud;
    return true;
}

TEST codex_search_cancelled_before_auth_is_not_fallback(void) {
    tny_ctx *ctx = ctx_with_settings("{}");
    tools_env env = {.ctx = ctx, .cancelled = search_already_cancelled};
    bool handled = false;
    char *result = tool_web_search_codex(&env, "fixture", &handled);
    ASSERT(handled);
    ASSERT(result && strstr(result, "interrupted"));
    free(result);
    result = tool_web_search_codex(&env, "", &handled);
    ASSERT(handled);
    ASSERT(result && str_starts(result, "error:"));
    free(result);
    tny_ctx_free(ctx);
    PASS();
}

SUITE(web_search_suite) {
    RUN_TEST(codex_search_uses_subscription_login_not_active_provider_key);
    RUN_TEST(codex_search_cancelled_before_auth_is_not_fallback);
    RUN_TEST(schema_includes_default_search);
    RUN_TEST(schema_includes_web_search_with_url_provider);
    RUN_TEST(schema_includes_web_search_with_command_provider);
    RUN_TEST(placeholder_expands_both_spellings);
    RUN_TEST(command_provider_runs_fake_command);
    RUN_TEST(command_provider_beats_url_provider);
}
