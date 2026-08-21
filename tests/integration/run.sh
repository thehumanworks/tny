#!/bin/sh
# tests/integration/run.sh — run every integration test against $TNY
# (default: the release binary). Fixture-only; no live keys (CLAUDE.md).
cd "$(dirname "$0")/../.." || exit 1

TNY="${TNY:-$PWD/build/tny}"
if [ ! -x "$TNY" ]; then
    echo "run.sh: $TNY not found — run 'make release' first" >&2
    exit 1
fi
export TNY

fail=0
run() {
    name=$1; shift
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
    # Honor each script's shebang (test_codex.sh is bash; dash rejects pipefail).
    run "$(basename "$t" .sh)" "$t" "$TNY"
done

for t in tests/integration/test_*.py; do
    [ -e "$t" ] || continue
    case "$t" in */test_openai.py) continue ;; esac
    run "$(basename "$t" .py)" python3 "$t" "$TNY"
done

exit $fail
