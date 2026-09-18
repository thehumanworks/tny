/* Issue #161: completion comes from the owning waitpid, never PID existence. */
#include "greatest.h"
#include "core/tools.h"
#include "util/terminal_task.h"
#include "util/jobs_host.h"
#include "util/tny_poll.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char root[512];
static tny_ctx *ctx;
static tools_env env;

static void setup(void *ud) {
    (void)ud;
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/tny-terminal-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(root)) abort();
    ctx = tny_ctx_load(root);
    if (!ctx) abort();
    free(ctx->tny_dir);
    ctx->tny_dir = xstrdup(root);
    free(ctx->sandbox_mode);
    ctx->sandbox_mode = xstrdup("none");
    ctx->perm_mode = TNY_MODE_YOLO;
    env = (tools_env){.ctx = ctx, .perm = perm_new(ctx)};
}

static void teardown(void *ud) {
    (void)ud;
    perm_free(env.perm);
    tny_ctx_free(ctx);
}

static yyjson_doc *call(const char *args) {
    char *out = tools_execute(&env, "terminal", args);
    yyjson_doc *doc = out ? jparse(out, strlen(out)) : NULL;
    if (!doc) fprintf(stderr, "terminal result: %s\n", out ? out : "(null)");
    free(out);
    return doc;
}

static yyjson_doc *collect(const char *id, int wait_s) {
    char args[128];
    snprintf(args, sizeof args, "{\"task_id\":\"%s\",\"wait_s\":%d}", id, wait_s);
    return call(args);
}

TEST terminal_background_collects_actual_exit(void) {
    const char *commands[] = {"exit 0", "exit 7", "exit 127", "kill -TERM $$"};
    const char *states[] = {"completed", "failed", "failed", "signalled"};
    const int codes[] = {0, 7, 127, -1};
    for (int i = 0; i < 4; i++) {
        /* Every profile has the same JSON, without a misleading `exit: 0`. */
        ctx->tool_profile = (tny_tool_profile)(i % 3);
        char args[192];
        snprintf(args, sizeof args, "{\"command\":\"%s\",\"background\":true}", commands[i]);
        yyjson_doc *launched = call(args);
        ASSERT(launched);
        yyjson_val *launch = yyjson_doc_get_root(launched);
        const char *id = jget_str(launch, "task_id");
        ASSERT(id);
        ASSERT(jget(launch, "collect"));
        yyjson_doc *done = collect(id, 5);
        ASSERT(done);
        yyjson_val *result = yyjson_doc_get_root(done);
        ASSERT_STR_EQ(states[i], jget_str(result, "state"));
        ASSERT_EQ(codes[i], jget_int(result, "exit_code", -1));
        ASSERT_EQ(i == 3 ? SIGTERM : 0, jget_int(result, "signal", 0));
        size_t len = 1;
        char *log = file_slurp(jget_str(result, "log"), &len);
        ASSERT(log);
        ASSERT_EQ(0, len);
        free(log);
        yyjson_doc_free(done);
        yyjson_doc_free(launched);
    }
    /* The cross-session integration checks the command's strict absence.
     * Never use a broad consuming wait even as a test postcondition. */
    PASS();
}

static bool cancelled(void *ud) { return *(bool *)ud; }

TEST terminal_wait_timeout_and_cancellation_only_stop_observation(void) {
    yyjson_doc *launched = call("{\"command\":\"sleep 2; exit 7\",\"background\":true}");
    ASSERT(launched);
    const char *id = jget_str(yyjson_doc_get_root(launched), "task_id");
    yyjson_doc *waiting = collect(id, 1);
    ASSERT(waiting);
    ASSERT_STR_EQ("running", jget_str(yyjson_doc_get_root(waiting), "state"));
    ASSERT_STR_EQ("timed_out", jget_str(yyjson_doc_get_root(waiting), "observation"));
    ASSERT(yyjson_is_null(jget(yyjson_doc_get_root(waiting), "exit_code")));
    yyjson_doc_free(waiting);
    bool cancel = true;
    env.cancelled = cancelled;
    env.cancelled_ud = &cancel;
    waiting = collect(id, 5);
    ASSERT(waiting);
    ASSERT_STR_EQ("cancelled", jget_str(yyjson_doc_get_root(waiting), "observation"));
    yyjson_doc_free(waiting);
    env.cancelled = NULL;
    waiting = collect(id, 5);
    ASSERT(waiting);
    ASSERT_STR_EQ("failed", jget_str(yyjson_doc_get_root(waiting), "state"));
    ASSERT_EQ(7, jget_int(yyjson_doc_get_root(waiting), "exit_code", -1));
    yyjson_doc_free(waiting);
    yyjson_doc_free(launched);
    PASS();
}

