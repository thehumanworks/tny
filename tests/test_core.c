/* test_core.c — permission rules, sessions, compaction. Uses a throwaway
 * $HOME so nothing touches the real ~/.tny. */
#include "greatest.h"
#include "core/config.h"
#include "core/backend.h"
#include "core/perm.h"
#include "core/session.h"
#include "core/tasks.h"
#include "core/tools.h"
#include "core/image.h"
#include "lib/custom_tools.h"
#include "backends/codex/codex.h"
#include "backends/openai/openai.h"
#include "backends/cursor/cursor.h"
#include "cli/cli.h"
#include "util/util.h"
#include "tny/tny.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* MSYS2 on Windows refuses symlink(2) unless the user holds
 * SeCreateSymbolicLinkPrivilege (Developer Mode). The snapshot guard below is
 * POSIX symlink semantics, so probe the host once instead of asserting a
 * capability it does not have. */
static bool symlinks_supported(void) {
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char dir[600];
    snprintf(dir, sizeof dir, "%s/tny-symlink-probe-XXXXXX", tmp);
    if (!mkdtemp(dir)) {
        cached = 0;
        return false;
    }
    char link[640];
    snprintf(link, sizeof link, "%s/link", dir);
    cached = symlink("target", link) == 0;
    unlink(link);
    rmdir(dir);
    return cached != 0;
}

static char g_home[512], g_ws[520];

/* Drop every *_BASE_URL from the environment: the host shell (CI, dev
 * boxes) may carry pairs that would register as env-defined providers and
 * flip the default-resolution assertions. */
static void clear_env_providers(void) {
    for (;;) {
        int n = 0;
        char **v = tny_env_provider_names(&n);
        if (!v) return;
        for (int i = 0; i < n; i++) {
            char *var = tny_provider_env_var(v[i], "_BASE_URL");
            if (var) unsetenv(var);
            free(var);
            free(v[i]);
        }
        free(v);
        if (n == 0) return;
    }
}

static void ensure_env(void) {
    if (g_home[0]) return;
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    snprintf(g_home, sizeof g_home, "%s/tny-test-home-XXXXXX", t);
    if (!mkdtemp(g_home)) abort();
    setenv("HOME", g_home, 1);
    unsetenv("TNY_PERMISSION_MODE");
    unsetenv("OPENAI_BASE_URL");
    unsetenv("OPENAI_API_KEY");
    /* builtin-profile credentials from the host shell must not leak in */
    unsetenv("CLAUDE_CODE_OAUTH_TOKEN");
    unsetenv("ANTHROPIC_API_KEY");
    unsetenv("CLAUDE_CONFIG_DIR");
    unsetenv("XAI_API_KEY");
    clear_env_providers();
    snprintf(g_ws, sizeof g_ws, "%s/ws", g_home);
    mkdir_p(g_ws);
}

static void write_settings(const char *json) {
    char path[600];
    snprintf(path, sizeof path, "%s/.tny", g_home);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/.tny/settings.json", g_home);
    file_write_atomic(path, json, strlen(json));
}

static bool tool_schema_has(tools_env *env, const char *wanted) {
    char *json = tools_schema_json(env);
    yyjson_doc *doc = json ? jparse(json, strlen(json)) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool found = false;
    if (root) {
        size_t idx, max;
        yyjson_val *item;
        yyjson_arr_foreach(root, idx, max, item) {
            const char *name = jget_str(jget(item, "function"), "name");
            if (name && strcmp(name, wanted) == 0) found = true;
        }
    }
    yyjson_doc_free(doc);
    free(json);
    return found;
}

static size_t tool_schema_count(tools_env *env) {
    char *json = tools_schema_json(env);
    yyjson_doc *doc = json ? jparse(json, strlen(json)) : NULL;
    size_t count = doc ? yyjson_arr_size(yyjson_doc_get_root(doc)) : 0;
    yyjson_doc_free(doc);
    free(json);
    return count;
}

static tny_bytes test_tool_bytes(const char *s) { return (tny_bytes){s, strlen(s)}; }

static int32_t TNY_CALL profile_custom_tool(void *user_data, tny_tool_call *call,
                                            uint64_t generation, tny_bytes arguments_json,
                                            tny_tool_result_v1 *result) {
    (void)user_data;
    (void)call;
    (void)generation;
    (void)arguments_json;
    memset(result, 0, sizeof *result);
    result->abi_version = TNY_TOOL_RESULT_ABI_VERSION;
    result->struct_size = sizeof *result;
    result->data = test_tool_bytes("custom ok");
    return TNY_TOOL_INVOKE_SYNC;
}

static tny_tool_registration *register_profile_custom(custom_tool_registry *registry) {
    tny_tool_spec_v1 spec = {0};
    spec.abi_version = TNY_TOOL_SPEC_ABI_VERSION;
    spec.struct_size = sizeof spec;
    spec.name = test_tool_bytes("custom_profile_tool");
    spec.description = test_tool_bytes("profile test");
    spec.input_schema_json = test_tool_bytes("{\"type\":\"object\",\"properties\":{}}");
    spec.sensitivity = TNY_TOOL_SENSITIVITY_SAFE;
    spec.invoke = profile_custom_tool;
    tny_tool_registration *registration = NULL;
    return custom_tools_register(registry, NULL, &spec, &registration) == TNY_STATUS_OK
               ? registration
               : NULL;
}

static tny_perm_decision profile_prompt(const char *tool, const char *summary, void *ud) {
    (void)tool;
    (void)summary;
    (void)ud;
    return TNY_PERM_DECISION_ALLOW;
}

/* ---- permissions ---- */

/* The out-of-the-box mode is yolo for every provider (docs/adr/0001):
 * fresh settings, no env, no flags -> nothing ever prompts. */
TEST perm_defaults_to_yolo(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT(ctx);
    ASSERT_EQ(TNY_MODE_YOLO, ctx->perm_mode);
    perm_engine *p = perm_new(ctx);
    ASSERT_EQ(PERM_ALLOW, perm_check(p, "terminal", "echo hi"));
    ASSERT_EQ(PERM_ALLOW, perm_check(p, "write_file", "/somewhere/else.txt"));
    perm_free(p);
    tny_ctx_free(ctx);
    PASS();
}

/* An explicit opt-in ("ask" in settings, env, or --permission-mode) still
 * restores the strict native-loop gate. */
TEST perm_ask_mode_opt_in(void) {
    ensure_env();
    write_settings("{\"permission_mode\":\"ask\"}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT(ctx);
    ASSERT_EQ(TNY_MODE_ASK, ctx->perm_mode);
    perm_engine *p = perm_new(ctx);

    ASSERT(perm_tool_is_safe("read_file"));
    ASSERT_FALSE(perm_tool_is_safe("terminal"));
    ASSERT_EQ(PERM_ALLOW, perm_check(p, "read_file", "relative/path.c"));
    ASSERT_EQ(PERM_PROMPT, perm_check(p, "read_file", "/etc/passwd"));
    ASSERT_EQ(PERM_PROMPT, perm_check(p, "terminal", "echo hi"));
    ASSERT_EQ(PERM_PROMPT, perm_check(p, "write_file", "/somewhere/else.txt"));

    perm_free(p);
    tny_ctx_free(ctx);
    PASS();
}

/* Every spelling of the override must land on the right mode: the settings
 * key (global and per-workspace) and TNY_PERMISSION_MODE. */
TEST perm_mode_overrides_parse(void) {
    ensure_env();

    write_settings("{\"permission_mode\":\"auto\"}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_MODE_AUTO, ctx->perm_mode);
    tny_ctx_free(ctx);

    write_settings("{\"permission_mode\":\"yolo\"}");
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_MODE_YOLO, ctx->perm_mode);
    tny_ctx_free(ctx);

    write_settings("{\"permission_mode\":\"garbage\"}"); /* unknown -> default */
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_MODE_YOLO, ctx->perm_mode);
    tny_ctx_free(ctx);

    /* the workspace-level key beats the global one */
    write_settings("{\"permission_mode\":\"yolo\"}");
    ctx = tny_ctx_load(g_ws);
    buf_t s;
    buf_init(&s);
    buf_appendf(&s,
                "{\"permission_mode\":\"yolo\","
                "\"workspaces\":{\"%s\":{\"permission_mode\":\"ask\"}}}",
                ctx->cwd);
    tny_ctx_free(ctx);
    write_settings(s.data);
    buf_free(&s);
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_MODE_ASK, ctx->perm_mode);
    tny_ctx_free(ctx);

    /* the env var beats settings, for all three values */
    write_settings("{\"permission_mode\":\"auto\"}");
    setenv("TNY_PERMISSION_MODE", "ask", 1);
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_MODE_ASK, ctx->perm_mode);
    tny_ctx_free(ctx);
    setenv("TNY_PERMISSION_MODE", "yolo", 1);
    write_settings("{\"permission_mode\":\"ask\"}");
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_MODE_YOLO, ctx->perm_mode);
    tny_ctx_free(ctx);
    setenv("TNY_PERMISSION_MODE", "auto", 1);
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_MODE_AUTO, ctx->perm_mode);
    tny_ctx_free(ctx);
    setenv("TNY_PERMISSION_MODE", "nonsense", 1); /* ignored, settings win */
    write_settings("{\"permission_mode\":\"ask\"}");
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_MODE_ASK, ctx->perm_mode);
    tny_ctx_free(ctx);

    unsetenv("TNY_PERMISSION_MODE");
    write_settings("{}");
    PASS();
}

/* Canonical levels map onto each provider's wire vocabulary; anything else
 * (a value the provider's catalog advertises) passes through verbatim. */
TEST effort_wire_mapping(void) {
    /* the canonical set is recognized, junk is not */
    const char *levels[] = {"off", "light", "medium", "high", "xhigh", "max"};
    for (size_t i = 0; i < sizeof levels / sizeof *levels; i++)
        ASSERT(tny_effort_canonical(levels[i]));
    ASSERT_FALSE(tny_effort_canonical("ultra"));
    ASSERT_FALSE(tny_effort_canonical(""));
    ASSERT_FALSE(tny_effort_canonical(NULL));

    /* off and light translate; medium/high/xhigh are shared spellings */
    ASSERT_STR_EQ("none", tny_effort_wire(TNY_BK_CODEX, "off"));
    ASSERT_STR_EQ("none", tny_effort_wire(TNY_BK_OPENAI, "off"));
    ASSERT_STR_EQ("low", tny_effort_wire(TNY_BK_CODEX, "light"));
    ASSERT_STR_EQ("low", tny_effort_wire(TNY_BK_CURSOR, "light"));
    ASSERT_STR_EQ("medium", tny_effort_wire(TNY_BK_OPENAI, "medium"));
    ASSERT_STR_EQ("high", tny_effort_wire(TNY_BK_CODEX, "high"));
    ASSERT_STR_EQ("xhigh", tny_effort_wire(TNY_BK_CURSOR, "xhigh"));

    /* "max" exists on codex/cursor but not in the OpenAI API: clamp there */
    ASSERT_STR_EQ("max", tny_effort_wire(TNY_BK_CODEX, "max"));
    ASSERT_STR_EQ("max", tny_effort_wire(TNY_BK_CURSOR, "max"));
    ASSERT_STR_EQ("xhigh", tny_effort_wire(TNY_BK_OPENAI, "max"));

    /* provider-advertised tokens pass through untouched, every backend */
    ASSERT_STR_EQ("ultra", tny_effort_wire(TNY_BK_CODEX, "ultra"));
    ASSERT_STR_EQ("minimal", tny_effort_wire(TNY_BK_OPENAI, "minimal"));
    ASSERT_STR_EQ("whatever", tny_effort_wire(TNY_BK_ACP, "whatever"));
    PASS();
}

/* TNY_REASONING_EFFORT seeds the ctx; unset leaves the provider default. */
TEST effort_env_loads(void) {
    ensure_env();
    write_settings("{}");
    unsetenv("TNY_REASONING_EFFORT");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(NULL, ctx->reasoning_effort);
    tny_ctx_free(ctx);

    setenv("TNY_REASONING_EFFORT", "xhigh", 1);
    ctx = tny_ctx_load(g_ws);
    ASSERT(ctx->reasoning_effort);
    ASSERT_STR_EQ("xhigh", ctx->reasoning_effort);
    tny_ctx_free(ctx);
    unsetenv("TNY_REASONING_EFFORT");
    PASS();
}

/* settings.json can carry a default effort — one string for every provider
 * or a per-provider object like "models" (docs/adr/0015). */
TEST effort_settings_global_default(void) {
    ensure_env();
    unsetenv("TNY_REASONING_EFFORT");
    write_settings("{\"effort\":\"high\",\"openai\":{\"base_url\":\"http://x/v1\"}}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(NULL, ctx->reasoning_effort); /* not before the provider resolves */
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "openai"));
    ASSERT(ctx->reasoning_effort);
    ASSERT_STR_EQ("high", ctx->reasoning_effort);
    tny_ctx_free(ctx);

    /* "default" and empty entries mean provider default (unset) */
    write_settings("{\"effort\":\"default\"}");
    ctx = tny_ctx_load(g_ws);
    tny_resolve_backend(ctx, "openai");
    ASSERT_EQ(NULL, ctx->reasoning_effort);
    tny_ctx_free(ctx);
    PASS();
}

