#!/bin/sh
set -e
if grep -rn -w "norm_path" . > /dev/null 2>&1; then
    echo "old name norm_path still present"
    grep -rn -w "norm_path" .
    exit 1
fi
n=$(grep -rn -w "normalize_path" . | wc -l | tr -d " ")
if [ "$n" -lt 3 ]; then
    echo "only $n uses of normalize_path"
    exit 1
fi
python3 cli.py
