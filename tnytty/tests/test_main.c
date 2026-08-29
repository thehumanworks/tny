#include "greatest.h"

GREATEST_MAIN_DEFS();

extern SUITE(vt_suite);
extern SUITE(icat_suite);
extern SUITE(http_suite);

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(vt_suite);
    RUN_SUITE(icat_suite);
    RUN_SUITE(http_suite);
    GREATEST_MAIN_END();
}