TEST effort_settings_per_provider(void) {
    ensure_env();
    unsetenv("TNY_REASONING_EFFORT");
    write_settings("{\"effort\":{\"openai\":\"light\",\"codex\":\"xhigh\"}}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    tny_resolve_backend(ctx, "openai");
    ASSERT_STR_EQ("light", ctx->reasoning_effort);
    /* switching provider recomputes: codex gets its own entry, and a
     * provider with no entry falls back to unset */
    tny_resolve_backend(ctx, "codex");
    ASSERT_STR_EQ("xhigh", ctx->reasoning_effort);
    tny_resolve_backend(ctx, "cursor");
    ASSERT_EQ(NULL, ctx->reasoning_effort);
    tny_ctx_free(ctx);
    PASS();
}

TEST effort_env_beats_settings(void) {
    ensure_env();
    write_settings("{\"effort\":\"light\"}");
    setenv("TNY_REASONING_EFFORT", "xhigh", 1);
    tny_ctx *ctx = tny_ctx_load(g_ws);
    tny_resolve_backend(ctx, "openai");
    ASSERT_STR_EQ("xhigh", ctx->reasoning_effort);
    tny_ctx_free(ctx);
    unsetenv("TNY_REASONING_EFFORT");
    PASS();
}

TEST effort_flag_beats_settings(void) {
    ensure_env();
    unsetenv("TNY_REASONING_EFFORT");
    write_settings("{\"effort\":\"light\"}");
    cli_globals g = {0};
    g.cwd = g_ws;
    g.backend = "openai";
    g.effort = "max";
    tny_ctx *ctx = cli_make_ctx(&g);
    ASSERT(ctx);
    ASSERT_STR_EQ("max", ctx->reasoning_effort);
    tny_ctx_free(ctx);

    /* --effort default clears the settings default too, and an explicit
     * choice survives a later provider re-resolve (TUI /provider) */
    g.effort = "default";
    ctx = cli_make_ctx(&g);
    ASSERT(ctx);
    ASSERT_EQ(NULL, ctx->reasoning_effort);
    tny_resolve_backend(ctx, "codex");
    ASSERT_EQ(NULL, ctx->reasoning_effort);
    tny_ctx_free(ctx);
    PASS();
}

TEST perm_safe_tool_inside_workspace(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ctx->perm_mode = TNY_MODE_ASK; /* the rule engine under test */
    perm_engine *p = perm_new(ctx);
    char *inside = path_join(ctx->cwd, "src/main.c");
    ASSERT_EQ(PERM_ALLOW, perm_check(p, "read_file", inside));
    free(inside);
    perm_free(p);
    tny_ctx_free(ctx);
    PASS();
}

TEST perm_rules_last_match_wins(void) {
    ensure_env();
    write_settings("{\"permission\":{\"bash\":{"
                   "\"git *\":\"allow\",\"git push*\":\"deny\"}}}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ctx->perm_mode = TNY_MODE_ASK; /* the rule engine under test */
    perm_engine *p = perm_new(ctx);
    ASSERT_EQ(PERM_ALLOW, perm_check(p, "terminal", "git pull --rebase"));
    ASSERT_EQ(PERM_DENY, perm_check(p, "terminal", "git push origin main"));
    ASSERT_EQ(PERM_PROMPT, perm_check(p, "terminal", "ls -la"));
    perm_free(p);
    tny_ctx_free(ctx);
    PASS();
}

TEST perm_workspace_beats_global(void) {
    ensure_env();
    /* load once to learn the resolved workspace path used as the map key */
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    buf_t s;
    buf_init(&s);
    buf_appendf(&s,
                "{\"permission\":{\"bash\":{\"git *\":\"deny\"}},"
                "\"workspaces\":{\"%s\":{\"permission\":{\"bash\":{\"git *\":\"allow\"}}}}}",
                ctx->cwd);
    tny_ctx_free(ctx);
    write_settings(s.data);
    buf_free(&s);

    ctx = tny_ctx_load(g_ws);
    ctx->perm_mode = TNY_MODE_ASK; /* the rule engine under test */
    perm_engine *p = perm_new(ctx);
    ASSERT_EQ(PERM_ALLOW, perm_check(p, "terminal", "git pull"));
    perm_free(p);
    tny_ctx_free(ctx);
    PASS();
}

TEST perm_session_grants(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ctx->perm_mode = TNY_MODE_ASK; /* the rule engine under test */
    perm_engine *p = perm_new(ctx);
    ASSERT_EQ(PERM_PROMPT, perm_check(p, "terminal", "npm install"));
    perm_grant(p, "terminal", "npm install --save");
    ASSERT_EQ(1, perm_grant_count(p));
    /* grant covers the same leading program */
    ASSERT_EQ(PERM_ALLOW, perm_check(p, "terminal", "npm run build"));
    ASSERT_EQ(PERM_PROMPT, perm_check(p, "terminal", "yarn build"));
    perm_free(p);
    tny_ctx_free(ctx);
    PASS();
}

TEST perm_auto_mode_heuristics(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ctx->perm_mode = TNY_MODE_AUTO;
    perm_engine *p = perm_new(ctx);
    ASSERT_EQ(PERM_ALLOW, perm_check(p, "terminal", "git status --short"));
    ASSERT_EQ(PERM_ALLOW, perm_check(p, "terminal", "rg TODO src"));
    ASSERT_EQ(PERM_PROMPT, perm_check(p, "terminal", "curl http://example.com"));
    char *inside = path_join(ctx->cwd, "notes.txt");
    ASSERT_EQ(PERM_ALLOW, perm_check(p, "write_file", inside));
    free(inside);
    ASSERT_EQ(PERM_PROMPT, perm_check(p, "write_file", "/etc/hosts"));
    perm_free(p);
    tny_ctx_free(ctx);
    PASS();
}

TEST perm_yolo_allows_everything(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ctx->perm_mode = TNY_MODE_YOLO;
    perm_engine *p = perm_new(ctx);
    ASSERT_EQ(PERM_ALLOW, perm_check(p, "terminal", "make deploy"));
    ASSERT_EQ(PERM_ALLOW, perm_check(p, "delete_file", "/anywhere"));
    perm_free(p);
    tny_ctx_free(ctx);
    PASS();
}

TEST tool_prepare_validates_rewrites_and_complete_permission_subjects(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ctx->perm_mode = TNY_MODE_AUTO;
    perm_engine *perm = perm_new(ctx);
    tny_session_state *session = session_new(ctx);
    tools_env env = {0};
    env.ctx = ctx;
    env.session = session;
    env.perm = perm;
    tools_call call;

    ASSERT_EQ(-1, tools_call_prepare(NULL, "list_files", "{}", &call));
    ASSERT_EQ(-1, tools_call_prepare(&env, NULL, "{}", &call));
    ASSERT_EQ(-1, tools_call_prepare(&env, "list_files", "{}", NULL));

    ASSERT_EQ(-1, tools_call_prepare(&env, "write_file", "{\"path\":\"a.txt\"}", &call));
    ASSERT(call.error && strstr(call.error, "needs argument content"));
    tools_call_free(&call);
    ASSERT_EQ(-1, tools_call_prepare(&env, "write_file", "{\"path\":7,\"content\":\"x\"}", &call));
    ASSERT(call.error && strstr(call.error, "path must be string"));
    tools_call_free(&call);

    ASSERT_EQ(0, tools_call_prepare(&env, "write_file",
                                    "{\"path\":\"inside.txt\",\"content\":\"x\"}", &call));
    ASSERT_EQ(PERM_ALLOW, call.verdict);
    tools_call_free(&call);
    ASSERT_EQ(0, tools_call_prepare(&env, "write_file",
                                    "{\"path\":\"/etc/tny-outside\",\"content\":\"x\"}", &call));
    ASSERT_EQ(PERM_PROMPT, call.verdict);
    ASSERT(call.summary && strstr(call.summary, "write_file /etc/tny-outside"));
    tools_call_free(&call);

    ASSERT_EQ(0, tools_call_prepare(&env, "rename_file",
                                    "{\"path\":\"inside.txt\",\"new_path\":\"/etc/tny-outside\"}",
                                    &call));
    ASSERT(call.detail && path_is_within(ctx->cwd, call.detail));
    ASSERT_STR_EQ("/etc/tny-outside", call.detail2);
    ASSERT_EQ(PERM_PROMPT, call.verdict);
    tools_call_grant(&env, &call);
    ASSERT_EQ(2, perm_grant_count(perm));
    tools_call_free(&call);

    ASSERT_EQ(0, tools_call_prepare(
                     &env, "copy_file",
                     "{\"path\":\"inside.txt\",\"new_path\":\"/etc/tny-copy-outside\"}", &call));
    ASSERT(call.detail && path_is_within(ctx->cwd, call.detail));
    ASSERT_STR_EQ("/etc/tny-copy-outside", call.detail2);
    ASSERT_EQ(PERM_PROMPT, call.verdict);
    tools_call_free(&call);

    ctx->perm_mode = TNY_MODE_ASK;
    ASSERT_EQ(0, tools_call_prepare(&env, "mcp_select_tool",
                                    "{\"server\":\"srv\",\"tool\":\"deploy\",\"arguments\":{}}",
                                    &call));
    ASSERT_STR_EQ("mcp:srv/deploy", call.permission_tool);
    ASSERT_EQ(PERM_PROMPT, call.verdict);
    tools_call_free(&call);

    session_close(session);
    perm_free(perm);
    tny_ctx_free(ctx);

    write_settings("{\"permission\":{\"edit\":{\"*\":\"allow\","
                   "\"*/deny-source\":\"deny\"}}}");
    ctx = tny_ctx_load(g_ws);
    ctx->perm_mode = TNY_MODE_ASK;
    perm = perm_new(ctx);
    session = session_new(ctx);
    env = (tools_env){.ctx = ctx, .session = session, .perm = perm};
    ASSERT_EQ(0, tools_call_prepare(&env, "copy_file",
                                    "{\"path\":\"/etc/deny-source\",\"new_path\":\"inside.txt\"}",
                                    &call));
    ASSERT_EQ(PERM_DENY, call.verdict);
    tools_call_free(&call);
    session_close(session);
    perm_free(perm);
    tny_ctx_free(ctx);
    PASS();
}

/* ---- sessions ---- */

TEST session_roundtrip(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    session_add_text(s, "user", "hello");
    session_add_assistant(s, "hi there", NULL);
    session_set_title(s, "greeting");
    session_set_meta(s, "openai", "mock-model");
    session_set_host_pointer(s, "thread-abc");
    session_add_usage(s, 10, 20);
    session_add_usage(s, 5, 5);
    session_bump_turns(s);
    ASSERT_EQ(0, session_save(s));
    char *id = xstrdup(s->id);
    session_close(s);

    s = session_open(ctx, id);
    ASSERT(s);
    ASSERT_STR_EQ("greeting", session_title(s));
    ASSERT_STR_EQ("thread-abc", session_host_pointer(s));
    ASSERT_EQ_FMT(1, session_turns(s), "%d");
    int64_t in_tok = 0, out_tok = 0;
    session_get_usage(s, &in_tok, &out_tok);
    ASSERT_EQ_FMT((long long)15, (long long)in_tok, "%lld");
    ASSERT_EQ_FMT((long long)25, (long long)out_tok, "%lld");
    session_close(s);

    char *latest = session_latest_id(ctx);
    ASSERT(latest);
    ASSERT_STR_EQ(id, latest);
    free(latest);
    free(id);
    tny_ctx_free(ctx);
    PASS();
}

TEST session_task_snapshot_roundtrip_and_resume_guards(void) {
    ensure_env();
    write_settings("{}");
    const char *body = "Exact private task instructions.\nSecond line.";
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT(ctx);
    ASSERT_EQ(TNY_TASK_OK, tny_task_set_explicit(ctx, "release", body, "project"));
    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    session_bump_turns(s);
    ASSERT_EQ(0, session_save(s));
    char *id = xstrdup(s->id);
    char *public_json = jwrite(s->doc);
    ASSERT(public_json);
    ASSERT(strstr(public_json, "\"task\":{\"name\":\"release\""));
    ASSERT_FALSE(strstr(public_json, "Exact private task instructions"));
    free(public_json);
    char *snapshot_path = path_join(s->dir, "task.md");
    size_t snapshot_len = 0;
    char *snapshot = file_slurp(snapshot_path, &snapshot_len);
    ASSERT(snapshot);
    ASSERT_EQ(strlen(body), snapshot_len);
    ASSERT_STR_EQ(body, snapshot);
    free(snapshot);
    free(snapshot_path);
    session_close(s);
    tny_ctx_free(ctx);

    /* No explicit selector restores the exact saved body and stable source. */
    ctx = tny_ctx_load(g_ws);
    s = session_open(ctx, id);
    ASSERT(s);
    char err[192] = {0};
    ASSERT_EQ(0, session_task_reconcile(s, err, sizeof err));
    ASSERT_STR_EQ("release", ctx->task_name);
    ASSERT_STR_EQ("project", ctx->task_source);
    ASSERT_STR_EQ(body, ctx->task_instructions);
    ASSERT_FALSE(ctx->task_explicit);
    session_close(s);
    tny_ctx_free(ctx);

    /* An explicit selector is accepted only when name and digest match. */
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_TASK_OK, tny_task_set_explicit(ctx, "release", body, "explicit"));
    s = session_open(ctx, id);
    ASSERT(s);
    ASSERT_EQ(0, session_task_reconcile(s, err, sizeof err));
    ASSERT(ctx->task_explicit);
    session_close(s);
    tny_ctx_free(ctx);

    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_TASK_OK,
              tny_task_set_explicit(ctx, "release", "Different instructions", "explicit"));
    s = session_open(ctx, id);
    ASSERT(s);
    ASSERT_EQ(-1, session_task_reconcile(s, err, sizeof err));
    ASSERT(strstr(err, "name and digest"));
    session_close(s);
    tny_ctx_free(ctx);
    free(id);

    /* Old sessions remain loadable, but a task cannot be grafted after a turn. */
    ctx = tny_ctx_load(g_ws);
    s = session_new(ctx);
    ASSERT(s);
    session_bump_turns(s);
    ASSERT_EQ(0, session_save(s));
    id = xstrdup(s->id);
    session_close(s);
    ASSERT_EQ(TNY_TASK_OK, tny_task_set_explicit(ctx, "review", "Review exactly.", "explicit"));
    s = session_open(ctx, id);
    ASSERT(s);
    ASSERT_EQ(-1, session_task_reconcile(s, err, sizeof err));
    ASSERT(strstr(err, "cannot be added"));
    session_close(s);
    tny_ctx_free(ctx);
    free(id);
    PASS();
}

TEST session_task_snapshot_atomic_window_and_symlink_guards(void) {
    if (!symlinks_supported()) SKIP();
    ensure_env();
    write_settings("{}");
    const char *body = "Crash-safe private task body.";
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT(ctx);
    ASSERT_EQ(TNY_TASK_OK, tny_task_set_explicit(ctx, "release", body, "project"));
    tny_session_state *s = session_new(ctx);
    ASSERT(s);
    ASSERT_EQ(0, session_save(s));
    char *id = xstrdup(s->id);
    char *snapshot = path_join(s->dir, "task.md");
    char *pending = path_join(s->dir, "task.md.next");
    ASSERT(id && snapshot && pending);
    ASSERT_EQ(0, rename(snapshot, pending));
    session_close(s);

    /* Metadata is committed before the final rename. The pending sidecar is
     * therefore a valid crash-recovery source, including for recover-copy. */
    char *recovered_id = session_recover_copy(ctx, id);
    ASSERT(recovered_id);
    tny_ctx_free(ctx);
    ctx = tny_ctx_load(g_ws);
    s = session_open(ctx, id);
    ASSERT(s);
    char err[192] = {0};
    ASSERT_EQ(0, session_task_reconcile(s, err, sizeof err));
    ASSERT_STR_EQ(body, ctx->task_instructions);
    ASSERT_EQ(0, session_save(s));
    ASSERT_EQ(0, access(snapshot, F_OK));
    ASSERT_EQ(-1, access(pending, F_OK));
    session_close(s);
    tny_ctx_free(ctx);

    ctx = tny_ctx_load(g_ws);
    s = session_open(ctx, recovered_id);
    ASSERT(s);
    ASSERT_EQ(0, session_task_reconcile(s, err, sizeof err));
    ASSERT_STR_EQ(body, ctx->task_instructions);
    char *recovered_snapshot = path_join(s->dir, "task.md");
    ASSERT(recovered_snapshot);
    session_task_clear(s);
    ASSERT_EQ(0, session_save(s));
    ASSERT_EQ(-1, access(recovered_snapshot, F_OK));
    free(recovered_snapshot);
    session_close(s);
    tny_ctx_free(ctx);

    /* Even a symlink to byte-for-byte matching content is not a snapshot. */
    char *decoy = path_join(g_home, "task-decoy.md");
    ASSERT(decoy);
    ASSERT_EQ(0, file_write_atomic(decoy, body, strlen(body)));
    ASSERT_EQ(0, unlink(snapshot));
    ASSERT_EQ(0, symlink(decoy, snapshot));
    ctx = tny_ctx_load(g_ws);
    s = session_open(ctx, id);
    ASSERT(s);
    ASSERT_EQ(-1, session_task_reconcile(s, err, sizeof err));
    ASSERT(strstr(err, "missing or invalid"));
    session_close(s);
    tny_ctx_free(ctx);

    free(decoy);
    free(pending);
    free(snapshot);
    free(recovered_id);
    free(id);
    PASS();
}

TEST session_tool_argument_rewrite_is_targeted_and_no_match_terminates(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    tny_session_state *s = session_new(ctx);
    session_add_text(s, "user", "prompt");
    session_add_assistant(
        s, NULL,
        "[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{"
        "\"name\":\"list_files\",\"arguments\":\"{\\\"path\\\":\\\"old\\\"}\"}}]");
    char *before = jwrite(s->doc);
    session_replace_tool_arguments(s, "missing", "{\"path\":\"never\"}");
    char *after = jwrite(s->doc);
    ASSERT_STR_EQ(before, after);
    free(before);
    free(after);

    session_replace_tool_arguments(s, "call_1", "{\"path\":\"new\"}");
    after = jwrite(s->doc);
    ASSERT(after && strstr(after, "{\\\"path\\\":\\\"new\\\"}"));
    ASSERT_FALSE(strstr(after, "{\\\"path\\\":\\\"old\\\"}"));
    free(after);
    session_close(s);
    tny_ctx_free(ctx);
    PASS();
}

TEST session_result_handles(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    tny_session_state *s = session_new(ctx);
    char *h = session_store_result(s, "0123456789", 10);
    ASSERT(h);
    size_t n = 0;
    char *slice = session_read_result(s, h, 3, 4, &n);
    ASSERT(slice);
    ASSERT_EQ_FMT((size_t)4, n, "%zu");
    ASSERT_STR_EQ("3456", slice);
    free(slice);
    /* path traversal in a handle must be rejected outright */
    ASSERT_EQ(NULL, session_read_result(s, "../../etc/passwd", 0, 10, &n));
    ASSERT_EQ(NULL, session_read_result(s, "ABCD", 0, 10, &n)); /* not lowercase hex */
    slice = session_read_result(s, h, 100, 10, &n);
    ASSERT(slice);
    ASSERT_EQ_FMT((size_t)0, n, "%zu");
    free(slice);
    free(h);
    session_close(s);
    tny_ctx_free(ctx);
    PASS();
}

TEST session_compaction(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    tny_session_state *s = session_new(ctx);
    for (int i = 0; i < 9; i++) {
        char q[64], a[64];
        snprintf(q, sizeof q, "question %d", i);
        snprintf(a, sizeof a, "answer %d", i);
        session_add_text(s, "user", q);
        session_add_assistant(s, a, NULL);
        session_bump_turns(s);
    }
    const char *summary = NULL;
    ASSERT_EQ_FMT(0, session_compact_boundary(s, &summary), "%d");

    session_compact(s, false); /* 9 turns ≥ 8: keep last 4 verbatim */
    int b = session_compact_boundary(s, &summary);
    ASSERT_EQ_FMT(10, b, "%d"); /* 18 msgs, last 4 user turns start at 10 */
    ASSERT(summary);
    ASSERT(strstr(summary, "user asked") != NULL);
    ASSERT(strstr(summary, "question 0") != NULL);
    ASSERT(strstr(summary, "question 8") == NULL); /* kept verbatim */

    session_compact(s, true); /* force: keep only the latest turn */
    b = session_compact_boundary(s, &summary);
    ASSERT_EQ_FMT(16, b, "%d");
    ASSERT(strstr(summary, "question 7") != NULL);
    session_close(s);
    tny_ctx_free(ctx);
    PASS();
}

TEST session_recovery_roundtrip(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    tny_session_state *s = session_new(ctx);
    session_recovery_write(s, "partial answer text");
    char *r = session_recovery_read(s);
    ASSERT(r);
    ASSERT_STR_EQ("partial answer text", r);
    free(r);
    session_recovery_clear(s);
    ASSERT_EQ(NULL, session_recovery_read(s));
    session_close(s);
    tny_ctx_free(ctx);
    PASS();
}

/* ---- default backend resolution ---- */

static void codex_auth_write(bool present) {
    char path[600];
    snprintf(path, sizeof path, "%s/.codex", g_home);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/.codex/auth.json", g_home);
    if (present) file_write_atomic(path, "{}", 2);
    else unlink(path);
}

TEST backend_default_prefers_codex_login(void) {
    ensure_env();
    unsetenv("CODEX_HOME");
    unsetenv("CURSOR_API_KEY");
    write_settings("{}");

    codex_auth_write(false);
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, NULL)); /* nothing configured */
    tny_ctx_free(ctx);

    codex_auth_write(true);
    ctx = tny_ctx_load(g_ws);
    ASSERT(tny_codex_auth_present());
    ASSERT_EQ(TNY_BK_CODEX, tny_resolve_backend(ctx, NULL)); /* subscription wins */
    ASSERT_EQ(TNY_BK_ACP, tny_resolve_backend(ctx, "acp"));  /* flag beats it */
    tny_ctx_free(ctx);

    setenv("OPENAI_API_KEY", "sk-test", 1); /* explicit key beats detection */
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, NULL));
    unsetenv("OPENAI_API_KEY");
    tny_ctx_free(ctx);

    write_settings("{\"last_backend\":\"openai\"}"); /* remembered choice beats it */
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, NULL));
    tny_ctx_free(ctx);

    write_settings("{}");
    codex_auth_write(false);
    PASS();
}

