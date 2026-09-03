#include <stdio.h>
#include "mod.h"

int main(void) {
    if (logmsg(2, 3) != 5) return 1;
    if (logmsg(0, 0) != 0) return 1;
    printf("ok\n");
    return 0;
}
