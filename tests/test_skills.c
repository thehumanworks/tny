/* test_skills.c — SKILL.md discovery, lazy load, install, and tool wiring. */
#include "greatest.h"
#include "core/config.h"
#include "core/perm.h"
#include "core/session.h"
#include "core/skills.h"
#include "core/tools.h"
#include "util/util.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char root[PATH_MAX];
    char *project;
    char *workspace;
    char *old_home;
} skills_fixture;

static skills_fixture g_skills;

static void fixture_write(const char *relative_dir, const char *contents) {
    char *dir = path_join(g_skills.root, relative_dir);
    if (!dir || mkdir_p(dir) != 0) abort();
    char *file = path_join(dir, "SKILL.md");
    if (!file || file_write_atomic(file, contents, strlen(contents)) != 0) abort();
    free(file);
    free(dir);
}

static void skills_fixture_begin(void *udata) {
    (void)udata;
    memset(&g_skills, 0, sizeof g_skills);
    const char *old = getenv("HOME");
    if (old) g_skills.old_home = xstrdup(old);
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(g_skills.root, sizeof g_skills.root, "%s/tny-skills-test-XXXXXX", tmp);
    if (!mkdtemp(g_skills.root)) abort();
    if (setenv("HOME", g_skills.root, 1) != 0) abort();

    g_skills.project = path_join(g_skills.root, "project");
    g_skills.workspace = path_join(g_skills.project, "ws");
    if (!g_skills.project || !g_skills.workspace || mkdir_p(g_skills.workspace) != 0) abort();

    fixture_write("project/ws/skills/foo",
                  "---\nname: foo\ndescription: Workspace foo description\n---\nFOO-BODY\n");
    fixture_write("project/ws/skills/collide",
                  "---\nname: collide\ndescription: Near collision\n---\nCOLLIDE-NEAR\n");
    fixture_write("project/ws/.agents/skills/agents-only",
                  "---\nname: agents-only\ndescription: Workspace agents root\n---\nAGENTS-BODY\n");
    fixture_write("project/ws/.claude/skills/claude-only",
                  "---\nname: claude-only\ndescription: Workspace claude root\n---\nCLAUDE-BODY\n");
    fixture_write("project/ws/.codex/skills/codex-only",
                  "---\nname: codex-only\ndescription: Workspace codex root\n---\nCODEX-BODY\n");
    fixture_write("project/ws/.cursor/skills/cursor-only",
                  "---\nname: cursor-only\ndescription: Workspace cursor root\n---\nCURSOR-BODY\n");
    fixture_write("project/ws/.opencode/skills/opencode-only",
                  "---\nname: opencode-only\ndescription: Workspace opencode root\n---\nOPENCODE-BODY\n");
    fixture_write("project/skills/parent-only",
                  "---\nname: parent-only\ndescription: Parent workspace root\n---\nPARENT-BODY\n");
    fixture_write("project/.agents/skills/collide",
                  "---\nname: collide\ndescription: Far collision\n---\nCOLLIDE-FAR\n");

    fixture_write(".tny/skills/managed",
                  "---\nname: managed\ndescription: Managed user skill\n---\nMANAGED-BODY\n");
    fixture_write(".agents/skills/home-agents",
                  "---\nname: home-agents\ndescription: User agents root\n---\nHOME-AGENTS-BODY\n");
    fixture_write(".claude/skills/home-hidden",
                  "---\nname: home-hidden\ndescription: User claude root\n---\nHOME-HIDDEN-BODY\n");
    fixture_write(".codex/skills/home-codex",
                  "---\nname: home-codex\ndescription: User codex root\n---\nHOME-CODEX-BODY\n");
    fixture_write(".cursor/skills/home-cursor",
                  "---\nname: home-cursor\ndescription: User cursor root\n---\nHOME-CURSOR-BODY\n");
    fixture_write(".opencode/skills/home-opencode",
                  "---\nname: home-opencode\ndescription: User opencode root\n---\nHOME-OPENCODE-BODY\n");
    fixture_write("skills/home-plain",
                  "---\nname: home-plain\ndescription: Must not be discovered\n---\nHOME-PLAIN-BODY\n");

    fixture_write("project/ws/skills/bad-no-fm", "BAD-NO-FRONTMATTER\n");
    fixture_write("project/ws/skills/bad-noname",
                  "---\ndescription: Missing required name\n---\nBAD-NONAME-BODY\n");
    fixture_write("project/ws/skills/source-directory",
                  "---\nname: frontmatter-name\ndescription: Install name comes from YAML\n---\n"
                  "FRONTMATTER-NAME-BODY\n");
}