TEST provider_last_used_and_scoped_models(void) {
    ensure_env();
    write_settings("{}");
    codex_auth_write(true); /* codex detectable, but last-used must win */

    tny_ctx *ctx = tny_ctx_load(g_ws);
    ctx->backend = TNY_BK_CURSOR;
    free(ctx->model);
    ctx->model = xstrdup("grok-4.6");
    ASSERT_EQ(0, tny_settings_remember_use(ctx));
    tny_ctx_free(ctx);

    ctx = tny_ctx_load(g_ws); /* fresh launch: last provider + its model */
    ASSERT_EQ(TNY_BK_CURSOR, tny_resolve_backend(ctx, NULL));
    ASSERT(ctx->model);
    ASSERT_STR_EQ("grok-4.6", ctx->model);
    tny_ctx_free(ctx);

    ctx = tny_ctx_load(g_ws); /* another provider must not inherit the model */
    ASSERT_EQ(TNY_BK_CODEX, tny_resolve_backend(ctx, "codex"));
    ASSERT_EQ(NULL, ctx->model);
    tny_ctx_free(ctx);

    ctx = tny_ctx_load(g_ws); /* --model flag beats the saved entry */
    free(ctx->model);
    ctx->model = xstrdup("flag-model");
    ctx->model_from_flag = true;
    ASSERT_EQ(TNY_BK_CURSOR, tny_resolve_backend(ctx, NULL));
    ASSERT_STR_EQ("flag-model", ctx->model);
    tny_ctx_free(ctx);

    write_settings("{}");
    codex_auth_write(false);
    PASS();
}

/* settings.json may define OpenAI-compatible providers under arbitrary
 * names ("openrouter", "xai", …): any top-level object with a base_url.
 * They resolve to the openai backend but keep their own name, config,
 * key env, and saved model. */
TEST custom_named_provider_profiles(void) {
    ensure_env();
    codex_auth_write(false);
    unsetenv("CURSOR_API_KEY");
    unsetenv("OPENAI_API_KEY");
    unsetenv("OPENAI_BASE_URL");
    write_settings("{\"openrouter\":{\"base_url\":\"https://openrouter.ai/api/v1\","
                   "\"api_key_env\":\"TEST_OR_KEY\",\"model\":\"anthropic/claude-sonnet-4.6\"},"
                   "\"xai\":{\"base_url\":\"https://api.x.ai/v1\"},"
                   "\"az-AZ09\":{\"base_url\":\"https://mixed.test/v1\"},"
                   "\"openai\":{\"base_url\":\"https://example.test/v1\"}}");
    setenv("TEST_OR_KEY", "sk-or-test", 1);
    setenv("XAI_API_KEY", "sk-xai-test", 1);

    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "openrouter"));
    ASSERT_STR_EQ("openrouter", tny_provider_name(ctx));
    ASSERT_STR_EQ("https://openrouter.ai/api/v1", ctx->base_url);
    ASSERT_STR_EQ("sk-or-test", ctx->api_key);
    ASSERT(ctx->model);
    ASSERT_STR_EQ("anthropic/claude-sonnet-4.6", ctx->model);
    ASSERT_EQ(0, tny_settings_remember_use(ctx)); /* saves "openrouter" */
    tny_ctx_free(ctx);

    ctx = tny_ctx_load(g_ws); /* fresh launch: the named profile comes back */
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, NULL));
    ASSERT_STR_EQ("openrouter", tny_provider_name(ctx));
    ASSERT_STR_EQ("https://openrouter.ai/api/v1", ctx->base_url);
    ASSERT_STR_EQ("sk-or-test", ctx->api_key);
    tny_ctx_free(ctx);

    ctx = tny_ctx_load(g_ws); /* no api_key_env: NAME_API_KEY is derived */
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "xai"));
    ASSERT_STR_EQ("xai", tny_provider_name(ctx));
    ASSERT_STR_EQ("https://api.x.ai/v1", ctx->base_url);
    ASSERT_STR_EQ("sk-xai-test", ctx->api_key);
    ASSERT_EQ(NULL, ctx->model); /* openrouter's model must not leak */

    /* switching back to a builtin restores the openai object's config */
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "openai"));
    ASSERT_STR_EQ("openai", tny_provider_name(ctx));
    ASSERT_STR_EQ("https://example.test/v1", ctx->base_url);
    ASSERT_EQ(NULL, ctx->api_key);
    tny_ctx_free(ctx);

    ctx = tny_ctx_load(g_ws); /* a missing profile key resolves to no key */
    unsetenv("XAI_API_KEY");
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "xai"));
    ASSERT_EQ(NULL, ctx->api_key);
    tny_ctx_free(ctx);

    /* an explicit --provider openai must not be hijacked by detection */
    setenv("CURSOR_API_KEY", "key_test", 1);
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "openai"));
    ASSERT_STR_EQ("openai", tny_provider_name(ctx));
    unsetenv("CURSOR_API_KEY");
    tny_ctx_free(ctx);

    ctx = tny_ctx_load(g_ws);
    ASSERT(tny_custom_provider_exists(ctx, "openrouter"));
    /* builtin even though a base_url object with that name exists */
    ASSERT_FALSE(tny_custom_provider_exists(ctx, "openai"));
    ASSERT_FALSE(tny_custom_provider_exists(ctx, "models")); /* no base_url */
    ASSERT_EQ(-1, tny_resolve_backend(ctx, "nope"));         /* unknown still fails */
    char *env = tny_custom_provider_key_env(ctx, "xai");
    ASSERT(env);
    ASSERT_STR_EQ("XAI_API_KEY", env);
    free(env);
    /* every character class in the derived name: lower, upper, digit, other */
    env = tny_custom_provider_key_env(ctx, "az-AZ09");
    ASSERT(env);
    ASSERT_STR_EQ("AZ_AZ09_API_KEY", env);
    free(env);
    tny_ctx_free(ctx);

    unsetenv("TEST_OR_KEY");
    write_settings("{}");
    PASS();
}

/* Named ACP profiles are addressed in the acp@NAME namespace (with the
 * earlier acp.agents/acp: shape kept compatible). Their argv is process-owned
 * (not a yyjson pointer), their model is scoped
 * to the effective provider ID, and switching away drops profile argv. */
TEST settings_general_defaults(void) {
    ensure_env();
    codex_auth_write(false);
    unsetenv("TNY_REASONING_EFFORT");
    write_settings("{\"provider\":\"codex\",\"model\":{\"codex\":\"cfg-model\"},"
                   "\"models\":{\"codex\":\"remembered-model\"},"
                   "\"effort\":\"high\",\"fast\":{\"codex\":true}}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_CODEX, tny_resolve_backend(ctx, NULL));
    ASSERT_STR_EQ("codex", tny_provider_name(ctx));
    ASSERT_STR_EQ("cfg-model", ctx->model);
    ASSERT_STR_EQ("high", ctx->reasoning_effort);
    ASSERT_STR_EQ("fast", ctx->service_tier);
    /* A per-provider fast default must not leak through a TUI provider switch. */
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "openai"));
    ASSERT_EQ(NULL, ctx->service_tier);
    tny_ctx_free(ctx);

    /* Leading flags beat every corresponding settings default. */
    cli_globals g = {0};
    g.cwd = g_ws;
    g.backend = "openai";
    g.model = "flag-model";
    g.effort = "light";
    ctx = cli_make_ctx(&g);
    ASSERT(ctx);
    ASSERT_EQ(TNY_BK_OPENAI, ctx->backend);
    ASSERT_STR_EQ("flag-model", ctx->model);
    ASSERT_STR_EQ("light", ctx->reasoning_effort);
    ASSERT_EQ(NULL, ctx->service_tier); /* codex-only fast object entry */
    tny_ctx_free(ctx);
    write_settings("{}");
    PASS();
}

TEST acp_named_provider_profiles(void) {
    ensure_env();
    codex_auth_write(false);
    unsetenv("CURSOR_API_KEY");
    unsetenv("OPENAI_API_KEY");
    write_settings("{\"acp\":{\"agents\":{"
                   "\"claude\":{\"command\":[\"npx\",\"-y\",\"claude-agent-acp\"],"
                   "\"model\":\"profile-model\"},"
                   "\"gemini\":{\"command\":[\"gemini\",\"--acp\"]}}},"
                   "\"models\":{\"acp:claude\":\"saved-model\"}}");

    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT(tny_acp_profile_exists(ctx, "acp:claude"));
    ASSERT_EQ(TNY_BK_ACP, tny_resolve_backend(ctx, "acp:claude"));
    ASSERT_STR_EQ("acp:claude", tny_provider_name(ctx));
    ASSERT(ctx->agent_from_profile);
    ASSERT_STR_EQ("npx", ctx->agent_argv[0]);
    ASSERT_STR_EQ("-y", ctx->agent_argv[1]);
    ASSERT_STR_EQ("claude-agent-acp", ctx->agent_argv[2]);
    ASSERT_EQ(NULL, ctx->agent_argv[3]);
    ASSERT_STR_EQ("saved-model", ctx->model); /* saved beats profile */

    /* Rewriting settings frees and reparses its yyjson doc. The copied argv
     * remains valid, proving no document-storage pointer escaped. */
    ASSERT_EQ(0, tny_settings_set_str(ctx, "marker", "reparsed"));
    ASSERT_STR_EQ("claude-agent-acp", ctx->agent_argv[2]);

    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "openai"));
    ASSERT_EQ(NULL, ctx->agent_argv); /* profile argv does not leak */
    ASSERT_FALSE(ctx->agent_from_profile);
    ASSERT_EQ(NULL, ctx->model); /* ACP model does not leak either */

    ASSERT_EQ(TNY_BK_ACP, tny_resolve_backend(ctx, "acp:gemini"));
    ASSERT_STR_EQ("gemini", ctx->agent_argv[0]);
    ASSERT_STR_EQ("--acp", ctx->agent_argv[1]);
    ASSERT_EQ(NULL, ctx->model); /* no saved/profile model -> agent default */
    tny_ctx_free(ctx);

    /* A previously used namespaced profile is restored like every other
     * effective provider; defining the profile alone is not auto-selection. */
    write_settings("{\"last_provider\":\"acp:claude\",\"acp\":{\"agents\":{"
                   "\"claude\":{\"command\":[\"claude-agent-acp\"]}}}}");
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_ACP, tny_resolve_backend(ctx, NULL));
    ASSERT_STR_EQ("acp:claude", tny_provider_name(ctx));
    ASSERT_STR_EQ("claude-agent-acp", ctx->agent_argv[0]);
    tny_ctx_free(ctx);
    /* Preferred shape + selector: command string and separate args array. */
    write_settings("{\"acp\":{\"claude\":{\"command\":\"npx\","
                   "\"args\":[\"-y\",\"@agentclientprotocol/claude-agent-acp\"]},"
                   "\"pi\":{\"command\":\"pi-acp\"}}}");
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_ACP, tny_resolve_backend(ctx, "acp@claude"));
    ASSERT_STR_EQ("acp@claude", tny_provider_name(ctx));
    ASSERT_STR_EQ("npx", ctx->agent_argv[0]);
    ASSERT_STR_EQ("-y", ctx->agent_argv[1]);
    ASSERT_STR_EQ("@agentclientprotocol/claude-agent-acp", ctx->agent_argv[2]);
    ASSERT_EQ(NULL, ctx->agent_argv[3]);
    tny_ctx_free(ctx);
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_ACP, tny_resolve_backend(ctx, "acp@pi"));
    ASSERT_STR_EQ("pi-acp", ctx->agent_argv[0]);
    ASSERT_EQ(NULL, ctx->agent_argv[1]);
    tny_ctx_free(ctx);

    write_settings("{}");
    PASS();
}

TEST acp_profile_model_precedence(void) {
    ensure_env();
    setenv("ACP_A_DEFAULT_MODEL", "env-default", 1);
    write_settings("{\"acp\":{\"agents\":{\"a\":{\"command\":[\"agent-a\"],"
                   "\"model\":\"profile\"}}},\"models\":{\"acp:a\":\"saved\"}}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_ACP, tny_resolve_backend(ctx, "acp:a"));
    ASSERT_STR_EQ("saved", ctx->model);
    tny_ctx_free(ctx);

    ctx = tny_ctx_load(g_ws);
    ctx->model = xstrdup("flag");
    ctx->model_from_flag = true;
    ASSERT_EQ(TNY_BK_ACP, tny_resolve_backend(ctx, "acp:a"));
    ASSERT_STR_EQ("flag", ctx->model);
    tny_ctx_free(ctx);

    write_settings("{\"acp\":{\"agents\":{\"a\":{\"command\":[\"agent-a\"],"
                   "\"model\":\"profile\"}}}}");
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_ACP, tny_resolve_backend(ctx, "acp:a"));
    ASSERT_STR_EQ("profile", ctx->model);
    tny_ctx_free(ctx);

    write_settings("{\"acp\":{\"agents\":{\"a\":{\"command\":[\"agent-a\"]}}}}");
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_ACP, tny_resolve_backend(ctx, "acp:a"));
    ASSERT_STR_EQ("env-default", ctx->model);
    tny_ctx_free(ctx);
    unsetenv("ACP_A_DEFAULT_MODEL");
    write_settings("{}");
    PASS();
}

