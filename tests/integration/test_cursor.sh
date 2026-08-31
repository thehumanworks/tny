#!/bin/sh
# test_cursor.sh — end-to-end check of `--backend cursor` against the mock
# SDK bridge: spawn + ready line + Ping + ListModels + CreateAgent + Send
# stream + Shutdown, then a --resume run that must reuse the same agent id.
#
# Usage: tests/integration/test_cursor.sh [path/to/tny]   (default build-cursor/tny)
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TNY=${1:-$ROOT/build-cursor/tny}
MOCK=$ROOT/tests/integration/mock_bridge.py

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

[ -x "$TNY" ] || fail "no tny binary at $TNY (run: make BUILD=build-cursor release)"
[ -f "$MOCK" ] || fail "missing $MOCK"
command -v python3 > /dev/null 2>&1 || fail "python3 is required"
chmod +x "$MOCK"

# Resolve the interpreter *before* HOME is a throwaway dir. macOS shims
# and site-packages init hang or miss python3 once HOME is overridden,
# so the mock never prints a ready line within 30 s.
PY=$(python3 -c 'import sys; print(sys.executable)')
[ -n "$PY" ] || fail "could not resolve python3 executable"
REAL_HOME=${HOME:-/tmp}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/tny-cursor.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
HOME=$TMP/home
WS=$TMP/ws
MOCK_DIR=$TMP/bridge
mkdir -p "$HOME" "$WS" "$MOCK_DIR"
echo "hello" > "$WS/README.md"

export HOME
export TNY_MOCK_DIR="$MOCK_DIR"
export TNY_MOCK_CWD="$WS"
export CURSOR_API_KEY=key_mock_deadbeef
unset CURSOR_SDK_BRIDGE_BIN

WRAP=$TMP/mock-bridge
# Keep the real HOME for the interpreter; the mock uses TNY_MOCK_DIR.
printf '#!/bin/sh\nexport HOME="%s"\nexec "%s" -u "%s" "$@"\n' \
    "$REAL_HOME" "$PY" "$MOCK" > "$WRAP"
chmod +x "$WRAP"
MOCK=$WRAP

run() { # run <outfile> <errfile> <ask args...>
    _out=$1
    _err=$2
    shift 2
    set +e
    "$TNY" --backend cursor --bridge-bin "$MOCK" --cwd "$WS" ask --json "$@" \
        > "$_out" 2> "$_err"
    _code=$?
    set -e
    return $_code
}

check_mock_assertions() {
    if [ -s "$MOCK_DIR/failures.log" ]; then
        cat "$MOCK_DIR/failures.log" >&2
        fail "the mock bridge recorded protocol assertion failures"
    fi
}

check_no_secret_leak() { # check_no_secret_leak <file>...
    _tok=$(tr -d '\n' < "$MOCK_DIR/auth.token" 2> /dev/null || true)
    for f in "$@"; do
        if grep -q "cursor-sdk-bridge ready" "$f"; then
            fail "the bridge ready line leaked into $f"
        fi
        if [ -n "$_tok" ] && grep -qF "$_tok" "$f"; then
            fail "the bridge bearer token leaked into $f"
        fi
        if grep -qF "$CURSOR_API_KEY" "$f"; then
            fail "CURSOR_API_KEY leaked into $f"
        fi
    done
}

# ---- run 1: fresh agent ----
echo "== run 1: new session"
if run "$TMP/1.out" "$TMP/1.err" "hi"; then :; else
    cat "$TMP/1.err" >&2
    fail "the first ask exited nonzero"
fi
cat "$TMP/1.err" >&2
if ! grep -q "CURSOR-MOCK-OK" "$TMP/1.out"; then
    cat "$TMP/1.out" >&2
    fail "the first ask did not stream the mock answer"
fi
# tool calls must render named with clipped args/results, never as an
# opaque "tool" line (the payload nests a tool_call.<variant>ToolCall union)
grep -q "⏺ read .*README.md" "$TMP/1.err" ||
    fail "the tool start line did not show the tool name and args"
[ "$(grep -c "⏺ read" "$TMP/1.err")" = "1" ] ||
    fail "a re-emitted running frame rendered a duplicate tool start line"
grep -q "✓ read" "$TMP/1.err" || fail "the tool end line did not show the tool name"
if grep -q "⏺ tool" "$TMP/1.err"; then
    fail "a tool call rendered as an opaque 'tool' line"
fi
grep -q '"name":"read"' "$TMP/1.out" ||
    fail "ask --json did not log the named tool call"
check_mock_assertions
[ -f "$MOCK_DIR/agent.txt" ] || fail "the mock never saw CreateAgent"
AGENT=$(cat "$MOCK_DIR/agent.txt")
check_no_secret_leak "$TMP/1.out" "$TMP/1.err"

