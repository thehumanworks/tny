#!/bin/sh
set -eu
workspace=$1
: "$2"
hidden_dir=$(mktemp -d "${TMPDIR:-/tmp}/harness-hidden.XXXXXX")
trap 'rm -rf "$hidden_dir"' EXIT
cp "$(dirname "$0")/hidden_test.py" "$hidden_dir/hidden_test.py"
if (cd "$workspace" && python3 -m unittest discover -s tests -p 'test_*.py' -v) > "$hidden_dir/visible.log" 2>&1 &&
    (cd "$workspace" && python3 "$hidden_dir/hidden_test.py" "$workspace") > "$hidden_dir/hidden.log" 2>&1; then
    echo "pass: visible and hidden suites passed"
else
    echo "fail: visible or hidden suite failed"
    exit 1
fi