TEST acp_profiles_validate_when_selected(void) {
    ensure_env();
    write_settings("{\"acp\":{\"agents\":{"
                   "\"bad name\":{\"command\":[\"x\"]},"
                   "\"missing\":{},\"not_array\":{\"command\":7},"
                   "\"empty\":{\"command\":[]},"
                   "\"non_string\":{\"command\":[\"x\",7]},"
                   "\"empty_arg\":{\"command\":[\"x\",\"\"]},"
                   "\"remote_args\":{\"command\":[\"wss://agent.test/acp\",\"extra\"]},"
                   "\"bad_model\":{\"command\":[\"x\"],\"model\":7}}}}");
    const char *bad[] = {
        "acp:",      "acp:unknown",    "acp:bad name",  "acp:missing",     "acp:not_array",
        "acp:empty", "acp:non_string", "acp:empty_arg", "acp:remote_args", "acp:bad_model"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        tny_ctx *ctx = tny_ctx_load(g_ws);
        ASSERT_EQ(-1, tny_resolve_backend(ctx, bad[i]));
        tny_ctx_free(ctx);
    }

    /* Pin every inclusive boundary in the profile-name alphabet. */
    write_settings("{\"acp\":{\"agents\":{"
                   "\"a\":{\"command\":[\"x\"]},\"z\":{\"command\":[\"x\"]},"
                   "\"A\":{\"command\":[\"x\"]},\"Z\":{\"command\":[\"x\"]},"
                   "\"0\":{\"command\":[\"x\"]},\"9\":{\"command\":[\"x\"]},"
                   "\"-\":{\"command\":[\"x\"]},\"_\":{\"command\":[\"x\"]}}}}");
    const char *edges[] = {"a", "z", "A", "Z", "0", "9", "-", "_"};
    for (size_t i = 0; i < sizeof edges / sizeof *edges; i++) {
        char provider[8];
        snprintf(provider, sizeof provider, "acp:%s", edges[i]);
        tny_ctx *edge_ctx = tny_ctx_load(g_ws);
        ASSERT_EQ(TNY_BK_ACP, tny_resolve_backend(edge_ctx, provider));
        tny_ctx_free(edge_ctx);
    }

    /* Explicit --agent is the ad-hoc `acp` form, never an override for a
     * named profile. Resolver rejects the ambiguous combination. */
    write_settings("{\"acp\":{\"agents\":{\"named\":{\"command\":[\"profile\"]}}}}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ctx->agent_argv = calloc(2, sizeof *ctx->agent_argv);
    ctx->agent_argv[0] = xstrdup("explicit");
    ASSERT_EQ(-1, tny_resolve_backend(ctx, "acp:named"));
    ASSERT_STR_EQ("explicit", ctx->agent_argv[0]);
    ASSERT_FALSE(ctx->agent_from_profile);
    tny_ctx_free(ctx);
    write_settings("{}");
    PASS();
}

TEST acp_profiles_list_without_auto_select(void) {
    ensure_env();
    codex_auth_write(false);
    unsetenv("CURSOR_API_KEY");
    unsetenv("OPENAI_API_KEY");
    unsetenv("OPENAI_BASE_URL");
    write_settings("{\"acp\":{\"agents\":{\"\":{\"command\":[\"empty\"]},"
                   "\"claude\":{\"command\":[\"claude\"]},"
                   "\"bad name\":{\"command\":[\"bad\"]}}}}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, NULL));
    ASSERT_STR_EQ("openai", tny_provider_name(ctx));
    char *names = tny_provider_names_joined(ctx);
    ASSERT(strstr(names, "|acp@claude") != NULL);
    ASSERT(strstr(names, "acp:bad name") == NULL);
    ASSERT(strstr(names, "|acp:|") == NULL);
    free(names);
    tny_ctx_free(ctx);
    write_settings("{}");
    PASS();
}

/* Providers can also be defined purely by environment variables:
 * NAME_BASE_URL makes NAME a valid provider, NAME_API_KEY supplies the key,
 * NAME_DEFAULT_MODEL the fallback model. Exactly one BASE_URL+API_KEY pair
 * is auto-detected; ambiguity falls through to the normal detection order. */
TEST env_defined_providers(void) {
    ensure_env();
    codex_auth_write(false);
    unsetenv("CURSOR_API_KEY");
    unsetenv("OPENAI_API_KEY");
    unsetenv("OPENAI_BASE_URL");
    clear_env_providers();
    write_settings("{}");
    setenv("ORWELL_BASE_URL", "https://orwell.test/v1", 1);
    setenv("ORWELL_API_KEY", "sk-orwell", 1);
    setenv("ORWELL_DEFAULT_MODEL", "orwell-1", 1);

    /* explicit flag: the env vars alone define the provider */
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "orwell"));
    ASSERT_STR_EQ("orwell", tny_provider_name(ctx));
    ASSERT_STR_EQ("https://orwell.test/v1", ctx->base_url);
    ASSERT_STR_EQ("sk-orwell", ctx->api_key);
    ASSERT(ctx->model);
    ASSERT_STR_EQ("orwell-1", ctx->model);
    tny_ctx_free(ctx);

    /* auto-detection: exactly one BASE_URL + API_KEY pair wins */
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, NULL));
    ASSERT_STR_EQ("orwell", tny_provider_name(ctx));
    tny_ctx_free(ctx);

    /* two pairs are ambiguous: fall through, but both stay addressable */
    setenv("HUXLEY_BASE_URL", "https://huxley.test/v1", 1);
    setenv("HUXLEY_API_KEY", "sk-huxley", 1);
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, NULL));
    ASSERT_STR_EQ("openai", tny_provider_name(ctx));
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "huxley"));
    ASSERT_STR_EQ("huxley", tny_provider_name(ctx));
    ASSERT_STR_EQ("https://huxley.test/v1", ctx->base_url);
    tny_ctx_free(ctx);
    unsetenv("HUXLEY_BASE_URL");
    unsetenv("HUXLEY_API_KEY");

    /* a BASE_URL without a key is never auto-detected (a stray *_BASE_URL
     * from an unrelated tool must not hijack the default), but an explicit
     * --provider still accepts it — keyless local gateways */
    unsetenv("ORWELL_API_KEY");
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, NULL));
    ASSERT_STR_EQ("openai", tny_provider_name(ctx));
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "orwell"));
    ASSERT_STR_EQ("orwell", tny_provider_name(ctx));
    ASSERT_EQ(NULL, ctx->api_key);
    tny_ctx_free(ctx);
    setenv("ORWELL_API_KEY", "sk-orwell", 1);

    /* NAME_BASE_URL beats the settings profile's base_url; the profile's
     * model still beats NAME_DEFAULT_MODEL */
    write_settings("{\"orwell\":{\"base_url\":\"https://settings.test/v1\","
                   "\"model\":\"cfg-model\"}}");
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "orwell"));
    ASSERT_STR_EQ("https://orwell.test/v1", ctx->base_url);
    ASSERT(ctx->model);
    ASSERT_STR_EQ("cfg-model", ctx->model);
    tny_ctx_free(ctx);

    /* NAME_DEFAULT_MODEL also works for builtin providers */
    write_settings("{}");
    setenv("CODEX_DEFAULT_MODEL", "o4-mini", 1);
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_CODEX, tny_resolve_backend(ctx, "codex"));
    ASSERT(ctx->model);
    ASSERT_STR_EQ("o4-mini", ctx->model);
    unsetenv("CODEX_DEFAULT_MODEL");
    tny_ctx_free(ctx);

    /* the scan itself: builtin exclusion, every prefix char class, and the
     * vars that must NOT register (empty value, empty prefix, chars that
     * cannot round-trip through the derived env-var name) */
    setenv("OPENAI_BASE_URL", "https://builtin.test/v1", 1); /* builtin */
    setenv("AZ09_G_BASE_URL", "https://mixed.test/v1", 1);   /* valid */
    setenv("EMPTYP_BASE_URL", "", 1);                        /* not set */
    setenv("_BASE_URL", "https://noname.test/v1", 1);        /* no prefix */
    setenv("bad-Prefix_BASE_URL", "https://bad.test/v1", 1); /* bad chars */
    int n = 0;
    char **v = tny_env_provider_names(&n);
    ASSERT_EQ(2, n);
    ASSERT(v);
    bool saw_orwell = false, saw_mixed = false;
    for (int i = 0; i < n; i++) {
        if (strcmp(v[i], "orwell") == 0) saw_orwell = true;
        if (strcmp(v[i], "az09_g") == 0) saw_mixed = true;
        free(v[i]);
    }
    free(v);
    ASSERT(saw_orwell);
    ASSERT(saw_mixed);
    unsetenv("OPENAI_BASE_URL");
    unsetenv("AZ09_G_BASE_URL");
    unsetenv("EMPTYP_BASE_URL");
    unsetenv("_BASE_URL");
    unsetenv("bad-Prefix_BASE_URL");

    unsetenv("ORWELL_BASE_URL");
    unsetenv("ORWELL_API_KEY");
    unsetenv("ORWELL_DEFAULT_MODEL");

    /* a stale last_provider naming a vanished provider falls back cleanly */
    write_settings("{\"last_provider\":\"ghost\"}");
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, NULL));
    ASSERT_STR_EQ("openai", tny_provider_name(ctx));
    ASSERT_STR_EQ("https://api.openai.com/v1", ctx->base_url);
    tny_ctx_free(ctx);

    write_settings("{}");
    PASS();
}

/* /provider help lists what is actually usable here: builtins, then
 * settings.json profiles, then env-only providers, each once. */
/* ---- builtin subscription profiles: claude and grok (docs/adr/0019) ---- */

static bool has_extra_header(tny_ctx *ctx, const char *prefix) {
    for (char **h = ctx->extra_headers; h && *h; h++)
        if (str_starts(*h, prefix)) return true;
    return false;
}

static void claude_credentials_write(const char *token) {
    char path[600];
    snprintf(path, sizeof path, "%s/.claude", g_home);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/.claude/.credentials.json", g_home);
    if (!token) {
        unlink(path);
        return;
    }
    buf_t b;
    buf_init(&b);
    buf_appendf(&b,
                "{\"claudeAiOauth\":{\"accessToken\":\"%s\","
                "\"refreshToken\":\"r\",\"expiresAt\":9999999999999}}",
                token);
    file_write_atomic(path, b.data, b.len);
    buf_free(&b);
}

static void grok_auth_write(const char *token) {
    char path[600];
    snprintf(path, sizeof path, "%s/.grok", g_home);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/.grok/auth.json", g_home);
    if (!token) {
        unlink(path);
        return;
    }
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "{\"https://accounts.x.ai/sign-in\":{\"key\":\"%s\"}}", token);
    file_write_atomic(path, b.data, b.len);
    buf_free(&b);
}

/* The claude builtin: Anthropic's OpenAI-compat endpoint on the chat wire.
 * OAuth-sourced tokens add the anthropic-beta oauth header; a Console API
 * key must not carry it. */
TEST builtin_claude_profile(void) {
    ensure_env();
    write_settings("{}");
    codex_auth_write(false);

    /* env OAuth token */
    setenv("CLAUDE_CODE_OAUTH_TOKEN", "sk-ant-oat01-test", 1);
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "claude"));
    ASSERT_STR_EQ("claude", tny_provider_name(ctx));
    ASSERT_STR_EQ("https://api.anthropic.com/v1", ctx->base_url);
    ASSERT(ctx->wire_api);
    ASSERT_STR_EQ("chat", ctx->wire_api);
    ASSERT(ctx->api_key);
    ASSERT_STR_EQ("sk-ant-oat01-test", ctx->api_key);
    ASSERT(has_extra_header(ctx, "anthropic-beta: oauth-2025-04-20"));
    ASSERT(ctx->model); /* the openai default model must not leak in */
    ASSERT(strcmp(ctx->model, "gpt-4.1-mini") != 0);
    tny_ctx_free(ctx);
    unsetenv("CLAUDE_CODE_OAUTH_TOKEN");

    /* credentials file from `claude /login` */
    claude_credentials_write("sk-ant-oat01-fromfile");
    ctx = tny_ctx_load(g_ws);
    ASSERT(tny_claude_auth_present());
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "claude"));
    ASSERT(ctx->api_key);
    ASSERT_STR_EQ("sk-ant-oat01-fromfile", ctx->api_key);
    ASSERT(has_extra_header(ctx, "anthropic-beta:"));
    tny_ctx_free(ctx);
    claude_credentials_write(NULL);

    /* Console API key: bearer, no oauth beta header */
    setenv("ANTHROPIC_API_KEY", "sk-ant-api03-test", 1);
    ctx = tny_ctx_load(g_ws);
    ASSERT_FALSE(tny_claude_auth_present()); /* a raw key never auto-detects */
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "claude"));
    ASSERT(ctx->api_key);
    ASSERT_STR_EQ("sk-ant-api03-test", ctx->api_key);
    ASSERT_FALSE(has_extra_header(ctx, "anthropic-beta:"));
    /* switching to another provider must drop the profile's headers */
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "openai"));
    ASSERT_EQ(NULL, ctx->extra_headers);
    tny_ctx_free(ctx);
    unsetenv("ANTHROPIC_API_KEY");
    PASS();
}

/* The grok builtin: `grok login` session token drives the CLI chat proxy
 * (chat wire, proxy auth + model-override headers); XAI_API_KEY falls back
 * to api.x.ai on the default Responses wire. */
TEST builtin_grok_profile(void) {
    ensure_env();
    write_settings("{}");
    codex_auth_write(false);

    grok_auth_write("sess-tok-1");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT(tny_grok_auth_present());
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "grok"));
    ASSERT_STR_EQ("grok", tny_provider_name(ctx));
    ASSERT_STR_EQ("https://cli-chat-proxy.grok.com/v1", ctx->base_url);
    ASSERT(ctx->wire_api);
    ASSERT_STR_EQ("chat", ctx->wire_api);
    ASSERT(ctx->api_key);
    ASSERT_STR_EQ("sess-tok-1", ctx->api_key);
    ASSERT(has_extra_header(ctx, "X-XAI-Token-Auth: xai-grok-cli"));
    /* the proxy 426s requests without a client-version claim */
    ASSERT(has_extra_header(ctx, "x-grok-client-version: "));
    ASSERT(ctx->model);
    ASSERT_STR_EQ("grok-4.6", ctx->model);
    ASSERT(has_extra_header(ctx, "x-grok-model-override: grok-4.6"));
    tny_ctx_free(ctx);

    /* no session: XAI_API_KEY against api.x.ai, no proxy headers */
    grok_auth_write(NULL);
    setenv("XAI_API_KEY", "sk-xai-test", 1);
    ctx = tny_ctx_load(g_ws);
    ASSERT_FALSE(tny_grok_auth_present());
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "grok"));
    ASSERT_STR_EQ("https://api.x.ai/v1", ctx->base_url);
    ASSERT_EQ(NULL, ctx->wire_api); /* responses default */
    ASSERT(ctx->api_key);
    ASSERT_STR_EQ("sk-xai-test", ctx->api_key);
    ASSERT_FALSE(has_extra_header(ctx, "X-XAI-Token-Auth:"));
    ASSERT_FALSE(has_extra_header(ctx, "x-grok-model-override:"));
    tny_ctx_free(ctx);
    unsetenv("XAI_API_KEY");
    PASS();
}

/* ---- native grok device-code login/refresh/logout (docs/adr/0021) ---- */

/* Scripted OAuth2 issuer: one connection per response, request captured. */
typedef struct {
    int lfd;
    int n;
    int status[6];
    const char *body[6];
    char req[6][4096];
} oauth_mock;

static void *oauth_mock_run(void *ud) {
    oauth_mock *s = ud;
    for (int i = 0; i < s->n; i++) {
        int fd = accept(s->lfd, NULL, NULL);
        if (fd < 0) return NULL;
        char *req = s->req[i];
        size_t got = 0, cap = sizeof s->req[i];
        for (;;) {
            ssize_t r = read(fd, req + got, cap - 1 - got);
            if (r <= 0) break;
            got += (size_t)r;
            req[got] = 0;
            char *he = strstr(req, "\r\n\r\n");
            if (!he) continue;
            long cl = 0;
            char *clh = strstr(req, "Content-Length:");
            if (clh) cl = strtol(clh + 15, NULL, 10);
            if (got >= (size_t)(he - req) + 4 + (size_t)cl) break;
        }
        char resp[4608];
        int rl = snprintf(resp, sizeof resp,
                          "HTTP/1.1 %d X\r\nContent-Type: application/json\r\n"
                          "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                          s->status[i], strlen(s->body[i]), s->body[i]);
        if (write(fd, resp, (size_t)rl) < 0) { /* peer gone; keep serving */
        }
        close(fd);
    }
    return NULL;
}

/* Bind 127.0.0.1:0, start the thread, return the port (or -1). */
static int oauth_mock_start(oauth_mock *s, pthread_t *th) {
    s->lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->lfd < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s->lfd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(s->lfd, 4) != 0) return -1;
    socklen_t sl = sizeof sa;
    if (getsockname(s->lfd, (struct sockaddr *)&sa, &sl) != 0) return -1;
    if (pthread_create(th, NULL, oauth_mock_run, s) != 0) return -1;
    return ntohs(sa.sin_port);
}

/* Full device flow against a scripted issuer: pending poll, then tokens.
 * The store entry must be grok-CLI-compatible and readable by tny. */
TEST grok_native_device_login(void) {
    ensure_env();
    grok_auth_write(NULL);
    oauth_mock s = {0};
    s.n = 3;
    s.status[0] = 200;
    s.body[0] = "{\"device_code\":\"dc-1\",\"user_code\":\"AB-12\","
                "\"verification_uri\":\"https://x.ai/device\","
                "\"verification_uri_complete\":"
                "\"https://x.ai/device?user_code=AB-12\","
                "\"expires_in\":900,\"interval\":0}";
    s.status[1] = 400;
    s.body[1] = "{\"error\":\"authorization_pending\"}";
    s.status[2] = 200;
    s.body[2] = "{\"access_token\":\"at-1\",\"refresh_token\":\"rt-1\","
                "\"expires_in\":900,\"id_token\":"
                "\"e30.eyJzdWIiOiJ1LTEiLCJlbWFpbCI6ImFAYi5jIn0.sig\"}";
    pthread_t th;
    int port = oauth_mock_start(&s, &th);
    ASSERT(port > 0);
    char issuer[64];
    snprintf(issuer, sizeof issuer, "http://127.0.0.1:%d", port);
    setenv("GROK_OAUTH2_ISSUER", issuer, 1);
    setenv("GROK_OAUTH2_CLIENT_ID", "cid-1", 1);
    int rc = tny_grok_login();
    pthread_join(th, NULL);
    close(s.lfd);
    ASSERT_EQ(0, rc);

    /* wire shape: endpoints, form encoding, RFC 8628 grant */
    ASSERT(strstr(s.req[0], "POST /oauth2/device/code"));
    ASSERT(strstr(s.req[0], "client_id=cid-1"));
    ASSERT(strstr(s.req[0], "grok-cli%3Aaccess")); /* scope, %-encoded */
    ASSERT(strstr(s.req[0], "referrer=grok-build"));
    ASSERT(strstr(s.req[2], "POST /oauth2/token"));
    ASSERT(strstr(s.req[2], "device_code=dc-1"));
    ASSERT(strstr(s.req[2], "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code"));

    /* the store entry is grok-CLI-compatible */
    char path[600];
    snprintf(path, sizeof path, "%s/.grok/auth.json", g_home);
    yyjson_doc *doc = jparse_file(path);
    ASSERT(doc);
    char scope[128];
    snprintf(scope, sizeof scope, "%s::cid-1", issuer);
    yyjson_val *e = jget(yyjson_doc_get_root(doc), scope);
    ASSERT(e);
    ASSERT_STR_EQ("at-1", jget_str(e, "key"));
    ASSERT_STR_EQ("oidc", jget_str(e, "auth_mode"));
    ASSERT_STR_EQ("rt-1", jget_str(e, "refresh_token"));
    ASSERT_STR_EQ("u-1", jget_str(e, "user_id")); /* id_token sub */
    ASSERT_STR_EQ("a@b.c", jget_str(e, "email"));
    ASSERT_STR_EQ(issuer, jget_str(e, "oidc_issuer"));
    ASSERT_STR_EQ("cid-1", jget_str(e, "oidc_client_id"));
    ASSERT(jget_str(e, "create_time"));
    ASSERT(jget_str(e, "expires_at"));
    yyjson_doc_free(doc);

    /* and tny reads it back as the session token */
    char *tok = tny_grok_session_token();
    ASSERT(tok);
    ASSERT_STR_EQ("at-1", tok);
    free(tok);

    unsetenv("GROK_OAUTH2_ISSUER");
    unsetenv("GROK_OAUTH2_CLIENT_ID");
    grok_auth_write(NULL);
    PASS();
}

/* A denied (or expired) device code fails without writing credentials. */
TEST grok_native_login_denied(void) {
    ensure_env();
    grok_auth_write(NULL);
    oauth_mock s = {0};
    s.n = 2;
    s.status[0] = 200;
    s.body[0] = "{\"device_code\":\"dc-2\",\"user_code\":\"CD-34\","
                "\"verification_uri\":\"https://x.ai/device\","
                "\"expires_in\":900,\"interval\":0}";
    s.status[1] = 400;
    s.body[1] = "{\"error\":\"access_denied\"}";
    pthread_t th;
    int port = oauth_mock_start(&s, &th);
    ASSERT(port > 0);
    char issuer[64];
    snprintf(issuer, sizeof issuer, "http://127.0.0.1:%d", port);
    setenv("GROK_OAUTH2_ISSUER", issuer, 1);
    setenv("GROK_OAUTH2_CLIENT_ID", "cid-1", 1);
    int rc = tny_grok_login();
    pthread_join(th, NULL);
    close(s.lfd);
    ASSERT(rc != 0);
    ASSERT_FALSE(tny_grok_auth_present());
    unsetenv("GROK_OAUTH2_ISSUER");
    unsetenv("GROK_OAUTH2_CLIENT_ID");
    PASS();
}

