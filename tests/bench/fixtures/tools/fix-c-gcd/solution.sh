#!/bin/sh
# Reference solution: proves check.sh is satisfiable and drives the
# bench harness's scripted --mock trajectory. Never copied into the
# scratch workspace a model sees.
set -e
cat > mod.c << 'EOF'
#include "mod.h"

int gcd(int a, int b) {
    while (b != 0) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a;
}
EOF
