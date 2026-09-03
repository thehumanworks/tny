#!/bin/sh
# Reference solution: proves check.sh is satisfiable and drives the
# bench harness's scripted --mock trajectory. Never copied into the
# scratch workspace a model sees.
set -e
cat > mod.py << 'EOF'
def median(xs):
    s = sorted(xs)
    m = len(s) // 2
    if len(s) % 2:
        return s[m]
    return (s[m - 1] + s[m]) / 2
EOF