/* An expired entry refreshes in place (issuer and client_id ride inside the
 * entry — no env needed) and the rotated tokens persist; a fresh entry is
 * left alone (no network: the mock is gone by then). */
TEST grok_native_refresh(void) {
    ensure_env();
    oauth_mock s = {0};
    s.n = 1;
    s.status[0] = 200;
    s.body[0] = "{\"access_token\":\"at-new\",\"refresh_token\":\"rt-new\","
                "\"expires_in\":900}";
    pthread_t th;
    int port = oauth_mock_start(&s, &th);
    ASSERT(port > 0);
    char issuer[64];
    snprintf(issuer, sizeof issuer, "http://127.0.0.1:%d", port);

    char path[600];
    snprintf(path, sizeof path, "%s/.grok", g_home);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/.grok/auth.json", g_home);
    buf_t b;
    buf_init(&b);
    buf_appendf(&b,
                "{\"%s::cid-1\":{\"key\":\"at-old\",\"auth_mode\":\"oidc\","
                "\"create_time\":\"2020-01-01T00:00:00Z\",\"user_id\":\"u-1\","
                "\"refresh_token\":\"rt-old\",\"expires_at\":\"2020-01-01T00:15:00Z\","
                "\"oidc_issuer\":\"%s\",\"oidc_client_id\":\"cid-1\"}}",
                issuer, issuer);
    file_write_atomic(path, b.data, b.len);
    buf_free(&b);

    tny_grok_refresh_if_stale();
    pthread_join(th, NULL);
    close(s.lfd);

    ASSERT(strstr(s.req[0], "POST /oauth2/token"));
    ASSERT(strstr(s.req[0], "grant_type=refresh_token"));
    ASSERT(strstr(s.req[0], "refresh_token=rt-old"));
    ASSERT(strstr(s.req[0], "client_id=cid-1"));

    yyjson_doc *doc = jparse_file(path);
    ASSERT(doc);
    char scope[128];
    snprintf(scope, sizeof scope, "%s::cid-1", issuer);
    yyjson_val *e = jget(yyjson_doc_get_root(doc), scope);
    ASSERT(e);
    ASSERT_STR_EQ("at-new", jget_str(e, "key"));
    ASSERT_STR_EQ("rt-new", jget_str(e, "refresh_token"));
    ASSERT_STR_EQ("u-1", jget_str(e, "user_id")); /* profile carried over */
    yyjson_doc_free(doc);

    /* now fresh: must return without touching the (closed) issuer */
    tny_grok_refresh_if_stale();
    char *tok = tny_grok_session_token();
    ASSERT(tok);
    ASSERT_STR_EQ("at-new", tok);
    free(tok);
    grok_auth_write(NULL);
    PASS();
}

/* Logout drops the xAI entries (legacy scope, auth.x.ai entries) but keeps
 * foreign-issuer entries; the file goes away once nothing is left. */
TEST grok_native_logout(void) {
    ensure_env();
    unsetenv("GROK_OAUTH2_ISSUER");
    unsetenv("GROK_OAUTH2_CLIENT_ID");
    char path[600];
    snprintf(path, sizeof path, "%s/.grok", g_home);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/.grok/auth.json", g_home);
    const char *store = "{\"https://accounts.x.ai/sign-in\":{\"key\":\"k1\"},"
                        "\"https://auth.x.ai::cid\":{\"key\":\"k2\","
                        "\"oidc_issuer\":\"https://auth.x.ai\"},"
                        "\"https://idp.acme.example::c\":{\"key\":\"k3\","
                        "\"oidc_issuer\":\"https://idp.acme.example\"}}";
    file_write_atomic(path, store, strlen(store));

    ASSERT_EQ(0, tny_grok_logout());
    yyjson_doc *doc = jparse_file(path);
    ASSERT(doc); /* foreign issuer kept, file kept */
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_EQ(1, (int)yyjson_obj_size(root));
    ASSERT(jget(root, "https://idp.acme.example::c"));
    yyjson_doc_free(doc);

    /* nothing of ours left: logout is a no-op, not an error */
    ASSERT_EQ(0, tny_grok_logout());
    ASSERT(file_exists(path));

    /* only xAI entries: the file itself goes away */
    const char *only = "{\"https://accounts.x.ai/sign-in\":{\"key\":\"k1\"}}";
    file_write_atomic(path, only, strlen(only));
    ASSERT_EQ(0, tny_grok_logout());
    ASSERT_FALSE(file_exists(path));
    PASS();
}

/* Credential edge cases: empty tokens are not credentials, the sign-in
 * entry wins over other auth.json objects, a credential-less claude profile
 * never carries the oauth beta header, and the header plumbing rejects
 * empty lines. */
TEST builtin_profile_edge_credentials(void) {
    ensure_env();
    write_settings("{}");
    codex_auth_write(false);

    /* credentials file without an accessToken: no token, and the source
     * out-param must stay untouched */
    char path[600];
    snprintf(path, sizeof path, "%s/.claude", g_home);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/.claude/.credentials.json", g_home);
    const char *no_tok = "{\"claudeAiOauth\":{}}";
    file_write_atomic(path, no_tok, strlen(no_tok));
    const char *source = NULL;
    char *tok = tny_claude_token(&source);
    ASSERT_EQ(NULL, tok);
    ASSERT_EQ(NULL, source);

    /* an empty accessToken string is not a credential either */
    const char *empty_tok = "{\"claudeAiOauth\":{\"accessToken\":\"\"}}";
    file_write_atomic(path, empty_tok, strlen(empty_tok));
    tok = tny_claude_token(&source);
    ASSERT_EQ(NULL, tok);

    /* no resolvable credential: the claude profile must not invent a key
     * or attach the oauth beta header */
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "claude"));
    ASSERT_EQ(NULL, ctx->api_key);
    ASSERT_FALSE(has_extra_header(ctx, "anthropic-beta:"));

    /* the header plumbing ignores empty lines */
    tny_ctx_add_extra_header(ctx, "");
    ASSERT_FALSE(has_extra_header(ctx, ""));
    tny_ctx_free(ctx);
    unlink(path);

    /* grok: the accounts.x.ai sign-in entry wins over other objects that
     * also carry a "key" (OIDC issuers, unrelated caches) */
    char gpath[600];
    snprintf(gpath, sizeof gpath, "%s/.grok", g_home);
    mkdir_p(gpath);
    snprintf(gpath, sizeof gpath, "%s/.grok/auth.json", g_home);
    const char *two = "{\"other\":{\"key\":\"wrong\"},"
                      "\"https://accounts.x.ai/sign-in\":{\"key\":\"right\"}}";
    file_write_atomic(gpath, two, strlen(two));
    char *g = tny_grok_session_token();
    ASSERT(g);
    ASSERT_STR_EQ("right", g);
    free(g);

    /* an empty session key is not a session */
    const char *empty_key = "{\"https://accounts.x.ai/sign-in\":{\"key\":\"\"}}";
    file_write_atomic(gpath, empty_key, strlen(empty_key));
    ASSERT_EQ(NULL, tny_grok_session_token());

    /* the model-override header needs a proxy base_url AND a real model */
    grok_auth_write("sess-tok-3");
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "grok"));
    tny_ctx_clear_extra_headers(ctx);
    free(ctx->model);
    ctx->model = xstrdup("");
    tny_finish_builtin_profile(ctx);
    ASSERT_FALSE(has_extra_header(ctx, "x-grok-model-override:"));
    tny_ctx_free(ctx);
    grok_auth_write(NULL);

    /* an empty XAI_API_KEY is unset */
    setenv("XAI_API_KEY", "", 1);
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "grok"));
    ASSERT_EQ(NULL, ctx->api_key);
    tny_ctx_free(ctx);
    unsetenv("XAI_API_KEY");
    PASS();
}

/* Subscription logins auto-detect in docs/cli.md order: codex first, then
 * claude, then grok — and a settings profile with the builtin's name
 * shadows the builtin entirely (explicit config wins). */
TEST builtin_profile_detection_and_shadowing(void) {
    ensure_env();
    write_settings("{}");
    codex_auth_write(false);
    unsetenv("CURSOR_API_KEY");

    grok_auth_write("sess-tok-2");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, NULL));
    ASSERT_STR_EQ("grok", tny_provider_name(ctx));
    tny_ctx_free(ctx);

    setenv("CLAUDE_CODE_OAUTH_TOKEN", "sk-ant-oat01-test", 1);
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, NULL));
    ASSERT_STR_EQ("claude", tny_provider_name(ctx)); /* claude beats grok */
    tny_ctx_free(ctx);

    codex_auth_write(true);
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_CODEX, tny_resolve_backend(ctx, NULL)); /* codex first */
    tny_ctx_free(ctx);
    codex_auth_write(false);

    /* last_provider remembers a builtin profile by name */
    write_settings("{\"last_provider\":\"claude\"}");
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, NULL));
    ASSERT_STR_EQ("claude", tny_provider_name(ctx));
    ASSERT_STR_EQ("https://api.anthropic.com/v1", ctx->base_url);
    tny_ctx_free(ctx);

    /* the remembered builtin beats the detection order: grok last-used
     * wins even while claude credentials are also present */
    write_settings("{\"last_provider\":\"grok\"}");
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, NULL));
    ASSERT_STR_EQ("grok", tny_provider_name(ctx));
    tny_ctx_free(ctx);

    /* a stale last_provider naming nothing must fall through to detection,
     * never resolve as a phantom builtin profile */
    write_settings("{\"last_provider\":\"gone\"}");
    codex_auth_write(true);
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_CODEX, tny_resolve_backend(ctx, NULL));
    tny_ctx_free(ctx);
    codex_auth_write(false);
    write_settings("{}");

    /* a user settings profile named "claude" shadows the builtin */
    write_settings("{\"claude\":{\"base_url\":\"https://gw.test/v1\","
                   "\"api_key_env\":\"GW_KEY\"}}");
    setenv("GW_KEY", "sk-gw", 1);
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "claude"));
    ASSERT_STR_EQ("https://gw.test/v1", ctx->base_url);
    ASSERT(ctx->api_key);
    ASSERT_STR_EQ("sk-gw", ctx->api_key);
    ASSERT_FALSE(has_extra_header(ctx, "anthropic-beta:"));
    tny_ctx_free(ctx);

    /* auto-detection routes through the shadowing profile too */
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, NULL));
    ASSERT_STR_EQ("claude", tny_provider_name(ctx));
    ASSERT_STR_EQ("https://gw.test/v1", ctx->base_url);
    tny_ctx_free(ctx);

    unsetenv("GW_KEY");
    unsetenv("CLAUDE_CODE_OAUTH_TOKEN");
    grok_auth_write(NULL);
    write_settings("{}");
    PASS();
}

TEST provider_names_joined_lists_detected(void) {
    ensure_env();
    clear_env_providers();
    write_settings("{\"openrouter\":{\"base_url\":\"https://openrouter.ai/api/v1\"},"
                   "\"nourl\":{\"model\":\"x\"},"
                   "\"xai\":{\"base_url\":\"https://api.x.ai/v1\"}}");
    setenv("XAI_BASE_URL", "https://api.x.ai/v1", 1);       /* dup of settings */
    setenv("ORWELL_BASE_URL", "https://orwell.test/v1", 1); /* env only */
    tny_ctx *ctx = tny_ctx_load(g_ws);
    char *j = tny_provider_names_joined(ctx);
    ASSERT(j);
    ASSERT_STR_EQ("openai|cursor|codex|acp|claude|grok|openrouter|xai|orwell", j);
    free(j);
    tny_ctx_free(ctx);
    unsetenv("XAI_BASE_URL");
    unsetenv("ORWELL_BASE_URL");
    write_settings("{}");
    PASS();
}

/* ---- fast tier capability (--fast / /fast) ---- */

/* TNY_CAP_FAST names the providers with a paid fast tier: codex serviceTier,
 * openai service_tier, cursor's per-model "fast" param. ACP has no tier
 * field in session/new, so it must not carry the bit. */
TEST fast_capability_per_provider(void) {
    ASSERT(tny_backend_caps(TNY_BK_OPENAI) & TNY_CAP_FAST);
    ASSERT(tny_backend_caps(TNY_BK_CURSOR) & TNY_CAP_FAST);
    ASSERT(tny_backend_caps(TNY_BK_CODEX) & TNY_CAP_FAST);
    ASSERT_FALSE(tny_backend_caps(TNY_BK_ACP) & TNY_CAP_FAST);
    ASSERT_EQ(0u, tny_backend_caps((tny_backend_id)TNY_BK_COUNT));
    PASS();
}

/* OpenAI renamed "priority" processing to "fast" mode; both spellings must
 * select the tier, everything else (including NULL and "default") must not. */
TEST fast_tier_spellings(void) {
    ASSERT(tny_tier_is_fast("fast"));
    ASSERT(tny_tier_is_fast("priority"));
    ASSERT_FALSE(tny_tier_is_fast("default"));
    ASSERT_FALSE(tny_tier_is_fast(""));
    ASSERT_FALSE(tny_tier_is_fast("FAST"));
    ASSERT_FALSE(tny_tier_is_fast(NULL));
    PASS();
}

/* `tny --provider X --fast …` parses as a leading global and lands on
 * ctx->service_tier for capable providers. */
TEST fast_flag_sets_service_tier(void) {
    ensure_env();
    write_settings("{}");

    char *argv[] = {"tny", "--provider", "openai", "--fast", "ask", "hi", NULL};
    cli_globals g = {0};
    int ci = cli_parse_globals(6, argv, &g);
    ASSERT_EQ(4, ci); /* the subcommand index */
    ASSERT(g.fast);
    g.cwd = g_ws;

    tny_ctx *ctx = cli_make_ctx(&g);
    ASSERT(ctx);
    ASSERT_EQ(TNY_BK_OPENAI, ctx->backend);
    ASSERT(ctx->service_tier);
    ASSERT(tny_tier_is_fast(ctx->service_tier));
    tny_ctx_free(ctx);

    /* without the flag nothing sets a tier */
    cli_globals g2 = {0};
    g2.backend = "openai";
    g2.cwd = g_ws;
    ctx = cli_make_ctx(&g2);
    ASSERT(ctx);
    ASSERT_EQ(NULL, ctx->service_tier);
    tny_ctx_free(ctx);
    PASS();
}

/* The cursor mapping is a per-model param: fast tiers pin the fast variant,
 * "default" pins the standard one, unset appends nothing (the model's own
 * default variant — which may itself be the fast one — applies). */
TEST fast_cursor_model_param(void) {
    buf_t b;

    buf_init(&b);
    cursor_append_model_params(&b, "fast");
    ASSERT_STR_EQ(",\"params\":[{\"id\":\"fast\",\"value\":\"true\"}]", b.data);
    buf_free(&b);

    buf_init(&b);
    cursor_append_model_params(&b, "priority");
    ASSERT_STR_EQ(",\"params\":[{\"id\":\"fast\",\"value\":\"true\"}]", b.data);
    buf_free(&b);

    buf_init(&b);
    cursor_append_model_params(&b, "default");
    ASSERT_STR_EQ(",\"params\":[{\"id\":\"fast\",\"value\":\"false\"}]", b.data);
    buf_free(&b);

    buf_init(&b);
    cursor_append_model_params(&b, NULL);
    cursor_append_model_params(&b, "");
    ASSERT_EQ(0, (int)b.len);
    buf_free(&b);
    PASS();
}

/* --fast on a provider without the capability is a startup error (exit 1
 * path): cli_make_ctx must refuse instead of silently dropping the flag. */
TEST fast_flag_rejected_without_capability(void) {
    ensure_env();
    write_settings("{}");

    cli_globals g = {0};
    g.backend = "acp";
    g.cwd = g_ws;
    g.fast = true;
    fprintf(stderr, "(expected error) ");
    tny_ctx *ctx = cli_make_ctx(&g);
    ASSERT_EQ(NULL, ctx);

    g.backend = "codex"; /* same globals on a capable provider succeed */
    ctx = cli_make_ctx(&g);
    ASSERT(ctx);
    ASSERT(tny_tier_is_fast(ctx->service_tier));
    tny_ctx_free(ctx);
    PASS();
}

TEST backend_default_cursor_key_from_env(void) {
    ensure_env();
    write_settings("{}");
    codex_auth_write(false);
    setenv("CURSOR_API_KEY", "key_test", 1);
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_CURSOR, tny_resolve_backend(ctx, NULL));
    tny_ctx_free(ctx);

    codex_auth_write(true); /* codex login outranks a cursor env key */
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_CODEX, tny_resolve_backend(ctx, NULL));
    tny_ctx_free(ctx);

    unsetenv("CURSOR_API_KEY");
    codex_auth_write(false);
    PASS();
}

/* ---- codex host registry (shared app-server reuse) ---- */

/* The registry is untrusted: only a bare ws:// loopback URL may pass. */
TEST codex_registry_loopback_only(void) {
    static const char *const ok[] = {
        "ws://127.0.0.1:8080",
        "ws://localhost:1234",
        "ws://127.0.0.1:65535/",
    };
    static const char *const bad[] = {
        "",
        "wss://127.0.0.1:443",
        "http://127.0.0.1:80",
        "ws://127.0.0.1",
        "ws://127.0.0.1:",
        "ws://127.0.0.1:0",
        "ws://127.0.0.1:65536",
        "ws://127.0.0.2:80",
        "ws://10.0.0.5:80",
        "ws://localhost.evil.io:80",
        "ws://evil:80",
        "ws://127.0.0.1:80/path",
        "ws://127.0.0.1:80x",
        "ws://[::1]:80",
        "ws://user@127.0.0.1:80",
    };
    ASSERT(!cx_ws_url_is_loopback(NULL));
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        ASSERTm(ok[i], cx_ws_url_is_loopback(ok[i]));
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        ASSERT_FALSEm(bad[i], cx_ws_url_is_loopback(bad[i]));
    PASS();
}

