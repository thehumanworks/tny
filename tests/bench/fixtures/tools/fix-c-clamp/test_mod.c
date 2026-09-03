#include <stdio.h>
#include <string.h>
#include "mod.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    CHECK(clamp(5, 0, 10) == 5);
    CHECK(clamp(-3, 0, 10) == 0);
    CHECK(clamp(42, 0, 10) == 10);
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
