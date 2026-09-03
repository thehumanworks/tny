/* test_main.c — greatest runner; suites live in the other test files. */
#include "greatest.h"

GREATEST_MAIN_DEFS();

SUITE_EXTERN(util_suite);
SUITE_EXTERN(net_suite);
SUITE_EXTERN(http_server_suite);
SUITE_EXTERN(core_suite);
SUITE_EXTERN(runtime_suite);
SUITE_EXTERN(extensions_suite);
SUITE_EXTERN(tui_suite);
SUITE_EXTERN(cursor_suite);
SUITE_EXTERN(cursor_sdk_suite);
SUITE_EXTERN(cursor_options_suite);
SUITE_EXTERN(cursor_management_suite);
SUITE_EXTERN(cursor_callbacks_suite);
SUITE_EXTERN(acp_suite);
SUITE_EXTERN(openai_suite);
SUITE_EXTERN(ephemeral_suite);
SUITE_EXTERN(session_bg_suite);
SUITE_EXTERN(runner_suite);
SUITE_EXTERN(ssh_suite);
SUITE_EXTERN(tasks_suite);
SUITE_EXTERN(mcp_suite);
SUITE_EXTERN(web_search_suite);
SUITE_EXTERN(skills_suite);
SUITE_EXTERN(sandbox_suite);
SUITE_EXTERN(edit_suite);
SUITE_EXTERN(perm_suite);
SUITE_EXTERN(intercept_suite);

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(util_suite);
    RUN_SUITE(net_suite);
    RUN_SUITE(http_server_suite);
    RUN_SUITE(core_suite);
    RUN_SUITE(runtime_suite);
    RUN_SUITE(extensions_suite);
    RUN_SUITE(tui_suite);
    RUN_SUITE(cursor_suite);
    RUN_SUITE(cursor_sdk_suite);
    RUN_SUITE(cursor_options_suite);
    RUN_SUITE(cursor_management_suite);
    RUN_SUITE(cursor_callbacks_suite);
    RUN_SUITE(acp_suite);
    RUN_SUITE(openai_suite);
    RUN_SUITE(ephemeral_suite);
    RUN_SUITE(session_bg_suite);
    RUN_SUITE(runner_suite);
    RUN_SUITE(ssh_suite);
    RUN_SUITE(tasks_suite);
    RUN_SUITE(mcp_suite);
    RUN_SUITE(web_search_suite);
    RUN_SUITE(skills_suite);
    RUN_SUITE(sandbox_suite);
    RUN_SUITE(edit_suite);
    RUN_SUITE(perm_suite);
    RUN_SUITE(intercept_suite);
    GREATEST_MAIN_END();
}