TEST codex_registry_roundtrip(void) {
    ensure_env();
    ASSERT_EQ(0, cx_registry_write("ws://127.0.0.1:4242", getpid()));
    char *path = cx_registry_path();
    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    /* 0600 on POSIX. Windows/MSYS keeps group/other bits on NTFS. */
#if !defined(__CYGWIN__) && !defined(__MSYS__)
    ASSERT_EQ(0, (int)(st.st_mode & 077)); /* private to the user */
#endif
    char *ws = NULL;
    pid_t pid = 0;
    ASSERT_EQ(0, cx_registry_load(&ws, &pid));
    ASSERT_STR_EQ("ws://127.0.0.1:4242", ws);
    ASSERT_EQ(getpid(), pid);
    free(ws);
    /* a mismatched pid means a newer writer owns the file: leave it */
    ASSERT(cx_registry_remove(getpid() + 1) != 0);
    ASSERT(file_exists(path));
    ASSERT_EQ(0, cx_registry_remove(getpid()));
    ASSERT_FALSE(file_exists(path));
    ASSERT(cx_registry_load(&ws, &pid) != 0); /* missing file */
    ASSERT_EQ(NULL, ws);
    free(path);
    PASS();
}

static void codex_registry_raw(const char *json) {
    char *dir = path_tny_dir();
    mkdir_p(dir);
    free(dir);
    char *path = cx_registry_path();
    if (json) file_write_atomic(path, json, strlen(json));
    else unlink(path);
    free(path);
}

TEST codex_registry_rejects_bad_entries(void) {
    ensure_env();
    char *ws = NULL;
    char json[128];

    codex_registry_raw("this is not json");
    ASSERT(cx_registry_load(&ws, NULL) != 0);

    codex_registry_raw("{\"ws\":\"ws://127.0.0.1:4242\"}"); /* no pid */
    ASSERT(cx_registry_load(&ws, NULL) != 0);

    snprintf(json, sizeof json, "{\"ws\":\"ws://10.0.0.5:4242\",\"pid\":%ld}", (long)getpid());
    codex_registry_raw(json); /* live pid but off-loopback: never attach */
    ASSERT(cx_registry_load(&ws, NULL) != 0);

    codex_registry_raw("{\"ws\":\"ws://127.0.0.1:4242\",\"pid\":99999999}");
    ASSERT(cx_registry_load(&ws, NULL) != 0); /* dead pid is a stale host */

    codex_registry_raw("{\"ws\":\"ws://127.0.0.1:4242\",\"pid\":-1}");
    ASSERT(cx_registry_load(&ws, NULL) != 0);

    codex_registry_raw(NULL);
    PASS();
}

/* 1x1 transparent PNG */
static const uint8_t PNG1[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
    0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78,
    0x9c, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

TEST image_mime_from_magic(void) {
    ASSERT_STR_EQ("image/png", image_mime(PNG1, sizeof PNG1));
    uint8_t jpeg[12] = {0xff, 0xd8, 0xff, 0xe0, 0, 0, 0, 0, 0, 0, 0, 0};
    ASSERT_STR_EQ("image/jpeg", image_mime(jpeg, sizeof jpeg));
    uint8_t gif[12] = {'G', 'I', 'F', '8', '9', 'a', 0, 0, 0, 0, 0, 0};
    ASSERT_STR_EQ("image/gif", image_mime(gif, sizeof gif));
    uint8_t webp[12] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
    ASSERT_STR_EQ("image/webp", image_mime(webp, sizeof webp));
    ASSERT_EQ(NULL, image_mime((const uint8_t *)"not an image!!", 14));
    ASSERT_EQ(NULL, image_mime(PNG1, 8)); /* too short */
    PASS();
}

TEST image_data_url_roundtrip(void) {
    buf_t url;
    buf_init(&url);
    image_data_url(PNG1, sizeof PNG1, "image/png", &url);
    ASSERT(str_starts(url.data, "data:image/png;base64,"));
    const char *b64 = url.data + strlen("data:image/png;base64,");
    uint8_t back[128];
    size_t n = b64_decode(b64, back, sizeof back);
    ASSERT_EQ_FMT(sizeof PNG1, n, "%zu");
    ASSERT_MEM_EQ(PNG1, back, sizeof PNG1);
    buf_free(&url);
    PASS();
}

TEST read_image_queues_user_message(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ctx->perm_mode = TNY_MODE_YOLO;
    tny_session_state *s = session_new(ctx);
    perm_engine *p = perm_new(ctx);
    tools_env env;
    memset(&env, 0, sizeof env);
    env.ctx = ctx;
    env.session = s;
    env.perm = p;

    char pngpath[600];
    snprintf(pngpath, sizeof pngpath, "%s/dot.png", g_ws);
    file_write_atomic(pngpath, PNG1, sizeof PNG1);

    char args[700];
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", pngpath);
    char *res = tools_execute(&env, "read_image", args);
    ASSERT(res);
    ASSERT(strstr(res, "image/png"));
    ASSERT_FALSE(str_starts(res, "error:"));
    ASSERT_EQ(1, env.n_pending_images);
    free(res);

    /* read_file must refuse the same path instead of dumping bytes */
    res = tools_execute(&env, "read_file", args);
    ASSERT(res);
    ASSERT(str_starts(res, "error:"));
    ASSERT(strstr(res, "read_image"));
    free(res);

    char err[128];
    ASSERT_EQ(0, tools_flush_images(&env, err, sizeof err));
    ASSERT_EQ(0, env.n_pending_images);
    yyjson_mut_val *msgs = session_messages(s);
    yyjson_mut_val *last = yyjson_mut_arr_get(msgs, yyjson_mut_arr_size(msgs) - 1);
    ASSERT_STR_EQ("user", yyjson_mut_get_str(yyjson_mut_obj_get(last, "role")));
    yyjson_mut_val *content = yyjson_mut_obj_get(last, "content");
    ASSERT(yyjson_mut_is_arr(content));
    ASSERT_EQ(2, (int)yyjson_mut_arr_size(content));
    yyjson_mut_val *img = yyjson_mut_arr_get(content, 1);
    ASSERT_STR_EQ("image_url", yyjson_mut_get_str(yyjson_mut_obj_get(img, "type")));
    const char *url =
        yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_obj_get(img, "image_url"), "url"));
    ASSERT(url);
    ASSERT(str_starts(url, "data:image/png;base64,"));

    /* vision is an alias */
    res = tools_execute(&env, "vision", args);
    ASSERT(res);
    ASSERT_FALSE(str_starts(res, "error:"));
    ASSERT_EQ(1, env.n_pending_images);
    free(res);
    tools_flush_images(&env, err, sizeof err);

    perm_free(p);
    session_close(s);
    tny_ctx_free(ctx);
    PASS();
}

TEST perm_read_image_is_safe(void) {
    ASSERT(perm_tool_is_safe("read_image"));
    PASS();
}

TEST cmd_ask_image_overflow_frees_prompt(void) {
    ensure_env();
    write_settings("{}");
    cli_globals g = {0};
    g.cwd = g_ws;
    g.backend = "openai";
    tny_ctx *ctx = cli_make_ctx(&g);
    ASSERT(ctx);

    char *argv[35];
    argv[0] = "hello"; /* allocate prompt.data before the overflow path */
    for (int i = 0; i < 17; i++) {
        argv[1 + i * 2] = "--image";
        argv[2 + i * 2] = "x.png";
    }

    int stderr_pipe[2];
    ASSERT_EQ(0, pipe(stderr_pipe));
    int saved_stderr = dup(STDERR_FILENO);
    ASSERT(saved_stderr >= 0);
    fflush(stderr);
    ASSERT_EQ(STDERR_FILENO, dup2(stderr_pipe[1], STDERR_FILENO));
    close(stderr_pipe[1]);

    int rc = cmd_ask(ctx, &g, 35, argv);
    fflush(stderr);
    ASSERT_EQ(STDERR_FILENO, dup2(saved_stderr, STDERR_FILENO));
    close(saved_stderr);

    char stderr_output[256];
    ssize_t n = read(stderr_pipe[0], stderr_output, sizeof stderr_output - 1);
    close(stderr_pipe[0]);
    ASSERT(n >= 0);
    stderr_output[n] = '\0';
    tny_ctx_free(ctx);

    ASSERT_EQ(1, rc);
    ASSERT(strstr(stderr_output, "tny: too many --image flags (max 16)"));
    PASS();
}

/* ---- structured outputs (tny ask --output-schema) ---- */

TEST output_schema_wraps_bare_schema(void) {
    const char *s = "{\"type\":\"object\",\"properties\":{\"n\":{\"type\":\"integer\"}}}";
    char *rf = tny_openai_response_format(s, strlen(s));
    ASSERT(rf);
    yyjson_doc *doc = jparse(rf, strlen(rf));
    ASSERT(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_STR_EQ("json_schema", jget_str(root, "type"));
    yyjson_val *js = jget(root, "json_schema");
    ASSERT_STR_EQ("output", jget_str(js, "name"));
    ASSERT(jget_bool(js, "strict", false));
    ASSERT_STR_EQ("object", jget_str(jget(js, "schema"), "type"));
    yyjson_doc_free(doc);
    free(rf);
    PASS();
}

TEST output_schema_accepts_json_schema_object(void) {
    const char *s = "{\"name\":\"todo\",\"strict\":false,"
                    "\"schema\":{\"type\":\"object\"}}";
    char *rf = tny_openai_response_format(s, strlen(s));
    ASSERT(rf);
    yyjson_doc *doc = jparse(rf, strlen(rf));
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_STR_EQ("json_schema", jget_str(root, "type"));
    yyjson_val *js = jget(root, "json_schema");
    ASSERT_STR_EQ("todo", jget_str(js, "name"));
    ASSERT_FALSE(jget_bool(js, "strict", true));
    yyjson_doc_free(doc);
    free(rf);
    PASS();
}

TEST output_schema_names_anonymous_json_schema_object(void) {
    const char *s = "{\"schema\":{\"type\":\"object\"}}";
    char *rf = tny_openai_response_format(s, strlen(s));
    ASSERT(rf);
    yyjson_doc *doc = jparse(rf, strlen(rf));
    yyjson_val *js = jget(yyjson_doc_get_root(doc), "json_schema");
    ASSERT_STR_EQ("output", jget_str(js, "name"));
    yyjson_doc_free(doc);
    free(rf);
    PASS();
}

TEST output_schema_passes_full_wrapper_through(void) {
    const char *s = "{\"type\":\"json_schema\",\"json_schema\":"
                    "{\"name\":\"x\",\"schema\":{\"type\":\"object\"}}}";
    char *rf = tny_openai_response_format(s, strlen(s));
    ASSERT(rf);
    yyjson_doc *doc = jparse(rf, strlen(rf));
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_STR_EQ("json_schema", jget_str(root, "type"));
    ASSERT_STR_EQ("x", jget_str(jget(root, "json_schema"), "name"));
    yyjson_doc_free(doc);
    free(rf);
    PASS();
}

TEST output_schema_rejects_non_object(void) {
    ASSERT_EQ(NULL, tny_openai_response_format("not json", 8));
    ASSERT_EQ(NULL, tny_openai_response_format("[1,2]", 5));
    ASSERT_EQ(NULL, tny_openai_response_format("\"str\"", 5));
    PASS();
}

/* ---- Responses API wire translation (docs/adr/0016) ----
 * Sessions persist chat-shaped messages; the responses wire translates
 * them at request time. These pin the translation invariants. */

/* One full loop of history: text turns, an assistant tool call, and its
 * result must come out as the exact Responses item sequence. */
TEST responses_input_translates_history(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    tny_session_state *s = session_new(ctx);
    session_add_text(s, "user", "hello");
    session_add_assistant(s, "hi there", NULL);
    session_add_text(s, "user", "list files");
    session_add_assistant(
        s, NULL,
        "[{\"id\":\"call_9\",\"type\":\"function\",\"function\":"
        "{\"name\":\"list_files\",\"arguments\":\"{\\\"path\\\": \\\".\\\"}\"}}]");
    session_add_tool_result(s, "call_9", "a.txt\nb.txt");

    char *in = tny_openai_responses_input(session_messages(s), 0, NULL);
    ASSERT(in);
    yyjson_doc *doc = jparse(in, strlen(in));
    ASSERT(doc);
    yyjson_val *arr = yyjson_doc_get_root(doc);
    ASSERT(yyjson_is_arr(arr));
    ASSERT_EQ_FMT(5, (int)yyjson_arr_size(arr), "%d");

    yyjson_val *m0 = yyjson_arr_get(arr, 0);
    ASSERT_STR_EQ("user", jget_str(m0, "role"));
    ASSERT_STR_EQ("hello", jget_str(m0, "content"));
    yyjson_val *m1 = yyjson_arr_get(arr, 1);
    ASSERT_STR_EQ("assistant", jget_str(m1, "role"));
    ASSERT_STR_EQ("hi there", jget_str(m1, "content"));

    /* the null-content assistant message is only its function_call item */
    yyjson_val *fc = yyjson_arr_get(arr, 3);
    ASSERT_STR_EQ("function_call", jget_str(fc, "type"));
    ASSERT_STR_EQ("call_9", jget_str(fc, "call_id"));
    ASSERT_STR_EQ("list_files", jget_str(fc, "name"));
    /* arguments stay a JSON *string*, byte-for-byte */
    ASSERT_STR_EQ("{\"path\": \".\"}", jget_str(fc, "arguments"));
    ASSERT_EQ(NULL, jget(fc, "role"));

    yyjson_val *out = yyjson_arr_get(arr, 4);
    ASSERT_STR_EQ("function_call_output", jget_str(out, "type"));
    ASSERT_STR_EQ("call_9", jget_str(out, "call_id"));
    ASSERT_STR_EQ("a.txt\nb.txt", jget_str(out, "output"));

    yyjson_doc_free(doc);
    free(in);
    session_close(s);
    tny_ctx_free(ctx);
    PASS();
}

/* The compaction summary rides as a leading system item and everything
 * before the boundary is dropped from the wire. */
TEST responses_input_honors_compact_boundary(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    tny_session_state *s = session_new(ctx);
    session_add_text(s, "user", "old question");
    session_add_assistant(s, "old answer", NULL);
    session_add_text(s, "user", "new question");

    char *in = tny_openai_responses_input(session_messages(s), 2, "the summary");
    ASSERT(in);
    yyjson_doc *doc = jparse(in, strlen(in));
    yyjson_val *arr = yyjson_doc_get_root(doc);
    ASSERT_EQ_FMT(2, (int)yyjson_arr_size(arr), "%d");
    yyjson_val *m0 = yyjson_arr_get(arr, 0);
    ASSERT_STR_EQ("system", jget_str(m0, "role"));
    ASSERT_STR_EQ("the summary", jget_str(m0, "content"));
    yyjson_val *m1 = yyjson_arr_get(arr, 1);
    ASSERT_STR_EQ("user", jget_str(m1, "role"));
    ASSERT_STR_EQ("new question", jget_str(m1, "content"));
    yyjson_doc_free(doc);
    free(in);

    /* boundary 0 with a summary: no summary item (matches the chat wire) */
    in = tny_openai_responses_input(session_messages(s), 0, "the summary");
    ASSERT(in);
    doc = jparse(in, strlen(in));
    arr = yyjson_doc_get_root(doc);
    ASSERT_EQ_FMT(3, (int)yyjson_arr_size(arr), "%d");
    ASSERT_STR_EQ("user", jget_str(yyjson_arr_get(arr, 0), "role"));
    yyjson_doc_free(doc);
    free(in);

    session_close(s);
    tny_ctx_free(ctx);
    PASS();
}

/* Image messages (chat text/image_url parts) become input_text/input_image
 * parts with the data URL as a plain string. */
TEST responses_input_translates_image_parts(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    tny_session_state *s = session_new(ctx);

    char pngpath[600];
    snprintf(pngpath, sizeof pngpath, "%s/dot2.png", g_ws);
    file_write_atomic(pngpath, PNG1, sizeof PNG1);
    const char *paths[] = {pngpath, NULL};
    char err[256];
    ASSERT_EQ(0, session_add_user_images(s, "look at this", paths, err, sizeof err));

    char *in = tny_openai_responses_input(session_messages(s), 0, NULL);
    ASSERT(in);
    yyjson_doc *doc = jparse(in, strlen(in));
    yyjson_val *arr = yyjson_doc_get_root(doc);
    ASSERT_EQ_FMT(1, (int)yyjson_arr_size(arr), "%d");
    yyjson_val *m = yyjson_arr_get(arr, 0);
    ASSERT_STR_EQ("user", jget_str(m, "role"));
    yyjson_val *parts = jget(m, "content");
    ASSERT(yyjson_is_arr(parts));
    ASSERT_EQ_FMT(2, (int)yyjson_arr_size(parts), "%d");
    yyjson_val *tp = yyjson_arr_get(parts, 0);
    ASSERT_STR_EQ("input_text", jget_str(tp, "type"));
    ASSERT_STR_EQ("look at this", jget_str(tp, "text"));
    yyjson_val *ip = yyjson_arr_get(parts, 1);
    ASSERT_STR_EQ("input_image", jget_str(ip, "type"));
    const char *url = jget_str(ip, "image_url");
    ASSERT(url); /* a string, not chat's {"url":…} object */
    ASSERT(str_starts(url, "data:image/png;base64,"));
    yyjson_doc_free(doc);
    free(in);
    session_close(s);
    tny_ctx_free(ctx);
    PASS();
}

/* Malformed stored messages (hand-edited sessions, buggy forks) must be
 * skipped, never crash the translation or leak into the wire. */
TEST responses_input_skips_malformed(void) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    ASSERT(d);
    yyjson_mut_val *msgs = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, msgs);

    /* user message whose parts have no type / an unknown type */
    yyjson_mut_val *m = yyjson_mut_obj(d);
    yyjson_mut_obj_put(m, yyjson_mut_strcpy(d, "role"), yyjson_mut_strcpy(d, "user"));
    yyjson_mut_val *parts = yyjson_mut_arr(d);
    yyjson_mut_val *p1 = yyjson_mut_obj(d); /* no "type" at all */
    yyjson_mut_obj_put(p1, yyjson_mut_strcpy(d, "text"), yyjson_mut_strcpy(d, "x"));
    yyjson_mut_arr_add_val(parts, p1);
    yyjson_mut_val *p2 = yyjson_mut_obj(d);
    yyjson_mut_obj_put(p2, yyjson_mut_strcpy(d, "type"), yyjson_mut_strcpy(d, "weird"));
    yyjson_mut_arr_add_val(parts, p2);
    yyjson_mut_val *p3 = yyjson_mut_obj(d); /* the one valid part */
    yyjson_mut_obj_put(p3, yyjson_mut_strcpy(d, "type"), yyjson_mut_strcpy(d, "text"));
    yyjson_mut_obj_put(p3, yyjson_mut_strcpy(d, "text"), yyjson_mut_strcpy(d, "ok"));
    yyjson_mut_arr_add_val(parts, p3);
    yyjson_mut_obj_put(m, yyjson_mut_strcpy(d, "content"), parts);
    yyjson_mut_arr_add_val(msgs, m);

    /* assistant message whose tool_calls is not an array */
    yyjson_mut_val *m2 = yyjson_mut_obj(d);
    yyjson_mut_obj_put(m2, yyjson_mut_strcpy(d, "role"), yyjson_mut_strcpy(d, "assistant"));
    yyjson_mut_obj_put(m2, yyjson_mut_strcpy(d, "content"), yyjson_mut_strcpy(d, "fine"));
    yyjson_mut_obj_put(m2, yyjson_mut_strcpy(d, "tool_calls"),
                       yyjson_mut_strcpy(d, "not an array"));
    yyjson_mut_arr_add_val(msgs, m2);

    /* message with no role at all */
    yyjson_mut_arr_add_val(msgs, yyjson_mut_obj(d));

    char *in = tny_openai_responses_input(msgs, 0, NULL);
    ASSERT(in);
    yyjson_doc *doc = jparse(in, strlen(in));
    ASSERT(doc);
    yyjson_val *arr = yyjson_doc_get_root(doc);
    ASSERT_EQ_FMT(2, (int)yyjson_arr_size(arr), "%d");
    yyjson_val *u = yyjson_arr_get(arr, 0);
    yyjson_val *up = jget(u, "content");
    ASSERT_EQ_FMT(1, (int)yyjson_arr_size(up), "%d"); /* junk parts dropped */
    ASSERT_STR_EQ("ok", jget_str(yyjson_arr_get(up, 0), "text"));
    yyjson_val *a = yyjson_arr_get(arr, 1);
    ASSERT_STR_EQ("assistant", jget_str(a, "role"));
    ASSERT_STR_EQ("fine", jget_str(a, "content"));
    yyjson_doc_free(doc);
    free(in);
    yyjson_mut_doc_free(d);

    /* text_format: a json_schema wrapper whose payload is not an object
     * degrades to the bare type, never crashes */
    char *fmt = tny_openai_responses_text_format("{\"type\":\"json_schema\",\"json_schema\":42}");
    ASSERT(fmt);
    yyjson_doc *fd = jparse(fmt, strlen(fmt));
    ASSERT_STR_EQ("json_schema", jget_str(yyjson_doc_get_root(fd), "type"));
    ASSERT_EQ(NULL, jget(yyjson_doc_get_root(fd), "schema"));
    yyjson_doc_free(fd);
    free(fmt);
    PASS();
}

