/* test_sandbox.c — pure profile/argv construction and denial diagnostics. */
#include "greatest.h"
#include "core/sandbox.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool argv_has(const tny_sandbox_command *command, const char *value) {
    for (size_t i = 0; i < command->argc; i++)
        if (strcmp(command->argv[i], value) == 0) return true;
    return false;
}

TEST effective_mode_honors_yolo_and_configuration(void) {
    tny_ctx ctx = {0};
    ctx.sandbox_mode = "auto";
    ctx.perm_mode = TNY_MODE_YOLO;
    ASSERT_EQ(TNY_SANDBOX_NONE, tny_sandbox_effective(&ctx));
    ctx.perm_mode = TNY_MODE_ASK;
    ASSERT_EQ(tny_sandbox_available(), tny_sandbox_effective(&ctx));
    ctx.sandbox_mode = "os";
    ASSERT_EQ(tny_sandbox_available(), tny_sandbox_effective(&ctx));
    ctx.sandbox_mode = "none";
    ASSERT_EQ(TNY_SANDBOX_NONE, tny_sandbox_effective(&ctx));
    ASSERT_STR_EQ("none", tny_sandbox_kind_name(TNY_SANDBOX_NONE));
    ASSERT_STR_EQ("os", tny_sandbox_kind_name(TNY_SANDBOX_SEATBELT));
    ASSERT_STR_EQ("os", tny_sandbox_kind_name(TNY_SANDBOX_BWRAP));
    PASS();
}

TEST unsandboxed_argv_is_the_original_shell_command(void) {
    tny_ctx ctx = {.cwd = "/workspace"};
    tny_sandbox_command command = {0};
    char err[128] = {0};
    ASSERT_EQ(0, tny_sandbox_command_build_kind(&ctx, TNY_SANDBOX_NONE, NULL, "/bin/sh",
                                                "printf ok", &command, err, sizeof err));
    ASSERT_EQ(3, command.argc);
    ASSERT_STR_EQ("/bin/sh", command.argv[0]);
    ASSERT_STR_EQ("-c", command.argv[1]);
    ASSERT_STR_EQ("printf ok", command.argv[2]);
    ASSERT_EQ(NULL, command.argv[3]);
    tny_sandbox_command_free(&command);
    PASS();
}

TEST seatbelt_profile_limits_writes_and_escapes_paths(void) {
    char *extra_dirs[] = {"/private/tmp/extra \"quoted\""};
    tny_ctx ctx = {.cwd = "/private/tmp/work space", .extra_dirs = extra_dirs, .n_extra_dirs = 1};
    tny_sandbox_command command = {0};
    char err[128] = {0};
    ASSERT_EQ(0, tny_sandbox_command_build_kind(&ctx, TNY_SANDBOX_SEATBELT, "/usr/bin/sandbox-exec",
                                                "/bin/sh", "true", &command, err, sizeof err));
    ASSERT_EQ(6, command.argc);
    ASSERT_STR_EQ("/usr/bin/sandbox-exec", command.argv[0]);
    ASSERT_STR_EQ("-p", command.argv[1]);
    ASSERT(strstr(command.argv[2], "(deny default)"));
    ASSERT(strstr(command.argv[2], "(allow file-read*)"));
    ASSERT(strstr(command.argv[2], "(subpath \"/private/tmp/work space\")"));
    ASSERT(strstr(command.argv[2], "extra \\\"quoted\\\""));
    ASSERT(strstr(command.argv[2], "(allow network-outbound)"));
    ASSERT(strstr(command.argv[2], "localhost:*"));
    ASSERT_STR_EQ("/bin/sh", command.argv[3]);
    ASSERT_STR_EQ("true", command.argv[5]);
    tny_sandbox_command_free(&command);
    PASS();
}

