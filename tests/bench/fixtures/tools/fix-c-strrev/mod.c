#include "mod.h"
#include <string.h>

/* Reverse s in place. */
void strrev(char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        char t = s[i];
        s[i] = s[n - i];
        s[n - i] = t;
    }
}
