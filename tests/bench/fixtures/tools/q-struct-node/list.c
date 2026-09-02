#include "list.h"

size_t list_len(const struct node *head) {
    size_t n = 0;
    while (head) {
        n++;
        head = head->next;
    }
    return n;
}

int list_sum(const struct node *head) {
    int total = 0;
    while (head) {
        total += head->value;
        head = head->next;
    }
    return total;
}
