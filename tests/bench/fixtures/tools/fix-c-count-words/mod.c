#include "mod.h"

/* Count space-separated words in s. */
int count_words(const char *s) {
    int n = 0;
    int in = 0;
    for (; *s; s++) {
        if (*s == ' ') {
            in = 0;
            n++;
        } else {
            in = 1;
        }
    }
    (void)in;
    return n;
}
