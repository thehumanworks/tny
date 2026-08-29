#include "greatest.h"

GREATEST_MAIN_DEFS();

extern SUITE(vt_suite);
extern SUITE(icat_suite);
extern SUITE(http_suite);
extern SUITE(session_suite);
extern SUITE(config_suite);
extern SUITE(keys_suite);
extern SUITE(render_suite);
extern SUITE(selection_suite);
extern SUITE(status_suite);

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(vt_suite);
    RUN_SUITE(icat_suite);
    RUN_SUITE(http_suite);
    RUN_SUITE(session_suite);
    RUN_SUITE(config_suite);
    RUN_SUITE(keys_suite);
    RUN_SUITE(render_suite);
    RUN_SUITE(selection_suite);
    RUN_SUITE(status_suite);
    GREATEST_MAIN_END();
}
