#!/bin/sh
set -e
if grep -rn -w "do_retry" . > /dev/null 2>&1; then
    echo "old name do_retry still present"
    grep -rn -w "do_retry" .
    exit 1
fi
n=$(grep -rn -w "retry_with_backoff" . | wc -l | tr -d " ")
if [ "$n" -lt 3 ]; then
    echo "only $n uses of retry_with_backoff"
    exit 1
fi
python3 cli.py
