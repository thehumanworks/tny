/* test_mcp.c — MCP background warm-up, cached catalog, token search
 * (docs/adr/0049). Fake stdio JSONL servers under a throwaway $HOME so
 * nothing touches the real ~/.tny and no real MCP server is spawned. */
#include "greatest.h"
#include "core/config.h"
#include "core/tools.h"
#include "mcp/mcp.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char m_home[512], m_ws[520], m_bin[sizeof m_home + sizeof "/fake-mcp.sh"];

/* Fake MCP server: answers initialize / tools/list / tools/call in JSONL,
 * matching ids by counting id-bearing requests (tny numbers them 1..n per
 * server). Notifications fall through unanswered. */
static const char *FAKE_SERVER =
    "#!/bin/sh\n"
    "[ -n \"$1\" ] && sleep \"$1\"\n"
    "n=0\n"
    "while IFS= read -r line; do\n"
    "  case \"$line\" in\n"
    "  *'\"method\":\"initialize\"'*) n=$((n+1));\n"
    "    printf '{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"protocolVersion\":"
    "\"2025-06-18\"}}\\n' \"$n\" ;;\n"
    "  *'\"method\":\"tools/list\"'*) n=$((n+1));\n"
    "    printf '{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"tools\":["
    "{\"name\":\"deploy_app\",\"description\":\"Deploy the application to production\"},"
    "{\"name\":\"rollback\",\"description\":\"Roll back the most recent deploy\"}]}}\\n' "
    "\"$n\" ;;\n"
    "  *'\"method\":\"tools/call\"'*) n=$((n+1));\n"
    "    printf '{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":["
    "{\"type\":\"text\",\"text\":\"called ok\"}]}}\\n' \"$n\" ;;\n"
    "  esac\n"
    "done\n";

/* Reads stdin forever, answers nothing, exits on EOF: a server that hangs
 * in its handshake without outliving the test process. */
static const char *HUNG_SERVER = "#!/bin/sh\nwhile IFS= read -r line; do :; done\n";

static void mcp_test_env(void) {
    if (m_home[0]) return;
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    snprintf(m_home, sizeof m_home, "%s/tny-mcp-home-XXXXXX", t);
    if (!mkdtemp(m_home)) abort();
    setenv("HOME", m_home, 1);
    snprintf(m_ws, sizeof m_ws, "%s/ws", m_home);
    mkdir_p(m_ws);
    char tnydir[600];
    snprintf(tnydir, sizeof tnydir, "%s/.tny", m_home);
    mkdir_p(tnydir);
    char settings[620];
    snprintf(settings, sizeof settings, "%s/settings.json", tnydir);
    file_write_atomic(settings, "{}", 2);
    snprintf(m_bin, sizeof m_bin, "%s/fake-mcp.sh", m_home);
    file_write_atomic(m_bin, FAKE_SERVER, strlen(FAKE_SERVER));
    chmod(m_bin, 0755);
}

static void write_profile(const char *json) {
    char path[620];
    snprintf(path, sizeof path, "%s/.tny/mcp.json", m_home);
    if (json) file_write_atomic(path, json, strlen(json));
    else unlink(path);
}

static char *fast_profile(void) {
    static char json[700];
    snprintf(json, sizeof json, "{\"servers\":{\"srv\":{\"command\":[\"%s\"]}}}", m_bin);
    return json;
}

/* Poll the cached catalog until it names srv/deploy_app (warm finished). */
static bool catalog_ready(tny_ctx *ctx, int timeout_ms) {
    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        buf_t cat;
        buf_init(&cat);
        mcp_catalog_collect(ctx, &cat);
        bool ok = cat.data && strstr(cat.data, "srv/deploy_app");
        buf_free(&cat);
        if (ok) return true;
        if (now_ms() >= deadline) return false;
        usleep(20 * 1000);
    }
}

/* Session start spawns the warm-up off the loop (a server that sleeps before
 * answering must not delay mcp_warm_start), and the catalog then appears in
 * the system-prompt collector without any search call. */
