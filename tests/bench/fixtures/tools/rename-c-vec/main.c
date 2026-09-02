#include <stdio.h>
#include "mod.h"

int main(void) {
    if (vpush(2, 3) != 5) return 1;
    if (vpush(0, 0) != 0) return 1;
    printf("ok\n");
    return 0;
}