TEST bubblewrap_argv_ro_binds_root_and_widens_named_dirs(void) {
    char *extra_dirs[] = {"/private/tmp/extra"};
    tny_ctx ctx = {.cwd = "/private/tmp/work", .extra_dirs = extra_dirs, .n_extra_dirs = 1};
    tny_sandbox_command command = {0};
    char err[128] = {0};
    ASSERT_EQ(0, tny_sandbox_command_build_kind(&ctx, TNY_SANDBOX_BWRAP, "/usr/bin/bwrap",
                                                "/bin/sh", "echo ok", &command, err, sizeof err));
    ASSERT_STR_EQ("/usr/bin/bwrap", command.argv[0]);
    ASSERT_STR_EQ("--ro-bind", command.argv[1]);
    ASSERT_STR_EQ("/", command.argv[2]);
    ASSERT_STR_EQ("/", command.argv[3]);
    ASSERT(argv_has(&command, "--bind"));
    ASSERT(argv_has(&command, ctx.cwd));
    ASSERT(argv_has(&command, extra_dirs[0]));
    ASSERT(argv_has(&command, "--dev"));
    ASSERT(argv_has(&command, "--proc"));
    ASSERT(argv_has(&command, "--unshare-pid"));
    ASSERT_STR_EQ("/bin/sh", command.argv[command.argc - 3]);
    ASSERT_STR_EQ("-c", command.argv[command.argc - 2]);
    ASSERT_STR_EQ("echo ok", command.argv[command.argc - 1]);
    tny_sandbox_command_free(&command);
    PASS();
}

TEST denied_path_parses_seatbelt_and_bubblewrap_shell_errors(void) {
    char *path =
        tny_sandbox_denied_path("touch: /Users/example/out.txt: Operation not permitted\n");
    ASSERT_STR_EQ("/Users/example/out.txt", path);
    free(path);
    path = tny_sandbox_denied_path(
        "touch: cannot touch '/home/example/out.txt': Read-only file system\n");
    ASSERT_STR_EQ("/home/example/out.txt", path);
    free(path);
    ASSERT_EQ(NULL, tny_sandbox_denied_path("ordinary command failure\n"));
    PASS();
}

/* The probe believes the wrapper's exit status, not its presence: a wrapper
 * that exists but cannot launch (nested Seatbelt, bubblewrap without user
 * namespaces) must resolve `auto` to none. */
static char *write_fake_wrapper(const char *body) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char *path = NULL;
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "%s/tny-sandbox-probe-XXXXXX", tmp);
    int fd = mkstemp(b.data);
    if (fd >= 0) {
        FILE *f = fdopen(fd, "w");
        if (f) {
            fprintf(f, "#!/bin/sh\n%s\n", body);
            fclose(f);
            chmod(b.data, 0700);
            path = buf_detach(&b);
        } else close(fd);
    }
    if (!path) buf_free(&b);
    return path;
}

TEST probe_trusts_the_wrapper_exit_status_not_its_presence(void) {
    char *ok = write_fake_wrapper("exit 0");
    char *broken = write_fake_wrapper("echo 'sandbox_apply: Operation not permitted' >&2; exit 1");
    char *hung = write_fake_wrapper("sleep 30");
    ASSERT(ok && broken && hung);
    ASSERT(tny_sandbox_probe(TNY_SANDBOX_SEATBELT, ok));
    ASSERT(tny_sandbox_probe(TNY_SANDBOX_BWRAP, ok));
    ASSERT_FALSE(tny_sandbox_probe(TNY_SANDBOX_SEATBELT, broken));
    ASSERT_FALSE(tny_sandbox_probe(TNY_SANDBOX_BWRAP, broken));
    ASSERT_FALSE(tny_sandbox_probe_ms(TNY_SANDBOX_BWRAP, hung, 150));
    ASSERT_FALSE(tny_sandbox_probe(TNY_SANDBOX_SEATBELT, "/nonexistent/sandbox-exec"));
    ASSERT_FALSE(tny_sandbox_probe(TNY_SANDBOX_NONE, ok));
    ASSERT_FALSE(tny_sandbox_probe(TNY_SANDBOX_SEATBELT, NULL));
    unlink(ok);
    unlink(broken);
    unlink(hung);
    free(ok);
    free(broken);
    free(hung);
    PASS();
}

SUITE(sandbox_suite) {
    RUN_TEST(probe_trusts_the_wrapper_exit_status_not_its_presence);
    RUN_TEST(effective_mode_honors_yolo_and_configuration);
    RUN_TEST(unsandboxed_argv_is_the_original_shell_command);
    RUN_TEST(seatbelt_profile_limits_writes_and_escapes_paths);
    RUN_TEST(bubblewrap_argv_ro_binds_root_and_widens_named_dirs);
    RUN_TEST(denied_path_parses_seatbelt_and_bubblewrap_shell_errors);
}