TEST warm_start_does_not_block_and_catalog_appears(void) {
    mcp_test_env();
    mcp_shutdown_all(); /* clear a warm latch an earlier suite may have set */
    char json[700];
    snprintf(json, sizeof json, "{\"servers\":{\"srv\":{\"command\":[\"%s\",\"0.4\"]}}}", m_bin);
    write_profile(json);
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);

    int64_t t0 = now_ms();
    mcp_warm_start(ctx);
    ASSERT(now_ms() - t0 < 350); /* the 0.4s handshake runs on the thread */

    buf_t cat;
    buf_init(&cat);
    mcp_catalog_collect(ctx, &cat); /* mid-warm: empty, and non-blocking */
    ASSERT_EQ_FMT(0, (int)cat.len, "%d");
    buf_free(&cat);

    ASSERT(catalog_ready(ctx, 8000));
    buf_init(&cat);
    mcp_catalog_collect(ctx, &cat);
    ASSERT(strstr(cat.data, "srv/deploy_app — Deploy the application to production"));
    ASSERT(strstr(cat.data, "srv/rollback"));
    ASSERT(strstr(cat.data, "mcp_select_tool"));
    buf_free(&cat);

    mcp_shutdown_all();
    tny_ctx_free(ctx);
    PASS();
}

/* A select against a server still warming waits for the handshake (the
 * prewarm-take contract) and then calls through the warmed connection. */
TEST select_waits_for_warming_server(void) {
    mcp_test_env();
    mcp_shutdown_all(); /* clear a warm latch an earlier suite may have set */
    char json[700];
    snprintf(json, sizeof json, "{\"servers\":{\"srv\":{\"command\":[\"%s\",\"0.3\"]}}}", m_bin);
    write_profile(json);
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};

    mcp_warm_start(ctx);
    char *out = mcp_call_tool(&env, "srv", "deploy_app", "{}");
    ASSERT(out);
    ASSERT(strstr(out, "called ok"));
    free(out);

    mcp_shutdown_all();
    tny_ctx_free(ctx);
    PASS();
}

/* A server that never answers its handshake must not stall anything except
 * a call that names it: warm start, catalog, features, empty-query search,
 * and shutdown all return immediately while it hangs. */
TEST hung_server_does_not_stall(void) {
    mcp_test_env();
    mcp_shutdown_all(); /* clear a warm latch an earlier suite may have set */
    char hung[560];
    snprintf(hung, sizeof hung, "%s/hung-mcp.sh", m_home);
    file_write_atomic(hung, HUNG_SERVER, strlen(HUNG_SERVER));
    chmod(hung, 0755);
    char json[1400];
    snprintf(json, sizeof json,
             "{\"servers\":{\"slow\":{\"command\":[\"%s\"]},\"srv\":{\"command\":[\"%s\"]}}}", hung,
             m_bin);
    write_profile(json);
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};

    int64_t t0 = now_ms();
    mcp_warm_start(ctx);
    ASSERT(now_ms() - t0 < 350);

    ASSERT(catalog_ready(ctx, 8000)); /* srv arrives while slow hangs */
    t0 = now_ms();
    char *feats = mcp_features(&env);
    char *empty = mcp_search_tools(&env, "");
    ASSERT(now_ms() - t0 < 1000);
    ASSERT(strstr(feats, "slow (starting)"));
    ASSERT(strstr(feats, "srv (connected)"));
    ASSERT(strstr(empty, "srv/deploy_app")); /* cached list, no waiting */
    ASSERT(!strstr(empty, "slow/"));
    free(feats);
    free(empty);

    t0 = now_ms();
    mcp_shutdown_all(); /* abandons the warming slot instead of waiting */
    ASSERT(now_ms() - t0 < 500);
    tny_ctx_free(ctx);
    PASS();
}

/* Search is a token AND-match over name + description, not one contiguous
 * substring; an empty query lists the cached catalog. */
TEST search_matches_tokens_not_substring(void) {
    mcp_test_env();
    mcp_shutdown_all(); /* clear a warm latch an earlier suite may have set */
    write_profile(fast_profile());
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};
    mcp_warm_start(ctx);
    ASSERT(catalog_ready(ctx, 8000));

    /* "deploy production" is not a substring of either description */
    char *out = mcp_search_tools(&env, "deploy production");
    ASSERT(strstr(out, "srv/deploy_app"));
    ASSERT(!strstr(out, "srv/rollback")); /* has "deploy", lacks "production" */
    free(out);

    out = mcp_search_tools(&env, "Recent ROLL");
    ASSERT(strstr(out, "srv/rollback"));
    ASSERT(!strstr(out, "srv/deploy_app"));
    free(out);

    out = mcp_search_tools(&env, "zzz-no-such-tool");
    ASSERT(strstr(out, "no matching MCP tools"));
    free(out);

    out = mcp_search_tools(&env, NULL); /* empty query: the cached catalog */
    ASSERT(strstr(out, "srv/deploy_app"));
    ASSERT(strstr(out, "srv/rollback"));
    free(out);

    mcp_shutdown_all();
    tny_ctx_free(ctx);
    PASS();
}