static void skills_fixture_end(void *udata) {
    (void)udata;
    if (g_skills.old_home) setenv("HOME", g_skills.old_home, 1);
    else unsetenv("HOME");
    free(g_skills.old_home);
    free(g_skills.workspace);
    free(g_skills.project);
    memset(&g_skills, 0, sizeof g_skills);
}

static const skill_meta *find_skill(const skill_meta *skills, int count, const char *name) {
    for (int i = 0; i < count; i++)
        if (strcmp(skills[i].name, name) == 0) return &skills[i];
    return NULL;
}

static int count_skill(const skill_meta *skills, int count, const char *name) {
    int found = 0;
    for (int i = 0; i < count; i++)
        if (strcmp(skills[i].name, name) == 0) found++;
    return found;
}

static bool has_skill_dir(const skill_meta *skills, int count, const char *dir) {
    for (int i = 0; i < count; i++)
        if (strcmp(skills[i].dir, dir) == 0) return true;
    return false;
}

TEST skills_discover_skips_home_plain(void) {
    tny_ctx *ctx = tny_ctx_load(g_skills.workspace);
    ASSERT(ctx);
    int count = 0;
    skill_meta *skills = skills_discover(ctx, &count);

    ASSERT(find_skill(skills, count, "foo"));
    ASSERT(find_skill(skills, count, "agents-only"));
    ASSERT(find_skill(skills, count, "claude-only"));
    ASSERT(find_skill(skills, count, "codex-only"));
    ASSERT(find_skill(skills, count, "cursor-only"));
    ASSERT(find_skill(skills, count, "opencode-only"));
    ASSERT(find_skill(skills, count, "parent-only"));
    ASSERT(find_skill(skills, count, "managed"));
    ASSERT(find_skill(skills, count, "home-agents"));
    ASSERT(find_skill(skills, count, "home-hidden"));
    ASSERT(find_skill(skills, count, "home-codex"));
    ASSERT(find_skill(skills, count, "home-cursor"));
    ASSERT(find_skill(skills, count, "home-opencode"));
    ASSERT_EQ(NULL, find_skill(skills, count, "home-plain"));

    char *foo_dir = path_join(g_skills.workspace, "skills/foo");
    ASSERT(foo_dir);
    ASSERT_STR_EQ(foo_dir, find_skill(skills, count, "foo")->dir);
    free(foo_dir);
    skills_free(skills, count);
    tny_ctx_free(ctx);
    PASS();
}

TEST skills_discover_lists_workspace_foo(void) {
    tny_ctx *ctx = tny_ctx_load(g_skills.workspace);
    ASSERT(ctx);
    int count = 0;
    skill_meta *skills = skills_discover(ctx, &count);
    const skill_meta *foo = find_skill(skills, count, "foo");
    ASSERT(foo);
    ASSERT_STR_EQ("Workspace foo description", foo->description);
    char *expected = path_join(g_skills.workspace, "skills/foo");
    ASSERT(expected);
    ASSERT_STR_EQ(expected, foo->dir);

    free(expected);
    skills_free(skills, count);
    tny_ctx_free(ctx);
    PASS();
}

TEST skills_load_foo_returns_body(void) {
    tny_ctx *ctx = tny_ctx_load(g_skills.workspace);
    ASSERT(ctx);
    char *body = skills_load(ctx, "foo");
    ASSERT(body);
    ASSERT(strstr(body, "name: foo"));
    ASSERT(strstr(body, "FOO-BODY"));
    ASSERT_EQ(NULL, skills_load(ctx, "no-such-skill"));

    free(body);
    tny_ctx_free(ctx);
    PASS();
}

