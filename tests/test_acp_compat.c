#include "greatest.h"
#include "backends/acp/acp_compat.h"

#include <stdio.h>
#include <string.h>

TEST acp_versions_match_lean(void) {
    FILE *f = fopen("tests/formal/acp/golden/versions.tsv", "r");
    ASSERT(f);
    static char line[1024];
    ASSERT(fgets(line, sizeof line, f));
    ASSERT_STR_EQ("name\tversion\taccepted\n", line);
    size_t rows = 0;
    while (fgets(line, sizeof line, f)) {
        char *version = strchr(line, '\t');
        ASSERT(version);
        *version++ = '\0';
        char *accepted = strchr(version, '\t');
        ASSERT(accepted);
        *accepted++ = '\0';
        ASSERT(strcmp(accepted, "1\n") == 0 || strcmp(accepted, "0\n") == 0);
        ASSERT_EQm(version, accepted[0] == '1', acp_claude_tools_only(line, version));
        rows++;
    }
    ASSERT(!ferror(f));
    fclose(f);
    ASSERT(rows >= 20);
    PASS();
}

TEST acp_versions_fail_closed(void) {
    ASSERT(!acp_claude_tools_only(NULL, "0.81.2"));
    ASSERT(!acp_claude_tools_only(ACP_CLAUDE_NAME, NULL));
    const char *bad[] = {"",
                         "0",
                         "0.75",
                         "0.75.",
                         ".75.1",
                         "0..1",
                         "0.75.1.0",
                         "00.75.1",
                         "0.075.1",
                         "0.75.01",
                         "v0.81.2",
                         " 0.81.2",
                         "0.81.2 ",
                         "0.81.2\n",
                         "0.81.2-rc.1",
                         "1.0.0-dev+build",
                         "0.81.2+",
                         "0.81.2+.",
                         "0.81.2+a..b",
                         "0.81.2+a.",
                         "0.81.2+a_b",
                         "0.81.2+é",
                         "4294967296.0.0",
                         "1.4294967296.0",
                         "1.0.4294967296",
                         "18446744073709551616.0.0",
                         "+1.0.0",
                         "-1.0.0"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        ASSERT_FALSEm(bad[i], acp_claude_tools_only(ACP_CLAUDE_NAME, bad[i]));
    PASS();
}

TEST acp_versions_numeric_order(void) {
    static char version[64];
    for (unsigned int major = 0; major < 3; major++) {
        for (unsigned int minor = 0; minor <= 110; minor++) {
            for (unsigned int patch = 0; patch < 4; patch++) {
                bool want = major != 0 || minor >= 76 || (minor == 75 && patch != 0);
                snprintf(version, sizeof version, "%u.%u.%u", major, minor, patch);
                ASSERT_EQm(version, want, acp_claude_tools_only(ACP_CLAUDE_NAME, version));
                size_t len = strlen(version);
                snprintf(version + len, sizeof version - len, "+build.001-A");
                ASSERT_EQm(version, want, acp_claude_tools_only(ACP_CLAUDE_NAME, version));
            }
        }
    }
    ASSERT(acp_claude_tools_only(ACP_CLAUDE_NAME, "4294967295.4294967295.4294967295"));
    ASSERT(acp_claude_tools_only(ACP_CLAUDE_NAME, "0.75.1+-"));
    ASSERT(!acp_claude_tools_only("claude-agent-acp", "1.0.0"));
    PASS();
}

SUITE(acp_compat_suite) {
    RUN_TEST(acp_versions_match_lean);
    RUN_TEST(acp_versions_fail_closed);
    RUN_TEST(acp_versions_numeric_order);
}
