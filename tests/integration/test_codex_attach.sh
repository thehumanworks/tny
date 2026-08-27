#!/bin/sh
# test_codex_attach.sh — the --codex-ws attach path only (docs/adr/0004),
# against tests/integration/mock_codex_ws.py. No spawn: this is the subset a
# host-less platform supports, so it runs against the native binary AND the
# wasm build (docs/adr/0017). The full lifecycle suite stays test_codex.sh.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TNY=${1:-${TNY:-$ROOT/build/tny}}
MOCK=$ROOT/tests/integration/mock_codex_ws.py
PY=${PYTHON:-python3}

TMP=$(mktemp -d)
trap 'kill $MPID 2>/dev/null; rm -rf "$TMP"' EXIT
mkdir -p "$TMP/ws" "$TMP/home"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}
contains() { case "$1" in *"$2"*) ;; *) fail "missing '$2' in: $1" ;; esac }

"$PY" "$MOCK" 0 > "$TMP/mock.out" 2> "$TMP/mock.err" &
MPID=$!
i=0
PORT=
while [ $i -lt 300 ]; do
    PORT=$(sed -n 's/^ready on //p' "$TMP/mock.out" 2> /dev/null)
    [ -n "$PORT" ] && break
    i=$((i + 1))
    sleep 0.1
done
[ -n "$PORT" ] || fail "codex mock did not start ($(cat "$TMP/mock.err"))"

OUT=$(HOME="$TMP/home" "$TNY" --cwd "$TMP/ws" --backend codex \
    --codex-ws "ws://127.0.0.1:$PORT" ask --json --yolo --no-save "attach" \
    2> "$TMP/err") || fail "attach run exited $? ($(cat "$TMP/err"))"
TEXT=$(printf '%s' "$OUT" | "$PY" -c 'import json,sys; print(json.load(sys.stdin)["output"])')
contains "$TEXT" "CODEX-MOCK-OK"
contains "$OUT" '"provider":"codex"'
grep -q "MOCK-FAIL" "$TMP/mock.err" && fail "mock assertions failed: $(grep MOCK-FAIL "$TMP/mock.err")"
echo "ok  codex attach: streamed CODEX-MOCK-OK, mock wire assertions clean"

echo "all codex-attach integration tests passed"
