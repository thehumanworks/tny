#!/bin/sh
set -e
if grep -rn -w "vpush" . > /dev/null 2>&1; then
    echo "old name vpush still present"
    grep -rn -w "vpush" .
    exit 1
fi
n=$(grep -rn -w "vec_push" . | wc -l | tr -d " ")
if [ "$n" -lt 3 ]; then
    echo "only $n uses of vec_push"
    exit 1
fi
out="$(mktemp "${TMPDIR:-/tmp}/tnybench.XXXXXX")" || exit 1
trap 'rm -f "$out"' EXIT
cc -std=c11 -o "$out" mod.c main.c
exec "$out"
