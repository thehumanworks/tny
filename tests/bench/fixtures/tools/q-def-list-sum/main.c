#include <stdio.h>
#include "list.h"

int main(void) {
    struct node b = {2, NULL};
    struct node a = {1, &b};
    printf("%zu %d\n", list_len(&a), list_sum(&a));
    return 0;
}
