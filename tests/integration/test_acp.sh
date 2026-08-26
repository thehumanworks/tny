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

# ---- turn 1: fresh session, explicit ask-mode denies the agent's permission
# request (ask is opt-in since docs/adr/0001; the default is yolo) ----
OUT1=$("$TNY" --backend acp --agent "$AGENT" --model selected-model \
       --permission-mode ask ask --json "hello" \
       2>"$TMP/err1") || fail "run 1 exited $? (stderr: $(cat "$TMP/err1"))"
echo "$OUT1" | python3 -c 'import json,sys; json.load(sys.stdin)' \
    || fail "run 1 did not print one JSON object: $OUT1"

TEXT=$(printf '%s' "$OUT1" | field output)
contains "$TEXT" "Hello from the fake ACP agent."
contains "$TEXT" "DENIED."
BACKEND=$(printf '%s' "$OUT1" | field provider)
[ "$BACKEND" = "acp" ] || fail "provider field is '$BACKEND'"
SID=$(printf '%s' "$OUT1" | field session_id)
[ -n "$SID" ] || fail "no session_id in run 1 output"

[ "$(state initialize_version)" = "1" ] || fail "agent saw protocolVersion $(state initialize_version)"
[ "$(state new_cwd)" = "$(pwd -P)" ] || fail "session/new cwd was $(state new_cwd), want $(pwd -P)"
[ "$(state set_config_id)" = "model" ] || fail "model config id was $(state set_config_id)"
[ "$(state set_config_value)" = "selected-model" ] \
    || fail "model config value was $(state set_config_value)"
[ "$(state model_at_prompt)" = "selected-model" ] \
    || fail "prompt ran with model $(state model_at_prompt)"
grep -q "fake-agent: permission outcome" "$TMP/err1" || fail "agent stderr was not forwarded"
echo "ok  turn 1: model selected before prompt, streamed text, denied permission, session $SID"

