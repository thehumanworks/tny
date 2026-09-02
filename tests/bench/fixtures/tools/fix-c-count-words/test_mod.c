#include <stdio.h>
#include <string.h>
#include "mod.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    CHECK(count_words("a b c") == 3);
    CHECK(count_words("") == 0);
    CHECK(count_words("one") == 1);
    CHECK(count_words("a  b") == 2);
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
