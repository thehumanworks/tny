#!/bin/sh
set -eu
workspace=$1
: "$2"
hidden_dir=$(mktemp -d "$workspace/.harness-hidden.XXXXXX")
trap 'rm -rf "$hidden_dir"' EXIT
cp "$(dirname "$0")/hidden_test.py" "$hidden_dir/hidden_test.py"
if [ -f "$(dirname "$0")/swarm_cases.py" ]; then
    cp "$(dirname "$0")/swarm_cases.py" "$hidden_dir/swarm_cases.py"
fi
if python3 "$hidden_dir/hidden_test.py" "$workspace" > "$hidden_dir/output" 2>&1; then
    echo "pass: oracle passed"
else
    echo "fail: oracle failed"
    exit 1
fi
