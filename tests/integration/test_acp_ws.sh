#!/bin/sh
# test_acp_ws.sh — `--agent ws://…`: the ACP client over a WebSocket instead
# of a spawned process (docs/adr/0017). Same fake agent, same transcript,
# reached through tests/integration/fake_acp_agent_ws.py. Runs against
# whatever $1/$TNY points at — the native binary and the wasm build both.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TNY=${1:-${TNY:-$ROOT/build/tny}}
WRAP=$ROOT/tests/integration/fake_acp_agent_ws.py
PY=${PYTHON:-python3}

TMP=$(mktemp -d)
trap 'kill $WSPID 2>/dev/null; rm -rf "$TMP"' EXIT
mkdir -p "$TMP/ws" "$TMP/home"

fail() { echo "FAIL: $*" >&2; exit 1; }
contains() { case "$1" in *"$2"*) ;; *) fail "missing '$2' in: $1" ;; esac; }

FAKE_ACP_STATE=$TMP/state.json "$PY" "$WRAP" 0 > "$TMP/wrap.out" 2> "$TMP/wrap.err" &
WSPID=$!
i=0
PORT=
while [ $i -lt 50 ]; do
    PORT=$(sed -n 's/^ready on //p' "$TMP/wrap.out" 2>/dev/null)
    [ -n "$PORT" ] && break
    i=$((i + 1))
    sleep 0.1
done
[ -n "$PORT" ] || fail "ws wrapper did not start"

# ---- turn 1: full turn over the WebSocket transport --------------------
OUT1=$(HOME="$TMP/home" "$TNY" --cwd "$TMP/ws" --backend acp \
    --agent "ws://127.0.0.1:$PORT" ask --json --yolo "hello over ws" \
    2> "$TMP/err1") || fail "run 1 exited $? ($(cat "$TMP/err1"))"
TEXT=$(printf '%s' "$OUT1" | "$PY" -c 'import json,sys; print(json.load(sys.stdin)["output"])')
contains "$TEXT" "Hello from the fake ACP agent."
SID=$(printf '%s' "$OUT1" | "$PY" -c 'import json,sys; print(json.load(sys.stdin)["session_id"])')
[ -n "$SID" ] || fail "no session id"
grep -q "fake-agent:" "$TMP/wrap.err" || fail "agent stderr was not forwarded through the wrapper"
echo "ok  turn 1 over ws: streamed text, session $SID"

# ---- turn 2: resume rides session/load over the same transport ---------
OUT2=$(HOME="$TMP/home" FAKE_ACP_STATE=$TMP/state.json "$TNY" --cwd "$TMP/ws" \
    --backend acp --agent "ws://127.0.0.1:$PORT" ask --json --yolo \
    --resume "$SID" "again" 2> "$TMP/err2") \
    || fail "run 2 exited $? ($(cat "$TMP/err2"))"
TEXT2=$(printf '%s' "$OUT2" | "$PY" -c 'import json,sys; print(json.load(sys.stdin)["output"])')
contains "$TEXT2" "Hello from the fake ACP agent."
echo "ok  turn 2 over ws: resumed"

# ---- refused connection: clean startup error, no hang ------------------
if HOME="$TMP/home" "$TNY" --cwd "$TMP/ws" --backend acp \
    --agent "ws://127.0.0.1:1" ask --yolo "hi" > "$TMP/out3" 2> "$TMP/err3"; then
    fail "run 3 should have failed"
fi
grep -qi "acp" "$TMP/err3" || fail "no acp error line: $(cat "$TMP/err3")"
echo "ok  refused ws connect is a clean startup error"

# ---- doctor: a ws agent resolves without a PATH lookup -----------------
DOC=$(HOME="$TMP/home" "$TNY" --backend acp --agent "ws://127.0.0.1:$PORT" \
    doctor --json 2>/dev/null) || true
contains "$DOC" "remote agent"
echo "ok  doctor reports the remote agent"

echo "all acp-ws integration tests passed"
