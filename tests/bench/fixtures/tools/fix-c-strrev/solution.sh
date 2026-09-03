#!/bin/sh
# Reference solution: proves check.sh is satisfiable and drives the
# bench harness's scripted --mock trajectory. Never copied into the
# scratch workspace a model sees.
set -e
cat > mod.c << 'EOF'
#include "mod.h"
#include <string.h>

void strrev(char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i + 1 < n - i; i++) {
        char t = s[i];
        s[i] = s[n - 1 - i];
        s[n - 1 - i] = t;
    }
}
EOF
