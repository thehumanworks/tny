/* test_main.c — greatest runner; suites live in the other test files. */
#include "greatest.h"

GREATEST_MAIN_DEFS();

SUITE_EXTERN(util_suite);
SUITE_EXTERN(net_suite);
SUITE_EXTERN(core_suite);
SUITE_EXTERN(tui_suite);
SUITE_EXTERN(codex_suite);
SUITE_EXTERN(openai_suite);

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(util_suite);
    RUN_SUITE(net_suite);
    RUN_SUITE(core_suite);
    RUN_SUITE(tui_suite);
    RUN_SUITE(codex_suite);
    RUN_SUITE(openai_suite);
    GREATEST_MAIN_END();
}
