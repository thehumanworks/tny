#!/bin/sh
[ -f answer.txt ] || {
    echo "answer.txt missing"
    exit 1
}
got=$(tr -d ' \t\r"' < answer.txt | sed '/^$/d' | head -n 1 | sed 's#^\./##')
if [ "$got" != "app.py:9" ]; then
    echo "answer $got, want app.py:9"
    exit 1
fi
