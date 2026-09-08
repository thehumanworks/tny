#include "greatest.h"
#include "cli/cli.h"
#include "util/worktree.h"
#include <string.h>

TEST worktree_names_are_single_safe_components(void) {
    ASSERT(worktree_name_valid("feature-1_test"));
    ASSERT(worktree_name_valid("A"));
    const char *invalid[] = {NULL,    "",    ".",    "..",   "../outside", "/absolute", "two words",
                             "-flag", "a.b", "a\\b", "a\nb", "a;command",  NULL};
    ASSERT_FALSE(worktree_name_valid(invalid[0]));
    for (size_t i = 1; invalid[i]; i++) ASSERT_FALSE(worktree_name_valid(invalid[i]));
    char name[82];
    memset(name, 'a', sizeof name);
    name[80] = 0;
    ASSERT(worktree_name_valid(name));
    name[80] = 'a';
    name[81] = 0;
    ASSERT_FALSE(worktree_name_valid(name));
    PASS();
}

TEST worktree_optional_argument_preserves_commands_and_flags(void) {
    cli_globals g = {0};
    char *a[] = {(char *)"tny", (char *)"--worktree"};
    ASSERT_EQ(2, cli_parse_globals(2, a, &g));
    ASSERT(g.worktree);
    ASSERT_EQ(NULL, g.worktree_name);
    char *b[] = {(char *)"tny", (char *)"--worktree", (char *)"fix", (char *)"ask", (char *)"hi"};
    ASSERT_EQ(3, cli_parse_globals(5, b, &g));
    ASSERT_STR_EQ("fix", g.worktree_name);
    char *c[] = {(char *)"tny", (char *)"--worktree", (char *)"ask", (char *)"hi"};
    ASSERT_EQ(2, cli_parse_globals(4, c, &g));
    ASSERT_EQ(NULL, g.worktree_name);
    char *d[] = {(char *)"tny", (char *)"--worktree", (char *)"--cwd", (char *)"/tmp"};
    ASSERT_EQ(4, cli_parse_globals(4, d, &g));
    ASSERT_EQ(NULL, g.worktree_name);
    ASSERT_STR_EQ("/tmp", g.cwd);
    char *e[] = {(char *)"tny", (char *)"--worktree=status", (char *)"status"};
    ASSERT_EQ(2, cli_parse_globals(3, e, &g));
    ASSERT_STR_EQ("status", g.worktree_name);
    char *f[] = {(char *)"tny", (char *)"--worktree", (char *)"--help"};
    ASSERT_EQ(2, cli_parse_globals(3, f, &g));
    PASS();
}

SUITE(worktree_suite) {
    RUN_TEST(worktree_names_are_single_safe_components);
    RUN_TEST(worktree_optional_argument_preserves_commands_and_flags);
}