# ---- turn 2: resume the same session; --yolo approves ----
OUT2=$("$TNY" --backend acp --agent "$AGENT" --model selected-model \
       ask --json --yolo --resume "$SID" "again" \
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
[ "$(state model_at_prompt)" = "selected-model" ] \
    || fail "resumed prompt ran with model $(state model_at_prompt)"
echo "ok  turn 2: session/load selected model before resumed prompt, permission allowed"

# ---- no mode flag at all: the default is yolo, so the request is allowed ----
mkdir -p "$TMP/default-home"
OUTD=$(HOME="$TMP/default-home" "$TNY" --backend acp --agent "$AGENT" \
       ask --json "default mode" \
       2>"$TMP/errd") || fail "default run exited $? (stderr: $(cat "$TMP/errd"))"
contains "$(printf '%s' "$OUTD" | field output)" "ALLOWED."
echo "ok  default permission mode is yolo (docs/adr/0001)"

# ---- semantic category + grouped values work over stdio too -------------
mkdir -p "$TMP/grouped-home"
OUTG=$(HOME="$TMP/grouped-home" FAKE_ACP_GROUPED_MODELS=1 \
       "$TNY" --backend acp --agent "$AGENT" --model selected-model \
       ask --json --yolo "grouped model" 2>"$TMP/err-grouped") \
       || fail "grouped model run exited $? ($(cat "$TMP/err-grouped"))"
contains "$(printf '%s' "$OUTG" | field output)" "ALLOWED."
[ "$(state set_config_id)" = "engine" ] \
    || fail "grouped model config id was $(state set_config_id)"
echo "ok  semantic model category and grouped values selected over stdio"

# ---- partial lines and several messages per read() ----
OUT3=$(FAKE_ACP_CHUNKY=1 "$TNY" --backend acp --agent "$AGENT" ask --json --yolo \
       "chunky" 2>"$TMP/err3") || fail "run 3 exited $? (stderr: $(cat "$TMP/err3"))"
contains "$(printf '%s' "$OUT3" | field output)" "Hello from the fake ACP agent."
echo "ok  framing: partial lines and batched reads reassembled"

# ---- explicit model selection fails closed instead of using the default ----
set +e
FAKE_ACP_NO_MODELS=1 "$TNY" --backend acp --agent "$AGENT" --model selected-model \
    ask "missing selector" >"$TMP/out-no-models" 2>"$TMP/err-no-models"
RC_NO_MODELS=$?
set -e
[ "$RC_NO_MODELS" -eq 1 ] || fail "missing model selector should exit 1, got $RC_NO_MODELS"
grep -q "did not advertise a selectable model option" "$TMP/err-no-models" \
    || fail "missing selector error was unclear: $(cat "$TMP/err-no-models")"

set +e
FAKE_ACP_GROUPED_MODELS=1 "$TNY" --backend acp --agent "$AGENT" \
    --model absent-model ask "unsupported" \
    >"$TMP/out-unsupported" 2>"$TMP/err-unsupported"
RC_UNSUPPORTED=$?
set -e
[ "$RC_UNSUPPORTED" -eq 1 ] || fail "unsupported model should exit 1, got $RC_UNSUPPORTED"
grep -q "requested model 'absent-model' is not advertised" "$TMP/err-unsupported" \
    || fail "unsupported model error was unclear: $(cat "$TMP/err-unsupported")"

set +e
FAKE_ACP_NO_MODEL_VALUES=1 "$TNY" --backend acp --agent "$AGENT" \
    --model selected-model ask "malformed selector" \
    >"$TMP/out-no-values" 2>"$TMP/err-no-values"
RC_NO_VALUES=$?
set -e
[ "$RC_NO_VALUES" -eq 1 ] || fail "selector without values should exit 1, got $RC_NO_VALUES"
grep -q "requested model 'selected-model' is not advertised" "$TMP/err-no-values" \
    || fail "selector-without-values error was unclear: $(cat "$TMP/err-no-values")"

set +e
FAKE_ACP_REJECT_MODEL=1 "$TNY" --backend acp --agent "$AGENT" --model selected-model \
    ask "rejected" >"$TMP/out-rejected" 2>"$TMP/err-rejected"
RC_REJECTED=$?
set -e
[ "$RC_REJECTED" -eq 1 ] || fail "rejected model should exit 1, got $RC_REJECTED"
grep -q "session/set_config_option failed: model selection rejected" "$TMP/err-rejected" \
    || fail "rejected model error was unclear: $(cat "$TMP/err-rejected")"

set +e
FAKE_ACP_BAD_MODEL_CONFIRM=1 "$TNY" --backend acp --agent "$AGENT" \
    --model selected-model ask "not confirmed" \
    >"$TMP/out-confirm" 2>"$TMP/err-confirm"
RC_CONFIRM=$?
set -e
[ "$RC_CONFIRM" -eq 1 ] || fail "unconfirmed model should exit 1, got $RC_CONFIRM"
grep -q "did not confirm requested model 'selected-model'" "$TMP/err-confirm" \
    || fail "unconfirmed model error was unclear: $(cat "$TMP/err-confirm")"
echo "ok  model selection fails clearly when missing, unsupported, rejected, or unconfirmed"

# ---- settings.json named ACP profile drives the same model lifecycle -----
AGENT_PATH="$AGENT" python3 -c '
import json, os
path = os.path.join(os.environ["HOME"], ".tny", "settings.json")
os.makedirs(os.path.dirname(path), exist_ok=True)
try:
    data = json.load(open(path))
except (OSError, ValueError):
    data = {}
data["acp"] = {"agents": {"fixture": {
    "command": [os.environ["AGENT_PATH"]], "model": "selected-model"
}}}
with open(path, "w") as fh:
    json.dump(data, fh)
'
OUTP=$("$TNY" --provider acp:fixture ask --json --yolo "profile model" \
       2>"$TMP/err-profile") || fail "named profile exited $? ($(cat "$TMP/err-profile"))"
[ "$(printf '%s' "$OUTP" | field provider)" = "acp:fixture" ] \
    || fail "named profile output was $(printf '%s' "$OUTP" | field provider)"
[ "$(state model_at_prompt)" = "selected-model" ] \
    || fail "named profile prompt ran with model $(state model_at_prompt)"
echo "ok  settings acp.agents profile selected its configured model"

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
