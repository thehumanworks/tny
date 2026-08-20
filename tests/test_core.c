/* test_core.c — permission rules, sessions, compaction. Uses a throwaway
 * $HOME so nothing touches the real ~/.tny. */
#include "greatest.h"
#include "core/config.h"
#include "core/backend.h"
#include "core/perm.h"
#include "core/session.h"
#include "backends/codex/codex.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_home[512], g_ws[512];

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
    ASSERT_EQ(0, (int)(st.st_mode & 077)); /* private to the user */
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

SUITE(core_suite) {
    RUN_TEST(backend_default_prefers_codex_login);
    RUN_TEST(backend_default_cursor_key_from_env);
    RUN_TEST(provider_last_used_and_scoped_models);
    RUN_TEST(perm_defaults_to_yolo);
    RUN_TEST(perm_ask_mode_opt_in);
    RUN_TEST(perm_mode_overrides_parse);
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
}