TEST skills_install_copies_into_tny_dir(void) {
    tny_ctx *ctx = tny_ctx_load(g_skills.workspace);
    ASSERT(ctx);
    char err[256] = {0};
    char *foo_src = path_join(g_skills.workspace, "skills/foo");
    ASSERT(foo_src);
    ASSERT_EQ(0, skills_install(ctx, foo_src, err, sizeof err));
    ASSERT_EQ('\0', err[0]);

    char *foo_dst_dir = path_join(ctx->tny_dir, "skills/foo");
    char *foo_dst = path_join(foo_dst_dir, "SKILL.md");
    char *wrong_foo_dir = path_join(g_skills.root, "skills/foo");
    ASSERT(foo_dst_dir && foo_dst && wrong_foo_dir);
    ASSERT_EQ(0, access(foo_dst, F_OK));
    ASSERT(access(wrong_foo_dir, F_OK) != 0);

    char *named_src = path_join(g_skills.workspace, "skills/source-directory");
    ASSERT(named_src);
    ASSERT_EQ(0, skills_install(ctx, named_src, err, sizeof err));
    char *named_dst_dir = path_join(ctx->tny_dir, "skills/frontmatter-name");
    char *named_dst = path_join(named_dst_dir, "SKILL.md");
    char *wrong_named_dir = path_join(ctx->tny_dir, "skills/source-directory");
    ASSERT(named_dst_dir && named_dst && wrong_named_dir);
    ASSERT_EQ(0, access(named_dst, F_OK));
    ASSERT(access(wrong_named_dir, F_OK) != 0);

    free(wrong_named_dir);
    free(named_dst);
    free(named_dst_dir);
    free(named_src);
    free(wrong_foo_dir);
    free(foo_dst);
    free(foo_dst_dir);
    free(foo_src);
    tny_ctx_free(ctx);
    PASS();
}

TEST skills_discover_nearer_name_wins(void) {
    tny_ctx *ctx = tny_ctx_load(g_skills.workspace);
    ASSERT(ctx);
    int count = 0;
    skill_meta *skills = skills_discover(ctx, &count);
    ASSERT_EQ(1, count_skill(skills, count, "collide"));
    const skill_meta *collide = find_skill(skills, count, "collide");
    ASSERT(collide);
    char *expected = path_join(g_skills.workspace, "skills/collide");
    ASSERT(expected);
    ASSERT_STR_EQ(expected, collide->dir);

    free(expected);
    skills_free(skills, count);
    tny_ctx_free(ctx);
    PASS();
}

TEST skills_discover_frontmatter_rules(void) {
    tny_ctx *ctx = tny_ctx_load(g_skills.workspace);
    ASSERT(ctx);
    int count = 0;
    skill_meta *skills = skills_discover(ctx, &count);
    const skill_meta *foo = find_skill(skills, count, "foo");
    ASSERT(foo);
    ASSERT_STR_EQ("foo", foo->name);
    ASSERT_STR_EQ("Workspace foo description", foo->description);
    ASSERT_EQ(NULL, strstr(foo->description, "FOO-BODY"));

    char *no_fm = path_join(g_skills.workspace, "skills/bad-no-fm");
    char *no_name = path_join(g_skills.workspace, "skills/bad-noname");
    ASSERT(no_fm && no_name);
    ASSERT_FALSE(has_skill_dir(skills, count, no_fm));
    ASSERT_FALSE(has_skill_dir(skills, count, no_name));

    free(no_name);
    free(no_fm);
    skills_free(skills, count);
    tny_ctx_free(ctx);
    PASS();
}

