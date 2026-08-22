/* test_core.c — permission rules, sessions, compaction. Uses a throwaway
 * $HOME so nothing touches the real ~/.tny. */
#include "greatest.h"
#include "core/config.h"
#include "core/backend.h"
#include "core/perm.h"
#include "core/session.h"
#include "core/tools.h"
#include "core/image.h"
#include "backends/codex/codex.h"
#include "backends/openai/openai.h"
#include "backends/cursor/cursor.h"
#include "cli/cli.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    buf_appendf(&s, "{\"permission_mode\":\"yolo\","
                    "\"workspaces\":{\"%s\":{\"permission_mode\":\"ask\"}}}", ctx->cwd);
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

/* ---- sessions ---- */

TEST session_roundtrip(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    tny_session *s = session_new(ctx);
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

TEST session_result_handles(void) {
    ensure_env();
    write_settings("{}");
    tny_ctx *ctx = tny_ctx_load(g_ws);
    tny_session *s = session_new(ctx);
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
    tny_session *s = session_new(ctx);
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
    tny_session *s = session_new(ctx);
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
    write_settings(
        "{\"openrouter\":{\"base_url\":\"https://openrouter.ai/api/v1\","
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
    ASSERT_EQ(-1, tny_resolve_backend(ctx, "nope")); /* unknown still fails */
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
    ASSERT_STR_EQ("openai|cursor|codex|acp|openrouter|xai|orwell", j);
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
        "ws://127.0.0.1:8080", "ws://localhost:1234", "ws://127.0.0.1:65535/",
    };
    static const char *const bad[] = {
        "", "wss://127.0.0.1:443", "http://127.0.0.1:80",
        "ws://127.0.0.1", "ws://127.0.0.1:", "ws://127.0.0.1:0",
        "ws://127.0.0.1:65536", "ws://127.0.0.2:80", "ws://10.0.0.5:80",
        "ws://localhost.evil.io:80", "ws://evil:80",
        "ws://127.0.0.1:80/path", "ws://127.0.0.1:80x", "ws://[::1]:80",
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

    snprintf(json, sizeof json, "{\"ws\":\"ws://10.0.0.5:4242\",\"pid\":%ld}",
             (long)getpid());
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
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00,
    0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
};

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
    tny_session *s = session_new(ctx);
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
    const char *url = yyjson_mut_get_str(
        yyjson_mut_obj_get(yyjson_mut_obj_get(img, "image_url"), "url"));
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

SUITE(core_suite) {
    RUN_TEST(backend_default_prefers_codex_login);
    RUN_TEST(backend_default_cursor_key_from_env);
    RUN_TEST(provider_last_used_and_scoped_models);
    RUN_TEST(custom_named_provider_profiles);
    RUN_TEST(env_defined_providers);
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
    RUN_TEST(codex_registry_loopback_only);
    RUN_TEST(codex_registry_roundtrip);
    RUN_TEST(codex_registry_rejects_bad_entries);
    RUN_TEST(session_roundtrip);
    RUN_TEST(session_result_handles);
    RUN_TEST(session_compaction);
    RUN_TEST(session_recovery_roundtrip);
    RUN_TEST(image_mime_from_magic);
    RUN_TEST(image_data_url_roundtrip);
    RUN_TEST(read_image_queues_user_message);
    RUN_TEST(perm_read_image_is_safe);
    RUN_TEST(output_schema_wraps_bare_schema);
    RUN_TEST(output_schema_accepts_json_schema_object);
    RUN_TEST(output_schema_names_anonymous_json_schema_object);
    RUN_TEST(output_schema_passes_full_wrapper_through);
    RUN_TEST(output_schema_rejects_non_object);
    RUN_TEST(version_string_is_sane);
}
