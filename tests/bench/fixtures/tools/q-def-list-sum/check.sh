#!/bin/sh
[ -f answer.txt ] || {
    echo "answer.txt missing"
    exit 1
}
got=$(tr -d ' \t\r"' < answer.txt | sed '/^$/d' | head -n 1 | sed 's#^\./##')
if [ "$got" != "list.c:12" ]; then
    echo "answer $got, want list.c:12"
    exit 1
fi