TEST terminal_multiple_tasks_do_not_steal_other_children_or_fds(void) {
    int unrelated_pipe[2];
    ASSERT_EQ(0, pipe(unrelated_pipe));
    pid_t provider = fork();
    ASSERT(provider >= 0);
    if (provider == 0) {
        close(unrelated_pipe[0]);
        close(unrelated_pipe[1]);
        _exit(23);
    }
    yyjson_doc *one = call("{\"command\":\"sleep 1; exit 4\",\"background\":true}");
    yyjson_doc *two = call("{\"command\":\"exit 9\",\"background\":true}");
    ASSERT(one && two);
    close(unrelated_pipe[1]);
    struct pollfd pf = {unrelated_pipe[0], POLLIN, 0};
    ASSERT(tny_poll(&pf, 1, 500) > 0);
    char byte;
    ASSERT_EQ(0, read(unrelated_pipe[0], &byte, 1));
    close(unrelated_pipe[0]);
    char *foreground = tools_execute(&env, "terminal", "{\"command\":\"exit 12\"}");
    ASSERT(strstr(foreground, "exit code: 12"));
    free(foreground);
    int status = 0;
    ASSERT_EQ(provider, waitpid(provider, &status, 0));
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(23, WEXITSTATUS(status));
    yyjson_doc *done = collect(jget_str(yyjson_doc_get_root(one), "task_id"), 5);
    ASSERT(done);
    ASSERT_EQ(4, jget_int(yyjson_doc_get_root(done), "exit_code", -1));
    yyjson_doc_free(done);
    done = collect(jget_str(yyjson_doc_get_root(two), "task_id"), 5);
    ASSERT(done);
    ASSERT_EQ(9, jget_int(yyjson_doc_get_root(done), "exit_code", -1));
    yyjson_doc_free(done);
    yyjson_doc_free(one);
    yyjson_doc_free(two);
    PASS();
}

TEST terminal_caller_loss_preserves_completion(void) {
    int channel[2];
    ASSERT_EQ(0, pipe(channel));
    pid_t caller = fork();
    ASSERT(caller >= 0);
    if (caller == 0) {
        close(channel[0]);
        close(STDIN_FILENO); /* descriptor handover must not alias stdio */
        yyjson_doc *launched = call("{\"command\":\"sleep 1; exit 17\",\"background\":true}");
        const char *id = launched ? jget_str(yyjson_doc_get_root(launched), "task_id") : NULL;
        if (id) (void)write(channel[1], id, 16);
        _exit(id ? 0 : 1);
    }
    close(channel[1]);
    char id[17] = {0};
    ASSERT_EQ(16, read(channel[0], id, 16));
    close(channel[0]);
    int status;
    ASSERT_EQ(caller, waitpid(caller, &status, 0));
    ASSERT_EQ(0, WEXITSTATUS(status));
    yyjson_doc *done = collect(id, 5);
    ASSERT(done);
    ASSERT_EQ(17, jget_int(yyjson_doc_get_root(done), "exit_code", -1));
    yyjson_doc_free(done);
    PASS();
}

TEST terminal_owner_loss_is_unknown_not_success(void) {
    yyjson_doc *launched =
        call("{\"command\":\"kill -KILL \\\"$PPID\\\"; exit 7\",\"background\":true}");
    ASSERT(launched);
    const char *id = jget_str(yyjson_doc_get_root(launched), "task_id");
    yyjson_doc *done = collect(id, 5);
    ASSERT(done);
    ASSERT_STR_EQ("unknown", jget_str(yyjson_doc_get_root(done), "state"));
    ASSERT(yyjson_is_null(jget(yyjson_doc_get_root(done), "exit_code")));
    yyjson_doc_free(done);
    yyjson_doc_free(launched);
    PASS();
}

