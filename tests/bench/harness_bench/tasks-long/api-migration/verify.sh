#!/bin/sh
set -eu
script_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
workspace=$(CDPATH='' cd "$1" && pwd)
run_dir=$(CDPATH='' cd "$(dirname "$2")" && pwd)
verify_log=$run_dir/verify.log
hidden_dir=$(mktemp -d "${TMPDIR:-$run_dir}/harness-hidden.XXXXXX")
trap 'rm -rf "$hidden_dir"' EXIT
cp "$script_dir/hidden_test.py" "$hidden_dir/hidden_test.py"
: > "$verify_log"
printf '%s\n' '=== visible tests ===' >> "$verify_log"
if ! (cd "$workspace" && python3 -m unittest discover -s tests -v) >> "$verify_log" 2>&1; then
    echo "fail: migration suite failed; see $verify_log"
    exit 1
fi
printf '%s\n' '=== hidden migration checks ===' >> "$verify_log"
if ! (cd "$workspace" && python3 "$hidden_dir/hidden_test.py" "$workspace") >> "$verify_log" 2>&1; then
    echo "fail: migration suite failed; see $verify_log"
    exit 1
fi
echo "pass: migration suite passed"