/* A server whose command cannot start is silent everywhere until a call
 * names it; the call then reports the existing spawn error. */
TEST failed_server_silent_until_call(void) {
    mcp_test_env();
    mcp_shutdown_all(); /* clear a warm latch an earlier suite may have set */
    write_profile("{\"servers\":{\"broken\":{\"command\":[\"/nonexistent-tny-mcp\"]}}}");
    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};
    mcp_warm_start(ctx);

    buf_t cat;
    buf_init(&cat);
    int64_t deadline = now_ms() + 5000;
    char *feats = NULL;
    for (;;) { /* wait out the warm attempt so the state is FAILED, not WARMING */
        feats = mcp_features(&env);
        if (strstr(feats, "broken (failed to start")) break;
        free(feats);
        feats = NULL;
        ASSERT(now_ms() < deadline);
        usleep(20 * 1000);
    }
    free(feats);
    mcp_catalog_collect(ctx, &cat);
    ASSERT_EQ_FMT(0, (int)cat.len, "%d"); /* failure is invisible in context */
    buf_free(&cat);

    char *out = mcp_call_tool(&env, "broken", "x", "{}");
    ASSERT(strstr(out, "error: could not start MCP server broken"));
    free(out);

    mcp_shutdown_all();
    tny_ctx_free(ctx);
    PASS();
}

/* Workspace mcp.json files never load (cloning a repo must not start a
 * server), and `tny acp` server mode (mcp_disabled) never warms even with
 * a valid user profile. */
TEST workspace_profile_never_loads_and_acp_never_warms(void) {
    mcp_test_env();
    mcp_shutdown_all();  /* clear a warm latch an earlier suite may have set */
    write_profile(NULL); /* no ~/.tny/mcp.json */
    char wsjson[620];
    snprintf(wsjson, sizeof wsjson, "%s/mcp.json", m_ws);
    file_write_atomic(wsjson, fast_profile(), strlen(fast_profile()));
    char wstny[640];
    snprintf(wstny, sizeof wstny, "%s/.tny", m_ws);
    mkdir_p(wstny);
    snprintf(wstny, sizeof wstny, "%s/.tny/mcp.json", m_ws);
    file_write_atomic(wstny, fast_profile(), strlen(fast_profile()));

    tny_ctx *ctx = tny_ctx_load(m_ws);
    ASSERT(ctx);
    tools_env env = {.ctx = ctx};
    mcp_warm_start(ctx);
    usleep(100 * 1000);
    buf_t cat;
    buf_init(&cat);
    mcp_catalog_collect(ctx, &cat);
    ASSERT_EQ_FMT(0, (int)cat.len, "%d");
    buf_free(&cat);
    char *feats = mcp_features(&env);
    ASSERT(strstr(feats, "no MCP servers configured"));
    free(feats);
    mcp_shutdown_all();

    /* disabled runtime: a valid profile still must not warm or list */
    write_profile(fast_profile());
    ctx->mcp_disabled = true;
    mcp_warm_start(ctx);
    usleep(100 * 1000);
    buf_init(&cat);
    mcp_catalog_collect(ctx, &cat);
    ASSERT_EQ_FMT(0, (int)cat.len, "%d");
    buf_free(&cat);
    char *out = mcp_search_tools(&env, "deploy");
    ASSERT(strstr(out, "no MCP servers configured"));
    free(out);
    mcp_shutdown_all();
    tny_ctx_free(ctx);
    PASS();
}

SUITE(mcp_suite) {
    RUN_TEST(warm_start_does_not_block_and_catalog_appears);
    RUN_TEST(select_waits_for_warming_server);
    RUN_TEST(hung_server_does_not_stall);
    RUN_TEST(search_matches_tokens_not_substring);
    RUN_TEST(failed_server_silent_until_call);
    RUN_TEST(workspace_profile_never_loads_and_acp_never_warms);
}
