/* Run real provider/runtime regressions against fully injected library objects. */
#include "greatest.h"
SUITE_EXTERN(acp_suite);
SUITE_EXTERN(cursor_suite);
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(acp_suite);
    RUN_SUITE(cursor_suite);
    GREATEST_MAIN_END();
}
