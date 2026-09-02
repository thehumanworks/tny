#ifndef LIST_H
#define LIST_H
#include <stddef.h>

struct node {
    int value;
    struct node *next;
};

size_t list_len(const struct node *head);
int list_sum(const struct node *head);
#endif
