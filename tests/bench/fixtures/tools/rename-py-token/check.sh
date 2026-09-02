#!/bin/sh
set -e
if grep -rn -w "tok" . > /dev/null 2>&1; then
    echo "old name tok still present"
    grep -rn -w "tok" .
    exit 1
fi
n=$(grep -rn -w "tokenize" . | wc -l | tr -d " ")
if [ "$n" -lt 3 ]; then
    echo "only $n uses of tokenize"
    exit 1
fi
python3 cli.py
