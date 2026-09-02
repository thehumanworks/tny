#!/bin/sh
set -e
if grep -rn -w "getcfg" . > /dev/null 2>&1; then
    echo "old name getcfg still present"
    grep -rn -w "getcfg" .
    exit 1
fi
n=$(grep -rn -w "get_config" . | wc -l | tr -d " ")
if [ "$n" -lt 3 ]; then
    echo "only $n uses of get_config"
    exit 1
fi
python3 cli.py
