#!/bin/sh
# tests/integration/run.sh — run every integration test against $TNY
# (default: the release binary). Fixture-only; no live keys (CLAUDE.md).
cd "$(dirname "$0")/../.." || exit 1

# Python subprocesses close make's jobserver descriptors. Nested fixture
# builds control their own parallelism; preserve compiler/config environment
# variables, but never forward unusable jobserver tokens or recursion state.
unset MAKEFLAGS MFLAGS MAKELEVEL

# A parent tny harness may use terminal-only tools. Fixtures choose their own
# profiles; do not let the caller hide tools from unrelated integration tests.
unset TNY_TOOLS

TNY="${TNY:-$PWD/build/tny}"
if [ ! -x "$TNY" ]; then
    echo "run.sh: $TNY not found — run 'make release' first" >&2
    exit 1
fi
export TNY

fail=0
run() {
    name=$1
    shift
    echo "== integration: $name"
    if "$@"; then
        echo "   ok"
    else
        echo "   FAIL: $name" >&2
        fail=1
    fi
}

run openai python3 tests/integration/test_openai.py

for t in tests/integration/test_*.sh; do
    [ -e "$t" ] || continue
    # Honor each script's shebang (bash scripts use pipefail; dash rejects it).
    run "$(basename "$t" .sh)" "$t" "$TNY"
done

for t in tests/integration/test_*.py; do
    [ -e "$t" ] || continue
    case "$t" in
        */test_openai.py | */test_help_flags.py) continue ;;
    esac
    run "$(basename "$t" .py)" python3 "$t" "$TNY"
done

exit $fail
