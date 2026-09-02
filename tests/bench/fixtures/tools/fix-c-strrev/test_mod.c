#include <stdio.h>
#include <string.h>
#include "mod.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    char a[] = "abc";
    strrev(a);
    CHECK(strcmp(a, "cba") == 0);
    char b[] = "ab";
    strrev(b);
    CHECK(strcmp(b, "ba") == 0);
    char c[] = "";
    strrev(c);
    CHECK(strcmp(c, "") == 0);
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
