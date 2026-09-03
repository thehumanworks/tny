#!/bin/sh
set -e
if grep -rn -w "buf_push" . > /dev/null 2>&1; then
    echo "old name buf_push still present"
    grep -rn -w "buf_push" .
    exit 1
fi
n=$(grep -rn -w "buffer_push" . | wc -l | tr -d " ")
if [ "$n" -lt 3 ]; then
    echo "only $n uses of buffer_push"
    exit 1
fi
out="$(mktemp "${TMPDIR:-/tmp}/tnybench.XXXXXX")" || exit 1
trap 'rm -f "$out"' EXIT
cc -std=c11 -o "$out" mod.c main.c
exec "$out"
