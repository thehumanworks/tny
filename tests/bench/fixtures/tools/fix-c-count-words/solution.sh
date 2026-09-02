#!/bin/sh
# Reference solution: proves check.sh is satisfiable and drives the
# bench harness's scripted --mock trajectory. Never copied into the
# scratch workspace a model sees.
set -e
cat > mod.c << 'EOF'
#include "mod.h"

int count_words(const char *s) {
    int n = 0;
    int in = 0;
    for (; *s; s++) {
        if (*s == ' ') {
            in = 0;
        } else if (!in) {
            in = 1;
            n++;
        }
    }
    return n;
}
EOF
