/* test_ssh.c — the --ssh remote tool runtime (docs/adr/0022).
 *
 * A fake `ssh` on PATH records its argv and runs the remote command string
 * with the local sh inside a sandbox "remote" directory, so every tool's
 * script, quoting, stdin plumbing, and result shape is exercised for real
 * without a network. */
#include "greatest.h"
#include "core/ssh.h"
#include "core/tools.h"
#include "core/config.h"
#include "core/instructions.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static char g_home[512], g_ws[600], g_remote[600], g_bin[600], g_log[600];

static void ensure_env(void) {
    if (g_home[0]) return;
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    snprintf(g_home, sizeof g_home, "%s%stny-ssh-home-XXXXXX", t,
             t[strlen(t) - 1] == '/' ? "" : "/");
    if (!mkdtemp(g_home)) abort();
    setenv("HOME", g_home, 1);
    unsetenv("TNY_PERMISSION_MODE");
    snprintf(g_ws, sizeof g_ws, "%s/ws", g_home);
    snprintf(g_remote, sizeof g_remote, "%s/remote", g_home);
    snprintf(g_bin, sizeof g_bin, "%s/bin", g_home);
    snprintf(g_log, sizeof g_log, "%s/ssh-argv.log", g_home);
    mkdir_p(g_ws);
    mkdir_p(g_remote);
    mkdir_p(g_bin);
    /* The fake: log argv, then behave like a remote login shell: run the
     * last argument through sh with HOME pointing at the sandbox. */
    char fake[700];
    snprintf(fake, sizeof fake, "%s/ssh", g_bin);
    buf_t b;
    buf_init(&b);
    buf_appendf(&b,
                "#!/bin/sh\n"
                "printf '%%s\\n' \"$@\" > '%s'\n"
                "for last; do :; done\n"
                "if [ \"$2\" = exit ] || [ \"$last\" = true ]; then exit 0; fi\n"
                "HOME='%s' exec sh -c \"$last\"\n",
                g_log, g_remote);
    file_write_atomic(fake, b.data, b.len);
    buf_free(&b);
    chmod(fake, 0755);
    buf_init(&b);
    buf_appendf(&b, "%s:%s", g_bin, getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
    setenv("PATH", b.data, 1);
    buf_free(&b);
}

static tny_ctx *remote_ctx(void) {
    ensure_env();
    tny_ctx *ctx = tny_ctx_load(g_ws);
    char err[256];
    if (ssh_target_set(ctx, "alice@example.test:2222", err, sizeof err) != 0) abort();
    ctx->ssh_cwd = xstrdup(g_remote);
    if (ssh_connect(ctx, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        abort();
    }
    return ctx;
}

static char *call(tools_env *env, const char *name, const char *args) {
    return tools_execute(env, name, args);
}

static char *slurp_log(void) {
    size_t n = 0;
    char *s = file_slurp(g_log, &n);
    return s ? s : xstrdup("");
}

TEST target_parsing(void) {
    ensure_env();
    tny_ctx *ctx = tny_ctx_load(g_ws);
    char err[256];
    ASSERT_EQ(0, ssh_target_set(ctx, "alice@example.test:2222", err, sizeof err));
    ASSERT_STR_EQ("alice@example.test", ctx->ssh_host);
    ASSERT_STR_EQ("2222", ctx->ssh_port);
    ASSERT_EQ(0, ssh_target_set(ctx, "box", err, sizeof err));
    ASSERT_STR_EQ("box", ctx->ssh_host);
    ASSERT_STR_EQ("", ctx->ssh_port);
    ASSERT_EQ(0, ssh_target_set(ctx, "bob@[2001:db8::1]:22", err, sizeof err));
    ASSERT_STR_EQ("bob@[2001:db8::1]", ctx->ssh_host);
    ASSERT_STR_EQ("22", ctx->ssh_port);
    ASSERT_EQ(-1, ssh_target_set(ctx, "host:0", err, sizeof err));
    ASSERT(strstr(err, "invalid SSH port"));
    ASSERT_EQ(-1, ssh_target_set(ctx, "host:70000", err, sizeof err));
    ASSERT_EQ(-1, ssh_target_set(ctx, "2001:db8::1:22", err, sizeof err));
    ASSERT(strstr(err, "[addr]:port"));
    ASSERT_EQ(-1, ssh_target_set(ctx, "a b", err, sizeof err));
    ASSERT_EQ(-1, ssh_target_set(ctx, "-oProxyCommand=x", err, sizeof err));
    ASSERT_EQ(-1, ssh_target_set(ctx, "@host", err, sizeof err));
    ASSERT_EQ(-1, ssh_target_set(ctx, "", err, sizeof err));
    tny_ctx_free(ctx);
    PASS();
}

/* connect resolves the remote cwd to an absolute path through ssh itself,
 * and every later call goes out with BatchMode + the control socket + -p. */
TEST connect_resolves_cwd_and_argv_shape(void) {
    tny_ctx *ctx = remote_ctx();
    ASSERT_STR_EQ(g_remote, ctx->ssh_cwd);
    ASSERT(ctx->ssh_control && strncmp(ctx->ssh_control, "ControlPath=", 12) == 0);
    ASSERT(strstr(ctx->ssh_control, "/.tny/ssh/%C"));
    buf_t out;
    buf_init(&out);
    bool tr, to;
    ASSERT_EQ(0, ssh_run(ctx, "echo hi", NULL, 0, 10, 1024, &out, &tr, &to));
    ASSERT_STR_EQ("hi\n", out.data);
    buf_free(&out);
    char *log = slurp_log();
    ASSERT(strstr(log, "BatchMode=yes\n"));
    ASSERT(strstr(log, "ControlMaster=auto\n"));
    ASSERT(strstr(log, "-p\n2222\n--\nalice@example.test\n"));
    /* the remote command cd's into the resolved dir, then runs POSIX sh */
    ASSERT(strstr(log, "&& exec sh -c 'echo hi'"));
    free(log);
    tny_ctx_free(ctx);
    PASS();
}

TEST run_stdin_timeout_and_cap(void) {
    tny_ctx *ctx = remote_ctx();
    buf_t out;
    bool tr, to;
    /* stdin round-trips, including a quote and a large body */
    buf_t big;
    buf_init(&big);
    for (int i = 0; i < 20000; i++) buf_appends(&big, "it's a line\n");
    buf_init(&out);
    ASSERT_EQ(0, ssh_run(ctx, "wc -c", big.data, big.len, 10, 1024, &out, &tr, &to));
    ASSERT(strstr(out.data, "240000"));
    buf_free(&out);
    buf_free(&big);
    /* timeout kills and reports */
    buf_init(&out);
    ASSERT_EQ(124, ssh_run(ctx, "sleep 5; echo late", NULL, 0, 1, 1024, &out, &tr, &to));
    ASSERT(to);
    buf_free(&out);
    /* output cap → truncated flag */
    buf_init(&out);
    ASSERT_EQ(0, ssh_run(ctx, "yes | head -c 50000", NULL, 0, 10, 100, &out, &tr, &to));
    ASSERT(tr);
    ASSERT(out.len <= 100 + 8192);
    buf_free(&out);
    /* exit status passes through */
    buf_init(&out);
    ASSERT_EQ(3, ssh_run(ctx, "exit 3", NULL, 0, 10, 100, &out, &tr, &to));
    buf_free(&out);
    tny_ctx_free(ctx);
    PASS();
}

TEST file_tools_round_trip(void) {
    tny_ctx *ctx = remote_ctx();
    perm_engine *perm = perm_new(ctx);
    tools_env env = {0};
    env.ctx = ctx;
    env.perm = perm;
    char *r;

    /* write: content on stdin, parents created, relative to the remote cwd */
    r = call(&env, "write_file",
             "{\"path\":\"sub/a.txt\",\"content\":\"hello 'quoted' $HOME\\nline2\\n\"}");
    ASSERT(strstr(r, "wrote 27 bytes to"));
    ASSERT(strstr(r, g_remote));
    free(r);
    char local[700];
    snprintf(local, sizeof local, "%s/sub/a.txt", g_remote);
    size_t n = 0;
    char *data = file_slurp(local, &n);
    ASSERT(data);
    ASSERT_STR_EQ("hello 'quoted' $HOME\nline2\n", data);
    free(data);
    /* the content never rides the command line */
    char *log = slurp_log();
    ASSERT(!strstr(log, "hello 'quoted'"));
    free(log);

    r = call(&env, "read_file", "{\"path\":\"sub/a.txt\"}");
    ASSERT_STR_EQ("hello 'quoted' $HOME\nline2\n", r);
    free(r);
    r = call(&env, "read_file", "{\"path\":\"sub/a.txt\",\"offset\":2,\"limit\":1}");
    ASSERT_STR_EQ("line2\n", r);
    free(r);
    r = call(&env, "read_file", "{\"path\":\"sub/missing.txt\"}");
    ASSERT(strncmp(r, "error: cannot read", 18) == 0);
    free(r);

    r = call(&env, "edit_file",
             "{\"path\":\"sub/a.txt\",\"old_string\":\"'quoted'\",\"new_string\":\"\\\"q\\\"\"}");
    ASSERT(strstr(r, "replaced 1 occurrence"));
    free(r);
    data = file_slurp(local, &n);
    ASSERT_STR_EQ("hello \"q\" $HOME\nline2\n", data);
    free(data);
    r = call(&env, "edit_file",
             "{\"path\":\"sub/a.txt\",\"old_string\":\"zzz\",\"new_string\":\"y\"}");
    ASSERT(strstr(r, "old_string not found"));
    free(r);
    r = call(&env, "edit_file",
             "{\"path\":\"sub/a.txt\",\"old_string\":\"l\",\"new_string\":\"L\"}");
    ASSERT(strstr(r, "occurs 3 times"));
    free(r);
    r = call(
        &env, "edit_file",
        "{\"path\":\"sub/a.txt\",\"old_string\":\"l\",\"new_string\":\"L\",\"replace_all\":true}");
    ASSERT(strstr(r, "replaced 3 occurrences"));
    free(r);

    r = call(&env, "list_files", "{\"path\":\".\"}");
    ASSERT(strstr(r, "sub/\n"));
    free(r);
    r = call(&env, "list_files", "{\"path\":\"nope\"}");
    ASSERT(strncmp(r, "error: cannot open", 18) == 0);
    free(r);

    r = call(&env, "create_folder", "{\"path\":\"d1/d2\"}");
    ASSERT(strstr(r, "created"));
    free(r);
    r = call(&env, "copy_file", "{\"path\":\"sub/a.txt\",\"new_path\":\"d1/d2/b.c\"}");
    ASSERT(strstr(r, "copied"));
    free(r);
    r = call(&env, "rename_file", "{\"path\":\"d1/d2/b.c\",\"new_path\":\"d1/c.c\"}");
    ASSERT(strstr(r, "renamed"));
    free(r);
    r = call(&env, "glob_files", "{\"pattern\":\"**/*.c\"}");
    ASSERT_STR_EQ("d1/c.c\n", r);
    free(r);
    r = call(&env, "glob_files", "{\"pattern\":\"*.zig\"}");
    ASSERT_STR_EQ("(no matches)", r);
    free(r);
    r = call(&env, "grep_files", "{\"pattern\":\"Line2\",\"case_insensitive\":true}");
    ASSERT(strstr(r, "sub/a.txt:2:Line2"));
    ASSERT(strstr(r, "d1/c.c:2:"));
    free(r);
    r = call(&env, "grep_files", "{\"pattern\":\"Line2\",\"path\":\"d1/c.c\"}");
    ASSERT(strstr(r, "2:Line2"));
    free(r);
    r = call(&env, "grep_files", "{\"pattern\":\"nothing-here\"}");
    ASSERT_STR_EQ("(no matches)", r);
    free(r);
    r = call(&env, "file_info", "{\"path\":\"sub/a.txt\"}");
    ASSERT(strstr(r, "a.txt: file, 22 bytes"));
    free(r);
    r = call(&env, "file_info", "{\"path\":\"d1\"}");
    ASSERT(strstr(r, "directory"));
    free(r);
    r = call(&env, "file_info", "{\"path\":\"ghost\"}");
    ASSERT(strstr(r, "error: cannot stat"));
    free(r);
    r = call(&env, "semantic_search", "{\"query\":\"hello line\"}");
    ASSERT(strstr(r, "sub/a.txt (score"));
    free(r);
    r = call(&env, "delete_file", "{\"path\":\"d1/c.c\"}");
    ASSERT(strstr(r, "deleted"));
    free(r);
    snprintf(local, sizeof local, "%s/d1/c.c", g_remote);
    ASSERT(!file_exists(local));
    /* absolute and ~ paths */
    r = call(&env, "write_file", "{\"path\":\"~/tilde.txt\",\"content\":\"t\"}");
    ASSERT(strstr(r, "wrote 1 bytes"));
    free(r);
    snprintf(local, sizeof local, "%s/tilde.txt", g_remote); /* fake HOME = sandbox */
    ASSERT(file_exists(local));
    r = call(&env, "open_file", "{\"path\":\"sub/a.txt\"}");
    ASSERT(strstr(r, "not available over --ssh"));
    free(r);
    perm_free(perm);
    tny_ctx_free(ctx);
    PASS();
}

TEST terminal_runs_remotely(void) {
    tny_ctx *ctx = remote_ctx();
    perm_engine *perm = perm_new(ctx);
    tools_env env = {0};
    env.ctx = ctx;
    env.perm = perm;
    char *r =
        tools_execute(&env, "terminal", "{\"command\":\"pwd; echo \\\"it's\\\" $HOME; exit 2\"}");
    ASSERT(strstr(r, "exit code: 2\n"));
    ASSERT(strstr(r, g_remote));
    ASSERT(strstr(r, "it's"));
    free(r);
    r = tools_execute(&env, "terminal", "{\"command\":\"sleep 3\",\"timeout_s\":1}");
    ASSERT(strstr(r, "timed out after 1s"));
    free(r);
    r = tools_execute(&env, "terminal", "{\"command\":\"true\"}");
    ASSERT(strstr(r, "exit code: 0\n(no output)"));
    free(r);
    r = tools_execute(&env, "terminal", "{\"command\":\"echo bg-done\",\"background\":true}");
    ASSERT(strstr(r, "started in background on alice@example.test: pid "));
    ASSERT(strstr(r, "log: "));
    free(r);
    /* the permission detail is the remote path, not a local realpath */
    r = tools_execute(&env, "read_file", "{\"path\":\"nothere\"}");
    ASSERT(strstr(r, g_remote));
    free(r);
    perm_free(perm);
    tny_ctx_free(ctx);
    PASS();
}

/* Without a target nothing changes: the ssh layer declines and local tools
 * answer (the workspace dir, not the sandbox). */
TEST local_when_not_attached(void) {
    ensure_env();
    tny_ctx *ctx = tny_ctx_load(g_ws);
    perm_engine *perm = perm_new(ctx);
    tools_env env = {0};
    env.ctx = ctx;
    env.perm = perm;
    unlink(g_log);
    char *r = tools_execute(&env, "write_file", "{\"path\":\"local.txt\",\"content\":\"x\"}");
    ASSERT(strstr(r, g_ws));
    free(r);
    ASSERT(!file_exists(g_log));
    ssh_disconnect(ctx); /* no-op when never attached */
    ASSERT(!ctx->ssh_host);
    perm_free(perm);
    tny_ctx_free(ctx);
    PASS();
}

/* --ssh must not feed the launch-dir AGENTS.md (wrong tree) and must load
 * the remote cwd's file, plus ~/.tny user rules with an SSH warning
 * (docs/adr/0040). */
TEST instructions_follow_the_remote_workspace(void) {
    ensure_env();
    char local_agents[700], remote_agents[700], user_agents[700], tny_dir[700];
    snprintf(local_agents, sizeof local_agents, "%s/AGENTS.md", g_ws);
    snprintf(remote_agents, sizeof remote_agents, "%s/AGENTS.md", g_remote);
    snprintf(tny_dir, sizeof tny_dir, "%s/.tny", g_home);
    mkdir_p(tny_dir);
    snprintf(user_agents, sizeof user_agents, "%s/AGENTS.md", tny_dir);
    ASSERT_EQ(0, file_write_atomic(local_agents, "LOCAL-LAUNCH-DIR-RULES\n", 23));
    ASSERT_EQ(0, file_write_atomic(remote_agents, "REMOTE-CWD-RULES\n", 17));
    ASSERT_EQ(0, file_write_atomic(user_agents, "USER-HOME-RULES\n", 16));

    tny_ctx *ctx = remote_ctx();
    buf_t out;
    buf_init(&out);
    instructions_collect(ctx, &out);
    ASSERT(out.data);
    ASSERT(strstr(out.data, "USER-HOME-RULES"));
    ASSERT(strstr(out.data, "User instructions from"));
    ASSERT(strstr(out.data, "do not treat paths in this file as the remote workspace"));
    ASSERT(strstr(out.data, "REMOTE-CWD-RULES"));
    ASSERT(strstr(out.data, "Remote project instructions from"));
    ASSERT(strstr(out.data, "alice@example.test"));
    ASSERT(!strstr(out.data, "LOCAL-LAUNCH-DIR-RULES"));
    ASSERT(!strstr(out.data, g_ws));
    buf_free(&out);

    ssh_disconnect(ctx);
    buf_init(&out);
    instructions_collect(ctx, &out);
    ASSERT(strstr(out.data, "LOCAL-LAUNCH-DIR-RULES"));
    ASSERT(strstr(out.data, "USER-HOME-RULES"));
    ASSERT(!strstr(out.data, "REMOTE-CWD-RULES"));
    ASSERT(!strstr(out.data, "Remote project instructions"));
    buf_free(&out);
    tny_ctx_free(ctx);
    PASS();
}

TEST disconnect_sends_control_exit(void) {
    tny_ctx *ctx = remote_ctx();
    ssh_disconnect(ctx);
    ASSERT(!ctx->ssh_host && !ctx->ssh_cwd && !ctx->ssh_control);
    char *log = slurp_log();
    ASSERT(strstr(log, "-O\nexit\n"));
    free(log);
    tny_ctx_free(ctx);
    PASS();
}

SUITE(ssh_suite) {
    RUN_TEST(target_parsing);
    RUN_TEST(connect_resolves_cwd_and_argv_shape);
    RUN_TEST(run_stdin_timeout_and_cap);
    RUN_TEST(file_tools_round_trip);
    RUN_TEST(terminal_runs_remotely);
    RUN_TEST(local_when_not_attached);
    RUN_TEST(instructions_follow_the_remote_workspace);
    RUN_TEST(disconnect_sends_control_exit);
}
