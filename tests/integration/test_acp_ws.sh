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

fail() {
    echo "FAIL: $*" >&2
    for f in "$TMP"/wrap*.err "$TMP"/wrap*.out; do
        [ -s "$f" ] && { echo "--- $(basename "$f"):" >&2; tail -20 "$f" >&2; }
    done
    exit 1
}
contains() { case "$1" in *"$2"*) ;; *) fail "missing '$2' in: $1" ;; esac; }

FAKE_ACP_STATE=$TMP/state.json FAKE_ACP_GROUPED_MODELS=1 \
    "$PY" "$WRAP" 0 > "$TMP/wrap.out" 2> "$TMP/wrap.err" &
WSPID=$!
i=0
PORT=
while [ $i -lt 300 ]; do
    PORT=$(sed -n 's/^ready on //p' "$TMP/wrap.out" 2>/dev/null)
    [ -n "$PORT" ] && break
    i=$((i + 1))
    sleep 0.1
done
[ -n "$PORT" ] || fail "ws wrapper did not start"

# ---- turn 1: full turn over the WebSocket transport --------------------
OUT1=$(HOME="$TMP/home" "$TNY" --cwd "$TMP/ws" --backend acp \
    --agent "ws://127.0.0.1:$PORT" --model ws-model \
    ask --json --yolo "hello over ws" \
    2> "$TMP/err1") || fail "run 1 exited $? ($(cat "$TMP/err1"))"
TEXT=$(printf '%s' "$OUT1" | "$PY" -c 'import json,sys; print(json.load(sys.stdin)["output"])')
contains "$TEXT" "Hello from the fake ACP agent."
SID=$(printf '%s' "$OUT1" | "$PY" -c 'import json,sys; print(json.load(sys.stdin)["session_id"])')
[ -n "$SID" ] || fail "no session id"
MODEL_STATE=$("$PY" -c 'import json,sys; print(json.load(open(sys.argv[1])).get("model_at_prompt"))' \
    "$TMP/state.json")
[ "$MODEL_STATE" = "ws-model" ] || fail "ws prompt ran with model $MODEL_STATE"
grep -q "fake-agent:" "$TMP/wrap.err" || fail "agent stderr was not forwarded through the wrapper"
echo "ok  turn 1 over ws: grouped model selected, streamed text, session $SID"

# ---- turn 2: resume rides session/load over the same transport ---------
OUT2=$(HOME="$TMP/home" FAKE_ACP_STATE=$TMP/state.json "$TNY" --cwd "$TMP/ws" \
    --backend acp --agent "ws://127.0.0.1:$PORT" --model ws-model ask --json --yolo \
    --resume "$SID" "again" 2> "$TMP/err2") \
    || fail "run 2 exited $? ($(cat "$TMP/err2"))"
TEXT2=$(printf '%s' "$OUT2" | "$PY" -c 'import json,sys; print(json.load(sys.stdin)["output"])')
contains "$TEXT2" "Hello from the fake ACP agent."
echo "ok  turn 2 over ws: resumed"

# ---- refused connection: clean startup error (exit 1), no hang ---------
HOME="$TMP/home" "$TNY" --cwd "$TMP/ws" --backend acp \
    --agent "ws://127.0.0.1:1" ask --yolo "hi" > "$TMP/out3" 2> "$TMP/err3"
rc=$?
[ "$rc" -eq 1 ] || fail "refused connect should exit 1, got $rc ($(cat "$TMP/err3"))"
grep -Eq "connect.*failed" "$TMP/err3" || fail "no connect-failed error line: $(cat "$TMP/err3")"
echo "ok  refused ws connect is a clean startup error"

# ---- agent dies mid-turn: the ws close ends the turn with an error -----
FAKE_ACP_DIE=1 "$PY" "$WRAP" 0 > "$TMP/wrap2.out" 2> "$TMP/wrap2.err" &
WS2PID=$!
i=0
PORT2=
while [ $i -lt 300 ]; do
    PORT2=$(sed -n 's/^ready on //p' "$TMP/wrap2.out" 2>/dev/null)
    [ -n "$PORT2" ] && break
    i=$((i + 1))
    sleep 0.1
done
[ -n "$PORT2" ] || { kill $WS2PID 2>/dev/null; fail "second ws wrapper did not start"; }
HOME="$TMP/home" "$TNY" --cwd "$TMP/ws" --backend acp \
    --agent "ws://127.0.0.1:$PORT2" ask --json --yolo --no-save "die please" \
    > "$TMP/out4" 2> "$TMP/err4"
rc=$?
kill $WS2PID 2>/dev/null
[ "$rc" -eq 2 ] || fail "mid-turn ws death should exit 2, got $rc ($(cat "$TMP/err4"))"
grep -q "closed the connection mid-turn" "$TMP/err4" \
    || fail "no mid-turn close error: $(cat "$TMP/err4")"
echo "ok  mid-turn ws close ends the turn with an error"

# ---- doctor: a ws agent resolves without a PATH lookup -----------------
DOC=$(HOME="$TMP/home" "$TNY" --backend acp --agent "ws://127.0.0.1:$PORT" \
    doctor --json 2>/dev/null) || true
contains "$DOC" "remote agent"
echo "ok  doctor reports the remote agent"

echo "all acp-ws integration tests passed"
