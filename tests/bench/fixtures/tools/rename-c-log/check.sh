#!/bin/sh
set -e
if grep -rn -w "logmsg" . > /dev/null 2>&1; then
    echo "old name logmsg still present"
    grep -rn -w "logmsg" .
    exit 1
fi
n=$(grep -rn -w "log_message" . | wc -l | tr -d " ")
if [ "$n" -lt 3 ]; then
    echo "only $n uses of log_message"
    exit 1
fi
out="$(mktemp "${TMPDIR:-/tmp}/tnybench.XXXXXX")" || exit 1
trap 'rm -f "$out"' EXIT
cc -std=c11 -o "$out" mod.c main.c
exec "$out"