/* The whole builtin tool schema must flatten: every entry keeps its name,
 * description, and parameters at the top level and loses the nested
 * "function" object the chat wire uses. */
TEST responses_tools_flatten(void) {
    tools_env env;
    memset(&env, 0, sizeof env);
    char *chat = tools_schema_json(&env);
    ASSERT(chat);
    char *flat = tny_openai_responses_tools(chat);
    ASSERT(flat);

    yyjson_doc *cd = jparse(chat, strlen(chat));
    yyjson_doc *fd = jparse(flat, strlen(flat));
    ASSERT(cd);
    ASSERT(fd);
    yyjson_val *ca = yyjson_doc_get_root(cd);
    yyjson_val *fa = yyjson_doc_get_root(fd);
    ASSERT_EQ(yyjson_arr_size(ca), yyjson_arr_size(fa));
    size_t idx, max;
    yyjson_val *t;
    yyjson_arr_foreach(fa, idx, max, t) {
        ASSERT_STR_EQ("function", jget_str(t, "type"));
        ASSERT(jget_str(t, "name"));
        ASSERT(jget_str(t, "description"));
        ASSERT(yyjson_is_obj(jget(t, "parameters")));
        ASSERT_EQ(NULL, jget(t, "function"));
        /* same tool, same position as the chat schema */
        yyjson_val *cfn = jget(yyjson_arr_get(ca, idx), "function");
        ASSERT_STR_EQ(jget_str(cfn, "name"), jget_str(t, "name"));
    }
    yyjson_doc_free(cd);
    yyjson_doc_free(fd);
    free(flat);
    free(chat);

    /* junk in, NULL out */
    ASSERT_EQ(NULL, tny_openai_responses_tools("{\"not\":\"an array\"}"));
    ASSERT_EQ(NULL, tny_openai_responses_tools("not json"));
    ASSERT_EQ(NULL, tny_openai_responses_tools(NULL));
    PASS();
}

TEST embedded_tool_schema_has_no_process_spawning_tools(void) {
    ensure_env();
    tny_ctx *ctx = tny_ctx_new_explicit(g_ws, g_home);
    ASSERT(ctx);
    perm_engine *perm = perm_new(ctx);
    ASSERT(perm);
    tools_env env = {.ctx = ctx, .perm = perm};
    char *schema = tools_schema_json(&env);
    ASSERT(schema);
    yyjson_doc *doc = jparse(schema, strlen(schema));
    ASSERT(doc);
    yyjson_val *item;
    size_t idx, max;
    yyjson_arr_foreach(yyjson_doc_get_root(doc), idx, max, item) {
        const char *name = jget_str(jget(item, "function"), "name");
        ASSERT(name);
        ASSERT(strcmp(name, "terminal") != 0);
        ASSERT(strcmp(name, "open_file") != 0);
        ASSERT(strcmp(name, "subagent") != 0);
    }
    tools_call call;
    ASSERT_EQ(-1, tools_call_prepare(&env, "terminal", "{\"command\":\"true\"}", &call));
    ASSERT(call.error && strstr(call.error, "unavailable"));
    tools_call_free(&call);
    yyjson_doc_free(doc);
    free(schema);
    perm_free(perm);
    tny_ctx_free(ctx);
    PASS();
}

TEST tool_profile_parsing_precedence_and_ignored_modes(void) {
    ensure_env();
    unsetenv("TNY_TOOLS");
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT(ctx);
    ASSERT_EQ(TNY_TOOLS_ALL, ctx->tool_profile);
    tny_ctx_free(ctx);

    write_settings("{\"tools\":\"terminal+edit\"}");
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_TOOLS_TERMINAL_EDIT, ctx->tool_profile);
    tny_ctx_free(ctx);

    setenv("TNY_TOOLS", "terminal", 1);
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_TOOLS_TERMINAL, ctx->tool_profile);
    tny_ctx_free(ctx);

    setenv("TNY_TOOLS", "invalid", 1); /* invalid env does not erase settings */
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_TOOLS_TERMINAL_EDIT, ctx->tool_profile);
    tny_ctx_free(ctx);

    /* Explicit/library contexts load neither settings nor environment. */
    ctx = tny_ctx_new_explicit(g_ws, g_home);
    ASSERT(ctx);
    ASSERT(ctx->library_mode);
    ASSERT_EQ(TNY_TOOLS_ALL, ctx->tool_profile);
    ctx->tool_profile = TNY_TOOLS_TERMINAL;
    perm_engine *library_perm = perm_new(ctx);
    tools_env library_env = {.ctx = ctx, .perm = library_perm};
    ASSERT(tool_schema_has(&library_env, "read_file"));
    ASSERT_FALSE(tool_schema_has(&library_env, "terminal")); /* library rule, not profile */
    perm_free(library_perm);
    tny_ctx_free(ctx);

    ctx = tny_ctx_load(g_ws);
    ASSERT(ctx);
    ctx->tool_profile = TNY_TOOLS_TERMINAL;
    tny_tool_profile_ignore(ctx, "ACP server mode");
    ASSERT_EQ(TNY_TOOLS_ALL, ctx->tool_profile);
    tny_ctx_free(ctx);

    unsetenv("TNY_TOOLS");
    write_settings("{}");
    PASS();
}

TEST tool_profile_filters_schema_enforces_and_keeps_custom_tools(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT(ctx);
    custom_tool_registry *registry = custom_tools_new();
    ASSERT(registry);
    tny_tool_registration *registration = register_profile_custom(registry);
    ASSERT(registration);
    ctx->custom_tools = registry;
    perm_engine *perm = perm_new(ctx);
    ASSERT(perm);
    tools_env env = {.ctx = ctx, .perm = perm};

    ctx->tool_profile = TNY_TOOLS_TERMINAL;
    ASSERT_EQ(3, tool_schema_count(&env));
    ASSERT(tool_schema_has(&env, "terminal"));
    ASSERT(tool_schema_has(&env, "read_image"));
    ASSERT(tool_schema_has(&env, "custom_profile_tool"));
    ASSERT_FALSE(tool_schema_has(&env, "edit_file"));
    ASSERT_FALSE(tool_schema_has(&env, "mcp_select_tool"));
    char *result = tools_execute(&env, "read_file", "{\"path\":\"AGENTS.md\"}");
    ASSERT(result);
    ASSERT_STR_EQ("error: unknown tool read_file", result);
    free(result);
    result = tools_execute(&env, "mcp_select_tool", "{\"server\":\"x\",\"tool\":\"y\"}");
    ASSERT_STR_EQ("error: unknown tool mcp_select_tool", result);
    free(result);
    result = tools_execute(&env, "custom_profile_tool", "{}");
    ASSERT_STR_EQ("custom ok", result);
    free(result);

    ctx->tool_profile = TNY_TOOLS_TERMINAL_EDIT;
    ASSERT_EQ(4, tool_schema_count(&env));
    ASSERT(tool_schema_has(&env, "edit_file"));
    ASSERT_FALSE(tool_schema_has(&env, "ask_user_question"));
    env.prompt = profile_prompt;
    ASSERT_EQ(5, tool_schema_count(&env));
    ASSERT(tool_schema_has(&env, "ask_user_question"));

    ctx->custom_tools = NULL;
    ASSERT_EQ(TNY_STATUS_OK, custom_tools_unregister(registration));
    custom_tools_free(registry);
    perm_free(perm);
    tny_ctx_free(ctx);
    PASS();
}

/* The subagent child command must forward the parent's resolved provider —
 * without it the child re-resolves from settings, where a remembered
 * last_provider (e.g. codex) beats environment detection and the child
 * fails at startup. Every model-supplied value must also be shell-quoted:
 * id and prompt reach a popen(3) shell. */
TEST subagent_command_forwards_provider_and_quotes(void) {
    ensure_env();
    tny_ctx *ctx = tny_ctx_new_explicit(g_ws, g_home);
    ASSERT(ctx);
    free(ctx->provider_name);
    ctx->provider_name = xstrdup("openrouter");
    free(ctx->base_url);
    ctx->base_url = xstrdup("https://example.test/v1");
    free(ctx->wire_api);
    ctx->wire_api = xstrdup("chat");
    free(ctx->model);
    ctx->model = xstrdup("mock-model");
    ctx->perm_mode = TNY_MODE_ASK;
    tools_env env = {.ctx = ctx};

    char *cmd =
        tools_subagent_command(&env, "x'; touch pwned; '", "say 'hi' $(date)", "/tmp/err file");
    ASSERT(cmd);
    ASSERT(strstr(cmd, " --provider 'openrouter'"));
    ASSERT(strstr(cmd, " --base-url 'https://example.test/v1'"));
    ASSERT(strstr(cmd, " --wire-api chat"));
    ASSERT(strstr(cmd, " --model 'mock-model'"));
    ASSERT(strstr(cmd, " --permission-mode ask"));
    ASSERT(strstr(cmd, " ask --json"));
    ASSERT_EQ(NULL, strstr(cmd, "--ephemeral"));
    /* an embedded single quote must be broken out of the quoted span, so
     * the injection attempt stays one argv string for the child */
    ASSERT(strstr(cmd, " --resume-id 'x'\\''; touch pwned; '\\'''"));
    ASSERT(strstr(cmd, " -- 'say '\\''hi'\\'' $(date)'"));
    ASSERT(strstr(cmd, " 2>'/tmp/err file'"));
    free(cmd);

    /* ephemeral parents pass the mode through; no stderr redirect when the
     * temp file could not be created */
    ctx->no_save = true;
    cmd = tools_subagent_command(&env, NULL, "hi", NULL);
    ASSERT(cmd);
    ASSERT(strstr(cmd, " --ephemeral ask --json -- 'hi'"));
    ASSERT_EQ(NULL, strstr(cmd, "--resume-id"));
    ASSERT_EQ(NULL, strstr(cmd, "2>"));
    free(cmd);
    tny_ctx_free(ctx);
    PASS();
}

/* Chat response_format wrappers flatten into the Responses text.format
 * object: json_schema members hoisted, no nested "json_schema" key. */
TEST responses_text_format_flattens(void) {
    const char *bare = "{\"type\":\"object\",\"properties\":{\"n\":{\"type\":\"integer\"}}}";
    char *rf = tny_openai_response_format(bare, strlen(bare));
    ASSERT(rf);
    char *fmt = tny_openai_responses_text_format(rf);
    ASSERT(fmt);
    yyjson_doc *doc = jparse(fmt, strlen(fmt));
    ASSERT(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_STR_EQ("json_schema", jget_str(root, "type"));
    ASSERT_STR_EQ("output", jget_str(root, "name"));
    ASSERT(jget_bool(root, "strict", false));
    ASSERT_STR_EQ("object", jget_str(jget(root, "schema"), "type"));
    ASSERT_EQ(NULL, jget(root, "json_schema"));
    yyjson_doc_free(doc);
    free(fmt);
    free(rf);

    /* a named wrapper keeps its name */
    const char *named = "{\"type\":\"json_schema\",\"json_schema\":"
                        "{\"name\":\"todo\",\"schema\":{\"type\":\"object\"}}}";
    fmt = tny_openai_responses_text_format(named);
    ASSERT(fmt);
    doc = jparse(fmt, strlen(fmt));
    ASSERT_STR_EQ("todo", jget_str(yyjson_doc_get_root(doc), "name"));
    yyjson_doc_free(doc);
    free(fmt);

    /* non-json_schema wrappers and junk return NULL */
    ASSERT_EQ(NULL, tny_openai_responses_text_format("{\"type\":\"text\"}"));
    ASSERT_EQ(NULL, tny_openai_responses_text_format("not json"));
    ASSERT_EQ(NULL, tny_openai_responses_text_format(NULL));
    PASS();
}

/* wire_api resolution: responses is the default; settings, env, and named
 * profiles opt into chat; OPENAI_WIRE_API beats the settings object. */
TEST wire_api_resolution(void) {
    ensure_env();
    unsetenv("OPENAI_WIRE_API");

    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(NULL, ctx->wire_api);
    ASSERT_FALSE(tny_wire_is_chat(ctx->wire_api));
    tny_ctx_free(ctx);
    ASSERT_FALSE(tny_wire_is_chat(NULL));
    ASSERT_FALSE(tny_wire_is_chat("responses"));
    ASSERT(tny_wire_is_chat("chat"));

    write_settings("{\"openai\":{\"wire_api\":\"chat\"}}");
    ctx = tny_ctx_load(g_ws);
    ASSERT(ctx->wire_api);
    ASSERT_STR_EQ("chat", ctx->wire_api);
    tny_ctx_free(ctx);

    setenv("OPENAI_WIRE_API", "responses", 1);
    ctx = tny_ctx_load(g_ws);
    ASSERT_STR_EQ("responses", ctx->wire_api);
    ASSERT_FALSE(tny_wire_is_chat(ctx->wire_api));
    tny_ctx_free(ctx);
    unsetenv("OPENAI_WIRE_API");

    /* a named settings profile carries its own wire */
    write_settings("{\"legacy\":{\"base_url\":\"https://legacy.test/v1\","
                   "\"wire_api\":\"chat\"}}");
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "legacy"));
    ASSERT_STR_EQ("chat", ctx->wire_api);
    /* switching back to the builtin restores the responses default */
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "openai"));
    ASSERT_EQ(NULL, ctx->wire_api);
    tny_ctx_free(ctx);

    /* NAME_WIRE_API beats the profile object */
    setenv("LEGACY_WIRE_API", "responses", 1);
    ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "legacy"));
    ASSERT_STR_EQ("responses", ctx->wire_api);
    tny_ctx_free(ctx);
    unsetenv("LEGACY_WIRE_API");
    write_settings("{}");
    PASS();
}

