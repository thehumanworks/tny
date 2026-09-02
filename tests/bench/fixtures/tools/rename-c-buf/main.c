#include <stdio.h>
#include "mod.h"

int main(void) {
    if (buf_push(2, 3) != 5) return 1;
    if (buf_push(0, 0) != 0) return 1;
    printf("ok\n");
    return 0;
}
