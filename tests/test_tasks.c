/* test_tasks.c — task parsing, precedence, listing, and atomic updates. */
#include "greatest.h"
#include "core/config.h"
#include "core/tasks.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char root[512];
    char workspace[540];
    char workflow[540];
    char *old_home;
    char *old_workflow;
} task_env;

static void task_env_begin(task_env *env) {
    memset(env, 0, sizeof *env);
    const char *value = getenv("HOME");
    if (value) env->old_home = xstrdup(value);
    value = getenv("TNY_WORKFLOW_TASK_DIR");
    if (value) env->old_workflow = xstrdup(value);
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(env->root, sizeof env->root, "%s/tny-task-test-XXXXXX", tmp);
    if (!mkdtemp(env->root)) abort();
    snprintf(env->workspace, sizeof env->workspace, "%s/workspace", env->root);
    snprintf(env->workflow, sizeof env->workflow, "%s/workflow", env->root);
    if (mkdir_p(env->workspace) != 0 || mkdir_p(env->workflow) != 0) abort();
    setenv("HOME", env->root, 1);
    unsetenv("TNY_WORKFLOW_TASK_DIR");
}

static void task_env_end(task_env *env) {
    if (env->old_home) setenv("HOME", env->old_home, 1);
    else unsetenv("HOME");
    if (env->old_workflow) setenv("TNY_WORKFLOW_TASK_DIR", env->old_workflow, 1);
    else unsetenv("TNY_WORKFLOW_TASK_DIR");
    free(env->old_home);
    free(env->old_workflow);
}

static char *custom_path(const char *root, const char *name) {
    char *directory = path_join(root, ".tny/tasks");
    if (!directory || mkdir_p(directory) != 0) abort();
    char filename[96];
    snprintf(filename, sizeof filename, "%s.md", name);
    char *path = path_join(directory, filename);
    free(directory);
    return path;
}

static const tny_task_info *find_task(const tny_task_info *items, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++)
        if (strcmp(items[i].name, name) == 0) return &items[i];
    return NULL;
}

