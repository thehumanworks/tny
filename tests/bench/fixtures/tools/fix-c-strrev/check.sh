#!/bin/sh
out="$(mktemp -t tnybench)" || exit 1
trap 'rm -f "$out"' EXIT
cc -std=c11 -o "$out" mod.c test_mod.c 2>&1 || exit 1
exec "$out"
