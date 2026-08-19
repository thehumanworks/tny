#!/bin/sh
# test_acp.sh — `--backend acp` end to end against tests/integration/fake_acp_agent.py.
# Covers: initialize, session/new, streamed chunks, permission deny + allow,
# and resume through session/load. No network, no real agent.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TNY=${TNY:-$ROOT/build-acp/tny}
AGENT=$ROOT/tests/integration/fake_acp_agent.py

[ -x "$TNY" ] || { echo "test_acp: no binary at $TNY (make BUILD=build-acp release)" >&2; exit 1; }
chmod +x "$AGENT"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/tny-acp.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/home" "$TMP/ws"
export HOME="$TMP/home"
export FAKE_ACP_STATE="$TMP/state.json"
cd "$TMP/ws"

fail() { echo "FAIL: $*" >&2; exit 1; }
contains() { case "$1" in *"$2"*) : ;; *) fail "expected '$2' in: $1" ;; esac; }

state() {
    python3 -c 'import json,os,sys;print(json.load(open(os.environ["FAKE_ACP_STATE"])).get(sys.argv[1]))' "$1"
}
field() {
    python3 -c 'import json,sys;print(json.loads(sys.stdin.read())[sys.argv[1]])' "$1"
}

# ---- turn 1: fresh session, ask-mode denies the agent's permission request ----
OUT1=$("$TNY" --backend acp --agent "$AGENT" ask --json "hello" 2>"$TMP/err1") \
    || fail "run 1 exited $? (stderr: $(cat "$TMP/err1"))"
echo "$OUT1" | python3 -c 'import json,sys; json.load(sys.stdin)' \
    || fail "run 1 did not print one JSON object: $OUT1"

TEXT=$(printf '%s' "$OUT1" | field output)
contains "$TEXT" "Hello from the fake ACP agent."
contains "$TEXT" "DENIED."
BACKEND=$(printf '%s' "$OUT1" | field backend)
[ "$BACKEND" = "acp" ] || fail "backend field is '$BACKEND'"
SID=$(printf '%s' "$OUT1" | field session_id)
[ -n "$SID" ] || fail "no session_id in run 1 output"

[ "$(state initialize_version)" = "1" ] || fail "agent saw protocolVersion $(state initialize_version)"
[ "$(state new_cwd)" = "$(pwd -P)" ] || fail "session/new cwd was $(state new_cwd), want $(pwd -P)"
grep -q "fake-agent: permission outcome" "$TMP/err1" || fail "agent stderr was not forwarded"
echo "ok  turn 1: streamed text, denied permission, session $SID"

# ---- turn 2: resume the same session; --yolo approves ----
OUT2=$("$TNY" --backend acp --agent "$AGENT" ask --json --yolo --resume "$SID" "again" \
       2>"$TMP/err2") || fail "run 2 exited $? (stderr: $(cat "$TMP/err2"))"
TEXT2=$(printf '%s' "$OUT2" | field output)
contains "$TEXT2" "Hello from the fake ACP agent."
contains "$TEXT2" "ALLOWED."
# the session/load replay arrives before send() installs an event sink, so it
# is intentionally not part of the turn output
[ "$(state load_requested)" = "fake-session-1" ] \
    || fail "session/load asked for $(state load_requested)"
[ "$(state loaded)" = "True" ] || fail "agent never loaded the session"
[ "$(state last_prompt)" = "again" ] || fail "agent got prompt '$(state last_prompt)'"
echo "ok  turn 2: session/load resumed fake-session-1, permission allowed"

# ---- partial lines and several messages per read() ----
OUT3=$(FAKE_ACP_CHUNKY=1 "$TNY" --backend acp --agent "$AGENT" ask --json --yolo \
       "chunky" 2>"$TMP/err3") || fail "run 3 exited $? (stderr: $(cat "$TMP/err3"))"
contains "$(printf '%s' "$OUT3" | field output)" "Hello from the fake ACP agent."
echo "ok  framing: partial lines and batched reads reassembled"

# ---- the agent dying mid-turn must end the turn, not hang ----
set +e
OUT4=$(FAKE_ACP_DIE=1 "$TNY" --backend acp --agent "$AGENT" ask --json "boom" 2>"$TMP/err4")
RC4=$?
set -e
[ "$RC4" -eq 2 ] || fail "agent death should exit 2, got $RC4 ($OUT4)"
grep -q "exited (status 3) mid-turn" "$TMP/err4" \
    || fail "no clear mid-turn exit message: $(cat "$TMP/err4")"
echo "ok  agent exit mid-turn: turn ends with an error"

# ---- a missing agent binary fails at connect ----
set +e
"$TNY" --backend acp --agent /nonexistent/acp-agent ask "hi" >/dev/null 2>"$TMP/err5"
RC5=$?
set -e
[ "$RC5" -ne 0 ] || fail "missing agent should not succeed"
echo "ok  missing agent binary: exit $RC5"

# ---- doctor reports the configured agent ----
DOC=$("$TNY" --backend acp --agent "$AGENT" doctor --json)
echo "$DOC" | grep -q '"name":"acp","healthy":true' || fail "doctor did not mark acp healthy: $DOC"
echo "ok  doctor: acp agent resolves"

echo "PASS test_acp.sh"
