/* test_ephemeral.c — no-write session, history, and host-wire invariants. */
#include "greatest.h"
#include "cli/cli.h"
#include "core/config.h"
#include "core/session.h"
#include "backends/codex/codex.h"
#include "tui/tui.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char root[512];
    char workspace[540];
    char *old_home;
} ephemeral_env;

static void ephemeral_env_begin(ephemeral_env *e) {
    memset(e, 0, sizeof *e);
    const char *old = getenv("HOME");
    if (old) e->old_home = xstrdup(old);
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(e->root, sizeof e->root, "%s/tny-ephemeral-test-XXXXXX", tmp);
    if (!mkdtemp(e->root)) abort();
    setenv("HOME", e->root, 1);
    snprintf(e->workspace, sizeof e->workspace, "%s/workspace", e->root);
    if (mkdir_p(e->workspace) != 0) abort();
}

static void ephemeral_env_end(ephemeral_env *e) {
    if (e->old_home) setenv("HOME", e->old_home, 1);
    else unsetenv("HOME");
    free(e->old_home);
}

TEST ephemeral_flags_parse_globally(void) {
    cli_globals g = {0};
    char *canonical[] = {(char *)"tny", (char *)"--ephemeral", (char *)"status"};
    ASSERT_EQ(2, cli_parse_globals(3, canonical, &g));
    ASSERT(g.ephemeral);

    memset(&g, 0, sizeof g);
    char *alias[] = {(char *)"tny", (char *)"--no-save", (char *)"status"};
    ASSERT_EQ(2, cli_parse_globals(3, alias, &g));
    ASSERT(g.ephemeral);
    PASS();
}

TEST ephemeral_session_artifacts_stay_in_memory(void) {
    ephemeral_env e;
    ephemeral_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);
    ctx->no_save = true;

    tny_session *s = session_new(ctx);
    ASSERT(s);
    session_add_text(s, "user", "keep this only in memory");
    ASSERT_EQ(0, session_save(s));
    ASSERT(access(s->dir, F_OK) != 0);

    char *handle = session_store_result(s, "0123456789", 10);
    ASSERT(handle);
    ASSERT(access(s->dir, F_OK) != 0);
    size_t n = 0;
    char *slice = session_read_result(s, handle, 3, 4, &n);
    ASSERT(slice);
    ASSERT_EQ_FMT((size_t)4, n, "%zu");
    ASSERT_STR_EQ("3456", slice);
    free(slice);
    free(handle);

    session_recovery_write(s, "partial response");
    ASSERT(access(s->dir, F_OK) != 0);
    ASSERT_EQ(NULL, session_recovery_read(s));
    ASSERT_EQ(NULL, session_open(ctx, s->id));

    session_close(s);
    tny_ctx_free(ctx);
    ephemeral_env_end(&e);
    PASS();
}

TEST ephemeral_tui_history_is_process_local(void) {
    ephemeral_env e;
    ephemeral_env_begin(&e);
    tny_ctx *ctx = tny_ctx_load(e.workspace);
    ASSERT(ctx);
    ctx->no_save = true;

    tui t;
    memset(&t, 0, sizeof t);
    t.ctx = ctx;
    tui_hist_add(&t, "first prompt");
    tui_hist_add(&t, "second prompt");
    ASSERT_EQ(2, t.n_hist);
    ASSERT_STR_EQ("second prompt", t.hist[1]);

    char *history = path_join(ctx->tny_dir, "history");
    ASSERT(access(history, F_OK) != 0);
    free(history);

    tui_hist_free(&t);
    tny_ctx_free(ctx);
    ephemeral_env_end(&e);
    PASS();
}

TEST ephemeral_codex_thread_start_sets_wire_flag(void) {
    tny_ctx ctx = {0};
    ctx.no_save = true;
    cx_impl cx = {0};
    cx.ctx = &ctx;
    cx.next_id = 1;

    ASSERT(cx_request(&cx, "thread/start", "{\"model\":\"gpt-5\"}", CXR_FREE) > 0);
    ASSERT_EQ(1, cx.n_out);
    ASSERT(strstr(cx.outq[0], "\"ephemeral\":true"));
    ASSERT(strstr(cx.outq[0], "\"model\":\"gpt-5\""));
    ASSERT_EQ(0, cx_flush(&cx));
    free(cx.outq);

    memset(&cx, 0, sizeof cx);
    ctx.no_save = false;
    cx.ctx = &ctx;
    cx.next_id = 1;
    ASSERT(cx_request(&cx, "thread/start", "{}", CXR_FREE) > 0);
    ASSERT_EQ(1, cx.n_out);
    ASSERT_FALSE(strstr(cx.outq[0], "\"ephemeral\""));
    ASSERT_EQ(0, cx_flush(&cx));
    free(cx.outq);
    PASS();
}

SUITE(ephemeral_suite) {
    RUN_TEST(ephemeral_flags_parse_globally);
    RUN_TEST(ephemeral_session_artifacts_stay_in_memory);
    RUN_TEST(ephemeral_tui_history_is_process_local);
    RUN_TEST(ephemeral_codex_thread_start_sets_wire_flag);
}