TEST task_name_grammar_is_strict(void) {
    ASSERT(tny_task_name_valid("review"));
    ASSERT(tny_task_name_valid("release_1.2-x"));
    ASSERT(tny_task_name_valid("AZaz09_-x.y"));
    ASSERT_FALSE(tny_task_name_valid(NULL));
    ASSERT_FALSE(tny_task_name_valid(""));
    ASSERT_FALSE(tny_task_name_valid(".hidden"));
    ASSERT_FALSE(tny_task_name_valid("a..b"));
    ASSERT_FALSE(tny_task_name_valid("has space"));
    ASSERT_FALSE(tny_task_name_valid("has/slash"));
    ASSERT_FALSE(tny_task_name_valid("has@sign"));
    ASSERT_FALSE(tny_task_name_valid("caf\xc3\xa9"));
    char long_name[TNY_TASK_NAME_MAX + 1];
    memset(long_name, 'a', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = 0;
    ASSERT_FALSE(tny_task_name_valid(long_name));
    PASS();
}

TEST task_set_is_atomic_on_invalid_input(void) {
    tny_ctx ctx = {0};
    ASSERT_EQ(TNY_TASK_OK, tny_task_set_explicit(&ctx, "first", "keep me", "explicit"));
    char digest[sizeof ctx.task_digest];
    memcpy(digest, ctx.task_digest, sizeof digest);
    ASSERT_EQ(TNY_TASK_INVALID, tny_task_set_explicit(&ctx, "bad/name", "replacement", "explicit"));
    ASSERT_EQ(TNY_TASK_INVALID,
              tny_task_set_explicit(&ctx, "second", "replacement", "/private/path"));
    ASSERT_STR_EQ("first", ctx.task_name);
    ASSERT_STR_EQ("keep me", ctx.task_instructions);
    ASSERT_STR_EQ("explicit", ctx.task_source);
    ASSERT_EQ(TNY_TASK_DIGEST_HEX_LEN, strlen(ctx.task_digest));
    ASSERT_MEM_EQ(digest, ctx.task_digest, sizeof digest);
    char *maximum = malloc(TNY_TASK_BODY_MAX + 1);
    ASSERT(maximum);
    memset(maximum, 'x', TNY_TASK_BODY_MAX);
    maximum[TNY_TASK_BODY_MAX] = 0;
    ASSERT_EQ(TNY_TASK_OK, tny_task_set_explicit(&ctx, "maximum", maximum, "explicit"));
    maximum[TNY_TASK_BODY_MAX] = 'x';
    char *grown = realloc(maximum, TNY_TASK_BODY_MAX + 2);
    ASSERT(grown);
    maximum = grown;
    maximum[TNY_TASK_BODY_MAX + 1] = 0;
    ASSERT_EQ(TNY_TASK_INVALID, tny_task_set_explicit(&ctx, "too-long", maximum, "explicit"));
    ASSERT_STR_EQ("maximum", ctx.task_name);
    free(maximum);
    free(ctx.task_name);
    free(ctx.task_source);
    free(ctx.task_instructions);
    PASS();
}

TEST task_frontmatter_is_stripped_and_described(void) {
    task_env env;
    task_env_begin(&env);
    char *path = custom_path(env.workspace, "review");
    const char *file = "---\r\nname: review\r\ndescription: Project review rules\r\n---\r\n\r\n"
                       "\t\r\nInspect project invariants.\n";
    ASSERT_EQ(0, file_write_atomic(path, file, strlen(file)));
    tny_ctx *ctx = tny_ctx_new_explicit(env.workspace, env.root);
    ASSERT(ctx);
    ASSERT_EQ(TNY_TASK_OK, tny_task_apply(ctx, "review"));
    ASSERT_STR_EQ("project", ctx->task_source);
    ASSERT_STR_EQ("Inspect project invariants.\n", ctx->task_instructions);
    ASSERT_FALSE(strstr(ctx->task_instructions, "description:"));

    tny_task_info *items = NULL;
    size_t count = 0;
    ASSERT_EQ(TNY_TASK_OK, tny_task_list(ctx, &items, &count));
    const tny_task_info *review = find_task(items, count, "review");
    ASSERT(review);
    ASSERT(review->valid);
    ASSERT_STR_EQ("project", review->source);
    ASSERT_STR_EQ("Project review rules", review->description);
    for (size_t i = 1; i < count; i++) ASSERT(strcmp(items[i - 1].name, items[i].name) < 0);
    tny_task_list_free(items, count);
    tny_ctx_free(ctx);
    free(path);
    task_env_end(&env);
    PASS();
}

TEST task_precedence_and_sources_match_selection(void) {
    task_env env;
    task_env_begin(&env);
    char *project = custom_path(env.workspace, "review");
    char *user = custom_path(env.root, "review");
    char *workflow = path_join(env.workflow, "review");
    ASSERT_EQ(0, file_write_atomic(user, "user body", 9));
    ASSERT_EQ(0, file_write_atomic(project, "project body", 12));
    ASSERT_EQ(0, file_write_atomic(workflow, "workflow body", 13));
    setenv("TNY_WORKFLOW_TASK_DIR", env.workflow, 1);
    tny_ctx *ctx = tny_ctx_new_explicit(env.workspace, env.root);
    ASSERT(ctx);
    ASSERT_EQ(TNY_TASK_OK, tny_task_apply(ctx, "review"));
    ASSERT_STR_EQ("workflow", ctx->task_source);
    ASSERT_STR_EQ("workflow body", ctx->task_instructions);
    tny_task_info *items = NULL;
    size_t count = 0;
    ASSERT_EQ(TNY_TASK_OK, tny_task_list(ctx, &items, &count));
    const tny_task_info *review = find_task(items, count, "review");
    ASSERT(review);
    ASSERT_STR_EQ("workflow", review->source);
    tny_task_list_free(items, count);

    unsetenv("TNY_WORKFLOW_TASK_DIR");
    ASSERT_EQ(TNY_TASK_OK, tny_task_apply(ctx, "review"));
    ASSERT_STR_EQ("project", ctx->task_source);
    ASSERT_STR_EQ("project body", ctx->task_instructions);
    unlink(project);
    ASSERT_EQ(TNY_TASK_OK, tny_task_apply(ctx, "review"));
    ASSERT_STR_EQ("user", ctx->task_source);
    unlink(user);
    ASSERT_EQ(TNY_TASK_OK, tny_task_apply(ctx, "review"));
    ASSERT_STR_EQ("builtin", ctx->task_source);
    tny_ctx_free(ctx);
    free(project);
    free(user);
    free(workflow);
    task_env_end(&env);
    PASS();
}

TEST invalid_utf8_and_symlink_shadow_without_mutation(void) {
    task_env env;
    task_env_begin(&env);
    tny_ctx *ctx = tny_ctx_new_explicit(env.workspace, env.root);
    ASSERT(ctx);
    ASSERT_EQ(TNY_TASK_OK, tny_task_apply(ctx, "review"));
    char *path = custom_path(env.workspace, "review");
    const unsigned char invalid[] = {'b', 'a', 'd', 0xc0, 0xaf};
    ASSERT_EQ(0, file_write_atomic(path, invalid, sizeof invalid));
    ASSERT_EQ(TNY_TASK_INVALID, tny_task_apply(ctx, "review"));
    ASSERT_STR_EQ("builtin", ctx->task_source);
    tny_task_info *items = NULL;
    size_t count = 0;
    ASSERT_EQ(TNY_TASK_OK, tny_task_list(ctx, &items, &count));
    const tny_task_info *review = find_task(items, count, "review");
    ASSERT(review);
    ASSERT_FALSE(review->valid);
    ASSERT_STR_EQ("project", review->source);
    tny_task_list_free(items, count);

    const char nul_body[] = {'o', 'k', '\0', 'x'};
    ASSERT_EQ(0, file_write_atomic(path, nul_body, sizeof nul_body));
    ASSERT_EQ(TNY_TASK_INVALID, tny_task_apply(ctx, "review"));
    const char *unknown_metadata = "---\ncommands: forbidden\n---\nbody\n";
    ASSERT_EQ(0, file_write_atomic(path, unknown_metadata, strlen(unknown_metadata)));
    ASSERT_EQ(TNY_TASK_INVALID, tny_task_apply(ctx, "review"));
    const char *wrong_name = "---\nname: optimizer\n---\nbody\n";
    ASSERT_EQ(0, file_write_atomic(path, wrong_name, strlen(wrong_name)));
    ASSERT_EQ(TNY_TASK_INVALID, tny_task_apply(ctx, "review"));
    const char *duplicate = "---\ndescription: one\ndescription: two\n---\nbody\n";
    ASSERT_EQ(0, file_write_atomic(path, duplicate, strlen(duplicate)));
    ASSERT_EQ(TNY_TASK_INVALID, tny_task_apply(ctx, "review"));
    const char *empty_body = "---\nname: review\n---\n \t\r\n";
    ASSERT_EQ(0, file_write_atomic(path, empty_body, strlen(empty_body)));
    ASSERT_EQ(TNY_TASK_INVALID, tny_task_apply(ctx, "review"));
    const char *unterminated = "---\nname: review\nbody\n";
    ASSERT_EQ(0, file_write_atomic(path, unterminated, strlen(unterminated)));
    ASSERT_EQ(TNY_TASK_INVALID, tny_task_apply(ctx, "review"));

    unlink(path);
    ASSERT_EQ(0, symlink("missing-target", path));
    ASSERT_EQ(TNY_TASK_INVALID, tny_task_apply(ctx, "review"));
    ASSERT_STR_EQ("builtin", ctx->task_source);
    unlink(path);
    ASSERT_EQ(0, mkfifo(path, 0600));
    ASSERT_EQ(TNY_TASK_INVALID, tny_task_apply(ctx, "review"));
    tny_ctx_free(ctx);
    free(path);
    task_env_end(&env);
    PASS();
}

TEST hostile_home_and_workflow_symlinks_are_not_ambient_authority(void) {
    task_env env;
    task_env_begin(&env);
    char *workflow_task = path_join(env.workflow, "review");
    ASSERT(workflow_task);
    ASSERT_EQ(0, file_write_atomic(workflow_task, "workflow through symlink", 24));
    char linked_workflow[560];
    snprintf(linked_workflow, sizeof linked_workflow, "%s/linked-workflow", env.root);
    ASSERT_EQ(0, symlink(env.workflow, linked_workflow));
    setenv("TNY_WORKFLOW_TASK_DIR", linked_workflow, 1);

    tny_ctx *ctx = tny_ctx_new_explicit(env.workspace, env.root);
    ASSERT(ctx);
    ASSERT_EQ(TNY_TASK_INVALID, tny_task_apply(ctx, "review"));
    tny_task_info *items = NULL;
    size_t count = 0;
    ASSERT_EQ(TNY_TASK_OK, tny_task_list(ctx, &items, &count));
    const tny_task_info *review = find_task(items, count, "review");
    ASSERT(review);
    ASSERT_FALSE(review->valid);
    ASSERT_STR_EQ("workflow", review->source);
    tny_task_list_free(items, count);

    unsetenv("TNY_WORKFLOW_TASK_DIR");
    char original_cwd[1024];
    ASSERT(getcwd(original_cwd, sizeof original_cwd));
    ASSERT_EQ(0, chdir(env.root));
    char *relative_task = custom_path("relative-home", "review");
    ASSERT(relative_task);
    ASSERT_EQ(0, file_write_atomic(relative_task, "relative HOME task", 18));
    setenv("HOME", "relative-home", 1);
    ASSERT_EQ(TNY_TASK_OK, tny_task_apply(ctx, "review"));
    ASSERT_STR_EQ("builtin", ctx->task_source);
    unsetenv("HOME");
    ASSERT_EQ(TNY_TASK_OK, tny_task_apply(ctx, "review"));
    ASSERT_STR_EQ("builtin", ctx->task_source);
    ASSERT_EQ(0, chdir(original_cwd));

    free(relative_task);
    tny_ctx_free(ctx);
    free(workflow_task);
    task_env_end(&env);
    PASS();
}

TEST ssh_task_discovery_is_builtin_only(void) {
    task_env env;
    task_env_begin(&env);
    char *path = custom_path(env.workspace, "review");
    ASSERT_EQ(0, file_write_atomic(path, "must not load", 13));
    tny_ctx *ctx = tny_ctx_new_explicit(env.workspace, env.root);
    ASSERT(ctx);
    ctx->ssh_host = xstrdup("example.test");
    ASSERT_EQ(TNY_TASK_OK, tny_task_apply(ctx, "review"));
    ASSERT_STR_EQ("builtin", ctx->task_source);
    ASSERT_EQ(TNY_TASK_INVALID, tny_task_apply(ctx, "project-only"));
    tny_ctx_free(ctx);
    free(path);
    task_env_end(&env);
    PASS();
}

TEST custom_directory_symlinks_do_not_escape_the_root(void) {
    task_env env;
    task_env_begin(&env);
    char outside[560];
    snprintf(outside, sizeof outside, "%s/outside", env.root);
    char *outside_task = custom_path(outside, "escaped");
    ASSERT_EQ(0, file_write_atomic(outside_task, "must not load", 13));
    char *workspace_tny = path_join(env.workspace, ".tny");
    char *outside_tny = path_join(outside, ".tny");
    ASSERT(workspace_tny);
    ASSERT(outside_tny);
    ASSERT_EQ(0, symlink(outside_tny, workspace_tny));

    tny_ctx *ctx = tny_ctx_new_explicit(env.workspace, env.root);
    ASSERT(ctx);
    ASSERT_EQ(TNY_TASK_INVALID, tny_task_apply(ctx, "escaped"));
    tny_task_info *items = NULL;
    size_t count = 0;
    ASSERT_EQ(TNY_TASK_OK, tny_task_list(ctx, &items, &count));
    ASSERT_FALSE(find_task(items, count, "escaped"));
    tny_task_list_free(items, count);
    tny_ctx_free(ctx);
    free(workspace_tny);
    free(outside_tny);
    free(outside_task);
    task_env_end(&env);
    PASS();
}

TEST task_listing_rejects_excessive_definition_count(void) {
    task_env env;
    task_env_begin(&env);
    for (unsigned i = 0; i < TNY_TASK_COUNT_MAX; i++) {
        char name[32], body[32];
        snprintf(name, sizeof name, "task-%03u", i);
        snprintf(body, sizeof body, "body %u\n", i);
        char *path = custom_path(env.workspace, name);
        ASSERT(path);
        ASSERT_EQ(0, file_write_atomic(path, body, strlen(body)));
        free(path);
    }
    char *overflow = custom_path(env.workspace, "overflow");
    ASSERT(overflow);
    ASSERT_EQ(0, file_write_atomic(overflow, "overflow\n", 9));
    tny_ctx *ctx = tny_ctx_new_explicit(env.workspace, env.root);
    ASSERT(ctx);
    tny_task_info *items = NULL;
    size_t count = 0;
    ASSERT_EQ(TNY_TASK_INVALID, tny_task_list(ctx, &items, &count));
    ASSERT(!items);
    ASSERT_EQ(0u, count);
    tny_ctx_free(ctx);
    free(overflow);
    task_env_end(&env);
    PASS();
}

SUITE(tasks_suite) {
    RUN_TEST(task_name_grammar_is_strict);
    RUN_TEST(task_set_is_atomic_on_invalid_input);
    RUN_TEST(task_frontmatter_is_stripped_and_described);
    RUN_TEST(task_precedence_and_sources_match_selection);
    RUN_TEST(invalid_utf8_and_symlink_shadow_without_mutation);
    RUN_TEST(hostile_home_and_workflow_symlinks_are_not_ambient_authority);
    RUN_TEST(ssh_task_discovery_is_builtin_only);
    RUN_TEST(custom_directory_symlinks_do_not_escape_the_root);
    RUN_TEST(task_listing_rejects_excessive_definition_count);
}
