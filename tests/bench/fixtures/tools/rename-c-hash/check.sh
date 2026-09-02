#!/bin/sh
set -e
if grep -rn -w "hsh" . > /dev/null 2>&1; then
    echo "old name hsh still present"
    grep -rn -w "hsh" .
    exit 1
fi
n=$(grep -rn -w "hash_str" . | wc -l | tr -d " ")
if [ "$n" -lt 3 ]; then
    echo "only $n uses of hash_str"
    exit 1
fi
out="$(mktemp -t tnybench)" || exit 1
trap 'rm -f "$out"' EXIT
cc -std=c11 -o "$out" mod.c main.c
exec "$out"