/* --wire-api: chat and responses parse; anything else is a startup error. */
TEST wire_api_flag(void) {
    ensure_env();
    unsetenv("OPENAI_WIRE_API");
    write_settings("{}");

    char *argv[] = {"tny", "--provider", "openai", "--wire-api", "chat", "ask", "hi", NULL};
    cli_globals g = {0};
    int ci = cli_parse_globals(7, argv, &g);
    ASSERT_EQ(5, ci);
    ASSERT_STR_EQ("chat", g.wire_api);
    g.cwd = g_ws;
    tny_ctx *ctx = cli_make_ctx(&g);
    ASSERT(ctx);
    ASSERT_STR_EQ("chat", ctx->wire_api);
    tny_ctx_free(ctx);

    cli_globals g2 = {0};
    g2.backend = "openai";
    g2.cwd = g_ws;
    g2.wire_api = "responses";
    ctx = cli_make_ctx(&g2);
    ASSERT(ctx);
    ASSERT_STR_EQ("responses", ctx->wire_api);
    tny_ctx_free(ctx);

    cli_globals g3 = {0};
    g3.backend = "openai";
    g3.cwd = g_ws;
    g3.wire_api = "grpc"; /* nonsense must fail at startup */
    ASSERT_EQ(NULL, cli_make_ctx(&g3));
    PASS();
}

/* --color / --no-color: always and never parse (both spellings); anything
 * else is a startup error (docs/adr/0026). */
TEST color_flag(void) {
    ensure_env();
    write_settings("{}");

    char *argv[] = {"tny", "--color", "always", "ask", "hi", NULL};
    cli_globals g = {0};
    int ci = cli_parse_globals(5, argv, &g);
    ASSERT_EQ(3, ci);
    ASSERT_STR_EQ("always", g.color);
    g.cwd = g_ws;
    tny_ctx *ctx = cli_make_ctx(&g);
    ASSERT(ctx);
    ASSERT(ctx->force_color);
    ASSERT_FALSE(ctx->no_color);
    tny_ctx_free(ctx);

    char *argv2[] = {"tny", "--color=never", "ask", "hi", NULL};
    cli_globals g2 = {0};
    ASSERT_EQ(2, cli_parse_globals(4, argv2, &g2));
    ASSERT_STR_EQ("never", g2.color);
    g2.cwd = g_ws;
    ctx = cli_make_ctx(&g2);
    ASSERT(ctx);
    ASSERT(ctx->no_color);
    ASSERT_FALSE(ctx->force_color);
    tny_ctx_free(ctx);

    char *argv3[] = {"tny", "--no-color", "ask", "hi", NULL};
    cli_globals g4 = {0};
    ASSERT_EQ(2, cli_parse_globals(4, argv3, &g4));
    ASSERT_STR_EQ("never", g4.color);

    /* Repeated CLI color flags follow normal last-flag-wins semantics. */
    char *argv4[] = {"tny", "--no-color", "--color=always", "ask", "hi", NULL};
    cli_globals g5 = {0};
    ASSERT_EQ(3, cli_parse_globals(5, argv4, &g5));
    ASSERT_STR_EQ("always", g5.color);

    char *argv5[] = {"tny", "--color=always", "--no-color", "ask", "hi", NULL};
    cli_globals g6 = {0};
    ASSERT_EQ(3, cli_parse_globals(5, argv5, &g6));
    ASSERT_STR_EQ("never", g6.color);

    cli_globals g7 = {0};
    g7.cwd = g_ws;
    g7.color = "grpc"; /* nonsense must fail at startup */
    ASSERT_EQ(NULL, cli_make_ctx(&g7));
    PASS();
}

/* TNY_VERSION is generated from git describe at build time (docs/adr/0014).
 * Assert shape, never a literal: non-empty, no v prefix, printable, no
 * whitespace or quotes that would break JSON/header embedding. */
TEST version_string_is_sane(void) {
    const char *v = TNY_VERSION;
    ASSERT(v[0] != '\0');
    ASSERT(v[0] != 'v');
    for (const char *p = v; *p; p++) {
        ASSERT(*p > 0x20 && *p < 0x7f);
        ASSERT(*p != '"');
        ASSERT(*p != '\\');
    }
    PASS();
}

/* docs/adr/0018: a key stored by `provider setup` is the fallback; any env
 * var still beats it, so shell-side rotation wins without editing files. */
TEST provider_profile_stored_api_key(void) {
    unsetenv("OPENAI_API_KEY");
    unsetenv("WIZPROV_API_KEY");
    write_settings("{\"wizprov\":{\"base_url\":\"http://127.0.0.1:1/v1\","
                   "\"api_key\":\"sk-stored\"}}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT_EQ(TNY_BK_OPENAI, tny_resolve_backend(ctx, "wizprov"));
    ASSERT(ctx->api_key);
    ASSERT_STR_EQ("sk-stored", ctx->api_key);
    tny_ctx_free(ctx);

    setenv("WIZPROV_API_KEY", "sk-from-env", 1);
    ctx = tny_ctx_load(g_ws);
    tny_resolve_backend(ctx, "wizprov");
    ASSERT_STR_EQ("sk-from-env", ctx->api_key);
    tny_ctx_free(ctx);
    unsetenv("WIZPROV_API_KEY");

    /* the builtin openai object takes a stored key the same way */
    write_settings("{\"openai\":{\"api_key\":\"sk-oa-stored\"}}");
    ctx = tny_ctx_load(g_ws);
    tny_resolve_backend(ctx, "openai");
    ASSERT_STR_EQ("sk-oa-stored", ctx->api_key);
    tny_ctx_free(ctx);
    PASS();
}

/* The native loop is unlimited by default (docs/adr/0024): max_steps 0.
 * The repo's .tny.json "steps" limit still caps it, and the shared
 * --max-steps / /max-steps parser takes caps plus the clearing tokens. */
TEST max_steps_default_and_overrides(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT(ctx);
    ASSERT_EQ(0, ctx->max_steps); /* unlimited out of the box */
    tny_ctx_free(ctx);

    char p[600];
    snprintf(p, sizeof p, "%s/.tny.json", g_ws);
    file_write_atomic(p, "{\"steps\":7}", strlen("{\"steps\":7}"));
    ctx = tny_ctx_load(g_ws);
    ASSERT(ctx);
    ASSERT_EQ(7, ctx->max_steps); /* repo limit still applies */
    tny_ctx_free(ctx);
    unlink(p);

    ASSERT_EQ(0, tny_parse_max_steps("0"));
    ASSERT_EQ(0, tny_parse_max_steps("unlimited"));
    ASSERT_EQ(0, tny_parse_max_steps("none"));
    ASSERT_EQ(30, tny_parse_max_steps("30"));
    ASSERT_EQ(-1, tny_parse_max_steps("abc"));
    ASSERT_EQ(-1, tny_parse_max_steps("-3"));
    ASSERT_EQ(-1, tny_parse_max_steps("3x"));
    ASSERT_EQ(-1, tny_parse_max_steps(""));
    ASSERT_EQ(-1, tny_parse_max_steps(NULL));
    PASS();
}

TEST extension_config_default_and_overrides(void) {
    ensure_env();
    unsetenv("TNY_EXTENSIONS");
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    ASSERT(ctx);
    ASSERT(ctx->extensions_enabled);
    ASSERT_EQ(0, ctx->max_extension_iterations);
    ASSERT_EQ(5000, ctx->extension_timeout_ms);
    tny_ctx_free(ctx);

    write_settings("{\"extensions\":{\"enabled\":true,"
                   "\"max_iterations\":7,\"timeout_ms\":1234}}");
    ctx = tny_ctx_load(g_ws);
    ASSERT(ctx);
    ASSERT(ctx->extensions_enabled);
    ASSERT_EQ(7, ctx->max_extension_iterations);
    ASSERT_EQ(1234, ctx->extension_timeout_ms);
    tny_ctx_free(ctx);

    char *argv[] = {
        "tny", "--max-extension-iterations", "unlimited", "--no-extensions", "ask", "hi", NULL};
    cli_globals g = {0};
    ASSERT_EQ(4, cli_parse_globals(6, argv, &g));
    ASSERT_STR_EQ("unlimited", g.max_extension_iterations);
    ASSERT(g.no_extensions);
    g.cwd = g_ws;
    ctx = cli_make_ctx(&g);
    ASSERT(ctx);
    ASSERT_FALSE(ctx->extensions_enabled);
    ASSERT_EQ(0, ctx->max_extension_iterations);
    tny_ctx_free(ctx);

    setenv("TNY_EXTENSIONS", "off", 1);
    ctx = tny_ctx_load(g_ws);
    ASSERT(ctx);
    ASSERT_FALSE(ctx->extensions_enabled);
    tny_ctx_free(ctx);
    unsetenv("TNY_EXTENSIONS");

    ctx = tny_ctx_new_explicit(g_ws, g_home);
    ASSERT(ctx);
    ASSERT_FALSE(ctx->extensions_enabled);
    tny_ctx_free(ctx);
    write_settings("{}");
    PASS();
}

TEST provider_write_profile_rules(void) {
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    char err[256];

    /* host providers and reserved settings keys are refused */
    tny_provider_fields f0 = {"http://h/v1", NULL, NULL, NULL, NULL};
    ASSERT_EQ(-1, tny_provider_write_profile(ctx, "codex", &f0, err, sizeof err));
    ASSERT_EQ(-1, tny_provider_write_profile(ctx, "models", &f0, err, sizeof err));
    ASSERT_EQ(-1, tny_provider_write_profile(ctx, "bad name", &f0, err, sizeof err));

    /* a new profile needs a base_url */
    tny_provider_fields f1 = {NULL, "sk-x", NULL, NULL, NULL};
    ASSERT_EQ(-1, tny_provider_write_profile(ctx, "newprov", &f1, err, sizeof err));

    /* create, then partial update keeps the untouched fields */
    tny_provider_fields f2 = {"http://127.0.0.1:1/v1", "sk-1", NULL, "m1", NULL};
    ASSERT_EQ(0, tny_provider_write_profile(ctx, "newprov", &f2, err, sizeof err));
    tny_provider_fields f3 = {NULL, NULL, NULL, "m2", NULL};
    ASSERT_EQ(0, tny_provider_write_profile(ctx, "newprov", &f3, err, sizeof err));
    tny_resolve_backend(ctx, "newprov");
    ASSERT_STR_EQ("http://127.0.0.1:1/v1", ctx->base_url);
    ASSERT_STR_EQ("sk-1", ctx->api_key);
    ASSERT(ctx->model);
    ASSERT_STR_EQ("m2", ctx->model);

    /* storing a key clears api_key_env and vice versa: one source of truth */
    tny_provider_fields f4 = {NULL, NULL, "NEWPROV_KEY_VAR", NULL, NULL};
    ASSERT_EQ(0, tny_provider_write_profile(ctx, "newprov", &f4, err, sizeof err));
    yyjson_val *o = jget(yyjson_doc_get_root(ctx->settings), "newprov");
    ASSERT(jget_str(o, "api_key") == NULL);
    ASSERT_STR_EQ("NEWPROV_KEY_VAR", jget_str(o, "api_key_env"));
    tny_provider_fields f5 = {NULL, "sk-2", NULL, NULL, NULL};
    ASSERT_EQ(0, tny_provider_write_profile(ctx, "newprov", &f5, err, sizeof err));
    o = jget(yyjson_doc_get_root(ctx->settings), "newprov");
    ASSERT(jget_str(o, "api_key_env") == NULL);
    ASSERT_STR_EQ("sk-2", jget_str(o, "api_key"));

    tny_ctx_free(ctx);
    PASS();
}

TEST embedded_public_runtime_does_not_claim_library_linkage(void) {
    tny_runtime_options_v0 options;
    ASSERT_EQ(TNY_STATUS_OK, tny_runtime_options_init(&options, sizeof options));
    options.workspace = (tny_bytes){g_ws, strlen(g_ws)};
    options.base_url = (tny_bytes){"http://127.0.0.1:1/v1", 21};
    options.api_key = (tny_bytes){"unit-key", 8};
    tny_runtime *runtime = NULL;
    ASSERT_EQ(TNY_STATUS_OK, tny_runtime_create(&options, sizeof options, &runtime, NULL));
    ASSERT(runtime);
    tny_capabilities_v0 capabilities;
    ASSERT_EQ(TNY_STATUS_OK, tny_capabilities_init(&capabilities, sizeof capabilities));
    ASSERT_EQ(TNY_STATUS_OK,
              tny_runtime_get_capabilities(runtime, &capabilities, sizeof capabilities));
    ASSERT_EQ(0, capabilities.feature_available_mask &
                     (TNY_CAP_FEATURE_SHARED_LIBRARY | TNY_CAP_FEATURE_STATIC_LIBRARY));
    ASSERT_EQ(0, capabilities.feature_enabled_mask &
                     (TNY_CAP_FEATURE_SHARED_LIBRARY | TNY_CAP_FEATURE_STATIC_LIBRARY));
    ASSERT_STR_EQ("embedded", capabilities.linkage.ptr);
    ASSERT_EQ(TNY_STATUS_OK, tny_runtime_destroy(&runtime));
    ASSERT(!runtime);
    PASS();
}

SUITE(core_suite) {
    RUN_TEST(backend_default_prefers_codex_login);
    RUN_TEST(backend_default_cursor_key_from_env);
    RUN_TEST(provider_last_used_and_scoped_models);
    RUN_TEST(custom_named_provider_profiles);
    RUN_TEST(settings_general_defaults);
    RUN_TEST(acp_named_provider_profiles);
    RUN_TEST(acp_profile_model_precedence);
    RUN_TEST(acp_profiles_validate_when_selected);
    RUN_TEST(acp_profiles_list_without_auto_select);
    RUN_TEST(provider_profile_stored_api_key);
    RUN_TEST(provider_write_profile_rules);
    RUN_TEST(embedded_public_runtime_does_not_claim_library_linkage);
    RUN_TEST(tool_profile_parsing_precedence_and_ignored_modes);
    RUN_TEST(tool_profile_filters_schema_enforces_and_keeps_custom_tools);
    RUN_TEST(max_steps_default_and_overrides);
    RUN_TEST(extension_config_default_and_overrides);
    RUN_TEST(env_defined_providers);
    RUN_TEST(builtin_claude_profile);
    RUN_TEST(builtin_grok_profile);
    RUN_TEST(grok_native_device_login);
    RUN_TEST(grok_native_login_denied);
    RUN_TEST(grok_native_refresh);
    RUN_TEST(grok_native_logout);
    RUN_TEST(builtin_profile_edge_credentials);
    RUN_TEST(builtin_profile_detection_and_shadowing);
    RUN_TEST(provider_names_joined_lists_detected);
    RUN_TEST(fast_capability_per_provider);
    RUN_TEST(fast_tier_spellings);
    RUN_TEST(fast_flag_sets_service_tier);
    RUN_TEST(fast_cursor_model_param);
    RUN_TEST(fast_flag_rejected_without_capability);
    RUN_TEST(perm_defaults_to_yolo);
    RUN_TEST(perm_ask_mode_opt_in);
    RUN_TEST(perm_mode_overrides_parse);
    RUN_TEST(effort_wire_mapping);
    RUN_TEST(effort_env_loads);
    RUN_TEST(effort_settings_global_default);
    RUN_TEST(effort_settings_per_provider);
    RUN_TEST(effort_env_beats_settings);
    RUN_TEST(effort_flag_beats_settings);
    RUN_TEST(perm_safe_tool_inside_workspace);
    RUN_TEST(perm_rules_last_match_wins);
    RUN_TEST(perm_workspace_beats_global);
    RUN_TEST(perm_session_grants);
    RUN_TEST(perm_auto_mode_heuristics);
    RUN_TEST(perm_yolo_allows_everything);
    RUN_TEST(tool_prepare_validates_rewrites_and_complete_permission_subjects);
    RUN_TEST(codex_registry_loopback_only);
    RUN_TEST(codex_registry_roundtrip);
    RUN_TEST(codex_registry_rejects_bad_entries);
    RUN_TEST(session_roundtrip);
    RUN_TEST(session_task_snapshot_roundtrip_and_resume_guards);
    RUN_TEST(session_task_snapshot_atomic_window_and_symlink_guards);
    RUN_TEST(session_tool_argument_rewrite_is_targeted_and_no_match_terminates);
    RUN_TEST(session_result_handles);
    RUN_TEST(session_compaction);
    RUN_TEST(session_recovery_roundtrip);
    RUN_TEST(image_mime_from_magic);
    RUN_TEST(image_data_url_roundtrip);
    RUN_TEST(read_image_queues_user_message);
    RUN_TEST(perm_read_image_is_safe);
    RUN_TEST(cmd_ask_image_overflow_frees_prompt);
    RUN_TEST(output_schema_wraps_bare_schema);
    RUN_TEST(output_schema_accepts_json_schema_object);
    RUN_TEST(output_schema_names_anonymous_json_schema_object);
    RUN_TEST(output_schema_passes_full_wrapper_through);
    RUN_TEST(output_schema_rejects_non_object);
    RUN_TEST(responses_input_translates_history);
    RUN_TEST(responses_input_honors_compact_boundary);
    RUN_TEST(responses_input_translates_image_parts);
    RUN_TEST(responses_input_skips_malformed);
    RUN_TEST(responses_tools_flatten);
    RUN_TEST(embedded_tool_schema_has_no_process_spawning_tools);
    RUN_TEST(subagent_command_forwards_provider_and_quotes);
    RUN_TEST(responses_text_format_flattens);
    RUN_TEST(wire_api_resolution);
    RUN_TEST(wire_api_flag);
    RUN_TEST(color_flag);
    RUN_TEST(version_string_is_sane);
}
