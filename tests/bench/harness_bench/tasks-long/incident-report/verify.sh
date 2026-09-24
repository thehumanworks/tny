#!/bin/sh
set -eu
workspace=$1
: "$2"
hidden_dir=$(mktemp -d "${TMPDIR:-/tmp}/harness-hidden.XXXXXX")
trap 'rm -rf "$hidden_dir"' EXIT
cp "$(dirname "$0")/hidden_test.py" "$hidden_dir/hidden_test.py"
if (cd "$workspace" && python3 -m unittest discover -s tests -v) > "$hidden_dir/visible.log" 2>&1 &&
    (cd "$workspace" && python3 "$hidden_dir/hidden_test.py" "$workspace") > "$hidden_dir/hidden.log" 2>&1; then
    echo "pass: incident analysis and config passed"
else
    echo "fail: incident analysis or config failed"
    exit 1
fi
