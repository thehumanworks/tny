#!/usr/bin/env bash
set -euo pipefail
workspace=$1
_final_message=$2
if [[ -f "$workspace/hello.txt" ]] && [[ "$(cat "$workspace/hello.txt")" == 'Hello from the harness benchmark.' ]]; then
    echo 'hello.txt has the required line'
    exit 0
fi
echo 'hello.txt missing or incorrect'
exit 1