SESSION=$(sed -n 's/.*"session_id":"\([^"]*\)".*/\1/p' "$TMP/1.out")
[ -n "$SESSION" ] || fail "no session_id in the JSON output"
SESSION_FILE=$(find "$HOME"/.tny/sessions -path "*/$SESSION/session.json" -print -quit)
[ -n "$SESSION_FILE" ] || fail "session.json was not persisted"
STORED=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["host_pointer"])' \
    "$SESSION_FILE")
case "$STORED" in
    cursor-sdk.v1:*) ;;
    *) fail "session stored an unversioned Cursor host pointer '$STORED'" ;;
esac
STORED_AGENT=$(python3 -c \
    'import json,sys; print(json.loads(sys.argv[1].split(":", 1)[1])["agent_id"])' "$STORED")
[ "$STORED_AGENT" = "$AGENT" ] ||
    fail "session stored agent '$STORED_AGENT', want '$AGENT'"

# ---- run 2: resume must reuse the same agent id ----
echo "== run 2: --resume $SESSION"
if run "$TMP/2.out" "$TMP/2.err" --resume "$SESSION" "again"; then :; else
    cat "$TMP/2.err" >&2
    fail "the resumed ask exited nonzero"
fi
cat "$TMP/2.err" >&2
grep -q "CURSOR-MOCK-OK" "$TMP/2.out" || fail "the resumed ask did not stream the mock answer"
check_mock_assertions
[ -f "$MOCK_DIR/resumed.txt" ] || fail "the mock never saw ResumeAgent"
[ "$(cat "$MOCK_DIR/resumed.txt")" = "$AGENT" ] || fail "ResumeAgent used a different agent id"
check_no_secret_leak "$TMP/2.out" "$TMP/2.err"

# ---- run 3: --effort light resolves against the catalog as "low" ----
# The canonical "light" is not in the mock's value list; tny must map it and
# send ModelSelection.params [{"id":"effort","value":"low"}] on CreateAgent
# and on SendOptions.model (the mock rejects anything else).
echo "== run 3: --effort light -> catalog value low"
export TNY_MOCK_EXPECT_EFFORT=low
set +e
"$TNY" --backend cursor --bridge-bin "$MOCK" --cwd "$WS" --effort light \
    ask --json --no-save "hi again" > "$TMP/3.out" 2> "$TMP/3.err"
_code=$?
set -e
unset TNY_MOCK_EXPECT_EFFORT
if [ $_code -ne 0 ]; then
    cat "$TMP/3.err" >&2
    fail "the --effort ask exited nonzero"
fi
cat "$TMP/3.err" >&2
grep -q "CURSOR-MOCK-OK" "$TMP/3.out" || fail "the --effort ask did not stream the mock answer"

check_mock_assertions
check_no_secret_leak "$TMP/3.out" "$TMP/3.err"

# ---- run 4: --fast must ride CreateAgent as a model "fast" param ----
echo "== run 4: --fast"
set +e
TNY_MOCK_FAST=1 "$TNY" --backend cursor --bridge-bin "$MOCK" --cwd "$WS" --fast \
    ask --json --no-save "quick" > "$TMP/4.out" 2> "$TMP/4.err"
CODE=$?
set -e
if [ $CODE -ne 0 ]; then
    cat "$TMP/4.err" >&2
    fail "the --fast ask exited nonzero"
fi
grep -q "CURSOR-MOCK-OK" "$TMP/4.out" || fail "the --fast ask did not stream the mock answer"
check_mock_assertions
check_no_secret_leak "$TMP/4.out" "$TMP/4.err"

# ---- run 5: --effort and --fast compose into one params array ----
echo "== run 5: --effort light --fast"
export TNY_MOCK_EXPECT_EFFORT=low
set +e
TNY_MOCK_FAST=1 "$TNY" --backend cursor --bridge-bin "$MOCK" --cwd "$WS" \
    --effort light --fast ask --json --no-save "both" > "$TMP/5.out" 2> "$TMP/5.err"
CODE=$?
set -e
unset TNY_MOCK_EXPECT_EFFORT
if [ $CODE -ne 0 ]; then
    cat "$TMP/5.err" >&2
    fail "the --effort --fast ask exited nonzero"
fi
grep -q "CURSOR-MOCK-OK" "$TMP/5.out" || fail "the --effort --fast ask did not stream the mock answer"
check_mock_assertions
check_no_secret_leak "$TMP/5.out" "$TMP/5.err"

# ---- the host must not outlive tny ----
if [ -f "$MOCK_DIR/pid.txt" ] && kill -0 "$(cat "$MOCK_DIR/pid.txt")" 2> /dev/null; then
    kill -9 "$(cat "$MOCK_DIR/pid.txt")" 2> /dev/null || true
    fail "the mock bridge survived tny exit"
fi

echo "PASS: cursor bridge backend (agent $AGENT reused on resume)"