TEST skills_install_error_buffer(void) {
    tny_ctx *ctx = tny_ctx_load(g_skills.workspace);
    ASSERT(ctx);
    char *invalid = path_join(g_skills.workspace, "skills/bad-no-fm");
    ASSERT(invalid);
    char err[256] = {0};
    ASSERT_EQ(-1, skills_install(ctx, invalid, err, sizeof err));
    ASSERT(err[0] != '\0');
    ASSERT(strstr(err, "no valid SKILL.md frontmatter"));

    char *invalid_dst = path_join(ctx->tny_dir, "skills/bad-no-fm");
    ASSERT(invalid_dst);
    ASSERT(access(invalid_dst, F_OK) != 0);

    free(invalid_dst);
    free(invalid);
    tny_ctx_free(ctx);
    PASS();
}

TEST tools_skill_and_install_happy(void) {
    tny_ctx *ctx = tny_ctx_load(g_skills.workspace);
    ASSERT(ctx);
    perm_engine *perm = perm_new(ctx);
    tny_session_state *session = session_new(ctx);
    ASSERT(perm && session);
    tools_env env = {.ctx = ctx, .session = session, .perm = perm};
    tools_call call;

    ASSERT_EQ(0, tools_call_prepare(&env, "skill", "{\"name\":\"foo\"}", &call));
    ASSERT_EQ(PERM_ALLOW, call.verdict);
    char *result = tools_call_execute(&env, &call);
    ASSERT(result);
    ASSERT(strstr(result, "FOO-BODY"));
    free(result);
    tools_call_free(&call);

    ASSERT_EQ(0, tools_call_prepare(&env, "install_skill", "{\"path\":\"skills/foo\"}",
                                    &call));
    ASSERT_EQ(PERM_ALLOW, call.verdict);
    result = tools_call_execute(&env, &call);
    ASSERT(result);
    ASSERT(strstr(result, "installed skill"));
    ASSERT(strstr(result, "~/.tny/skills"));
    free(result);
    tools_call_free(&call);

    char *dst_dir = path_join(ctx->tny_dir, "skills/foo");
    char *dst = path_join(dst_dir, "SKILL.md");
    char *wrong = path_join(g_skills.root, "skills/foo");
    ASSERT(dst_dir && dst && wrong);
    ASSERT_EQ(0, access(dst, F_OK));
    ASSERT(access(wrong, F_OK) != 0);

    free(wrong);
    free(dst);
    free(dst_dir);
    session_close(session);
    perm_free(perm);
    tny_ctx_free(ctx);
    PASS();
}

TEST tools_skill_collision_nearer_body(void) {
    tny_ctx *ctx = tny_ctx_load(g_skills.workspace);
    ASSERT(ctx);
    perm_engine *perm = perm_new(ctx);
    tny_session_state *session = session_new(ctx);
    ASSERT(perm && session);
    tools_env env = {.ctx = ctx, .session = session, .perm = perm};
    tools_call call;

    ASSERT_EQ(0,
              tools_call_prepare(&env, "skill", "{\"name\":\"collide\"}", &call));
    ASSERT_EQ(PERM_ALLOW, call.verdict);
    char *result = tools_call_execute(&env, &call);
    ASSERT(result);
    ASSERT(strstr(result, "COLLIDE-NEAR"));
    ASSERT_EQ(NULL, strstr(result, "COLLIDE-FAR"));

    free(result);
    tools_call_free(&call);
    session_close(session);
    perm_free(perm);
    tny_ctx_free(ctx);
    PASS();
}

SUITE(skills_suite) {
    SET_SETUP(skills_fixture_begin, NULL);
    SET_TEARDOWN(skills_fixture_end, NULL);
    RUN_TEST(skills_discover_skips_home_plain);
    RUN_TEST(skills_discover_lists_workspace_foo);
    RUN_TEST(skills_load_foo_returns_body);
    RUN_TEST(skills_install_copies_into_tny_dir);
    RUN_TEST(skills_discover_nearer_name_wins);
    RUN_TEST(skills_discover_frontmatter_rules);
    RUN_TEST(skills_install_error_buffer);
    RUN_TEST(tools_skill_and_install_happy);
    RUN_TEST(tools_skill_collision_nearer_body);
    SET_SETUP(NULL, NULL);
    SET_TEARDOWN(NULL, NULL);
}
