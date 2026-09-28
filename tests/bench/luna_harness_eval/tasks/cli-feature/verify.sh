#!/bin/sh
set -eu
script_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
workspace=$(CDPATH='' cd "$1" && pwd)
run_dir=$(CDPATH='' cd "$(dirname "$2")" && pwd)
verify_log=$run_dir/verify.log
hidden_dir=$(mktemp -d "$run_dir/.harness-hidden.XXXXXX")
trap 'rm -rf "$hidden_dir"' EXIT
cp "$script_dir/hidden_test.py" "$hidden_dir/hidden_test.py"
if [ -f "$script_dir/swarm_cases.py" ]; then
    cp "$script_dir/swarm_cases.py" "$hidden_dir/swarm_cases.py"
fi
cd "$workspace"
if python3 "$hidden_dir/hidden_test.py" "$workspace" > "$verify_log" 2>&1; then
    echo "pass: oracle passed"
else
    echo "fail: oracle failed; see $verify_log"
    exit 1
fi
