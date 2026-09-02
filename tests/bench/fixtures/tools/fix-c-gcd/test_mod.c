#include <stdio.h>
#include <string.h>
#include "mod.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    CHECK(gcd(12, 8) == 4);
    CHECK(gcd(7, 13) == 1);
    CHECK(gcd(9, 0) == 9);
    CHECK(gcd(0, 5) == 5);
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