TEST terminal_launch_failure_and_invalid_identity(void) {
    char *saved = ctx->cwd;
    ctx->cwd = "/tny-test-directory-that-does-not-exist";
    yyjson_doc *launched = call("{\"command\":\"exit 0\",\"background\":true}");
    ctx->cwd = saved;
    ASSERT(launched);
    yyjson_doc *done = collect(jget_str(yyjson_doc_get_root(launched), "task_id"), 5);
    ASSERT(done);
    ASSERT_STR_EQ("launch_failed", jget_str(yyjson_doc_get_root(done), "state"));
    ASSERT_EQ(ENOENT, jget_int(yyjson_doc_get_root(done), "error_code", 0));
    ASSERT(yyjson_is_null(jget(yyjson_doc_get_root(done), "exit_code")));
    yyjson_doc_free(done);
    yyjson_doc_free(launched);
    char *out = tools_execute(&env, "terminal", "{\"task_id\":\"../../elsewhere\"}");
    ASSERT(strstr(out, "error:"));
    free(out);
    out =
        tools_execute(&env, "terminal", "{\"task_id\":\"0123456789abcdef\",\"command\":\"true\"}");
    ASSERT(strstr(out, "cannot be combined"));
    free(out);
    done = collect("0123456789abcdef", 0);
    ASSERT(done);
    ASSERT_STR_EQ("unknown", jget_str(yyjson_doc_get_root(done), "state"));
    yyjson_doc_free(done);
    PASS();
}

TEST terminal_missing_and_corrupt_records_never_report_success(void) {
    char *tasks = path_join(root, "terminal");
    char *dir = path_join(tasks, "0123456789abcdef");
    ASSERT_EQ(0, mkdir_p(dir));
    char *path = path_join(dir, "status.json");
    const char *records[] = {"not json", "{\"state\":2}", "{\"state\":1,\"exit_code\":0}",
                             "{\"state\":4,\"signal\":0}", ""};
    for (size_t i = 0; i < sizeof records / sizeof records[0]; i++) {
        ASSERT_EQ(0, file_write_atomic(path, records[i], strlen(records[i])));
        yyjson_doc *done = collect("0123456789abcdef", 0);
        ASSERT(done);
        ASSERT_STR_EQ("unknown", jget_str(yyjson_doc_get_root(done), "state"));
        ASSERT(yyjson_is_null(jget(yyjson_doc_get_root(done), "exit_code")));
        yyjson_doc_free(done);
    }
    free(path);
    free(dir);
    free(tasks);
    PASS();
}

TEST terminal_nested_ceiling_and_permission_identity(void) {
    tools_call prepared;
    ASSERT_EQ(
        0, tools_call_prepare(&env, "terminal", "{\"task_id\":\"0123456789abcdef\"}", &prepared));
    ASSERT_STR_EQ("terminal", prepared.permission_tool);
    tools_call_free(&prepared);
    ctx->perm_mode = TNY_MODE_AUTO;
    yyjson_doc *args = jparse("{\"command\":\"printf '%s %s' \\\"$TNY_NESTED\\\" "
                              "\\\"$TNY_NESTED_MODE\\\"\",\"background\":true}",
                              strlen("{\"command\":\"printf '%s %s' \\\"$TNY_NESTED\\\" "
                                     "\\\"$TNY_NESTED_MODE\\\"\",\"background\":true}"));
    ASSERT(args);
    bool handled = false;
    char *out = tool_shell_execute(&env, "terminal", yyjson_doc_get_root(args), &handled);
    ASSERT(handled);
    yyjson_doc *launched = jparse(out, strlen(out));
    free(out);
    yyjson_doc_free(args);
    ASSERT(launched);
    /* Exercise execution after a permission decision, without asking a user. */
    ctx->perm_mode = TNY_MODE_YOLO;
    yyjson_doc *done = collect(jget_str(yyjson_doc_get_root(launched), "task_id"), 5);
    ASSERT(done);
    char *log = file_slurp(jget_str(yyjson_doc_get_root(done), "log"), NULL);
    ASSERT(log);
    ASSERT_STR_EQ("1 auto", log);
    free(log);
    yyjson_doc_free(done);
    yyjson_doc_free(launched);
    PASS();
}

SUITE(terminal_task_suite) {
    SET_SETUP(setup, NULL);
    SET_TEARDOWN(teardown, NULL);
    RUN_TEST(terminal_background_collects_actual_exit);
    RUN_TEST(terminal_wait_timeout_and_cancellation_only_stop_observation);
    RUN_TEST(terminal_multiple_tasks_do_not_steal_other_children_or_fds);
    RUN_TEST(terminal_caller_loss_preserves_completion);
    RUN_TEST(terminal_owner_loss_is_unknown_not_success);
    RUN_TEST(terminal_launch_failure_and_invalid_identity);
    RUN_TEST(terminal_missing_and_corrupt_records_never_report_success);
    RUN_TEST(terminal_nested_ceiling_and_permission_identity);
}
