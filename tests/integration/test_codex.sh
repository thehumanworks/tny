#!/usr/bin/env bash
# test_codex.sh — end-to-end check of `tny --backend codex` against a scripted
# WebSocket app-server (tests/integration/mock_codex_ws.py). No network, no
# real codex binary. Exits nonzero on any failure.
set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
TNY="${TNY:-$root/build-codex/tny}"

if [ ! -x "$TNY" ]; then
  echo "FAIL: $TNY missing — run: make BUILD=build-codex release" >&2
  exit 1
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/tny-codex.XXXXXX")
mock_pid=""
cleanup() {
  [ -n "$mock_pid" ] && kill "$mock_pid" 2>/dev/null
  rm -rf "$tmp"
}
trap cleanup EXIT

fails=0
ok()   { printf 'ok   %s\n' "$1"; }
bad()  { printf 'FAIL %s\n' "$1" >&2; fails=$((fails + 1)); }

# tny gets a throwaway HOME (sessions/settings live there); the mock keeps the
# real one so python3 version managers still resolve.
TNY_HOME="$tmp/home"
mkdir -p "$TNY_HOME" "$tmp/ws"
PYTHON="${PYTHON:-$(command -v python3 || echo /usr/bin/python3)}"
unset OPENAI_API_KEY CODEX_REMOTE_TOKEN 2>/dev/null

# --- token plumbing: the mock demands a bearer on the upgrade -------------
token="s3cr3t-capability-token"
printf '%s\n' "$token" > "$tmp/token"
export MOCK_TOKEN="$token"
export MOCK_CONNECTIONS=6
export MOCK_BUSY_CONN=3   # connection 3 replies -32001 once per request kind
export MOCK_EXPECT_EFFORT="xhigh"  # run 1 passes --effort xhigh; others none
export MOCK_FAST_CONN=6   # connection 6 runs with --fast: serviceTier=priority

"$PYTHON" "$here/mock_codex_ws.py" 0 > "$tmp/mock.out" 2> "$tmp/mock.err" &
mock_pid=$!

port=""
for _ in $(seq 1 100); do
  port=$(sed -n 's/^ready on \([0-9][0-9]*\)$/\1/p' "$tmp/mock.out" 2>/dev/null | head -1)
  [ -n "$port" ] && break
  sleep 0.1
done
if [ -z "$port" ]; then
  bad "mock app-server never printed a ready line"
  cat "$tmp/mock.err" >&2
  exit 1
fi
url="ws://127.0.0.1:$port"
ok "mock app-server listening on $url"

# --- run 1: fresh thread, token from --ws-token-file ---------------------
# --effort xhigh must land as turn/start.effort (the mock asserts the value)
HOME="$TNY_HOME" "$TNY" --cwd "$tmp/ws" --backend codex --codex-ws "$url" \
       --ws-token-file "$tmp/token" --effort xhigh \
       ask --json --yolo "hi" > "$tmp/run1.out" 2> "$tmp/run1.err"
rc1=$?

[ $rc1 -eq 0 ] && ok "run 1 exit 0" || bad "run 1 exit $rc1 (stderr below)"
grep -q 'CODEX-MOCK-OK' "$tmp/run1.out" \
  && ok "run 1 streamed CODEX-MOCK-OK" \
  || bad "run 1 output did not contain CODEX-MOCK-OK"
grep -q '"provider":"codex"' "$tmp/run1.out" \
  && ok "run 1 reported the codex provider" \
  || bad "run 1 json did not report backend codex"
grep -q 'commandExecution' "$tmp/run1.err" \
  && ok "run 1 rendered the commandExecution tool item" \
  || bad "run 1 did not render the commandExecution item"
grep -q 'tokens: 123 in, 45 out' "$tmp/run1.err" \
  && ok "run 1 mapped the token count notification to USAGE" \
  || bad "run 1 did not report usage 123/45"

sid=$(sed -n 's/.*"session_id":"\([^"]*\)".*/\1/p' "$tmp/run1.out" | head -1)
if [ -z "$sid" ]; then
  bad "run 1 json had no session_id to resume"
else
  ok "run 1 saved session $sid"
fi

# --- run 2: resume must send thread/resume with the same threadId --------
if [ -n "$sid" ]; then
  HOME="$TNY_HOME" CODEX_REMOTE_TOKEN="$token" "$TNY" --cwd "$tmp/ws" --backend codex --codex-ws "$url" \
         ask --json --yolo --resume "$sid" "again" > "$tmp/run2.out" 2> "$tmp/run2.err"
  rc2=$?
  [ $rc2 -eq 0 ] && ok "run 2 exit 0" || bad "run 2 exit $rc2"
  grep -q 'CODEX-MOCK-OK' "$tmp/run2.out" \
    && ok "run 2 streamed CODEX-MOCK-OK" \
    || bad "run 2 output did not contain CODEX-MOCK-OK"
fi

# --- run 3: the host answers -32001 first; the client must back off ------
# The prompt arrives on piped stdin: this also covers cmd_ask overlapping the
# host connect with the stdin read.
printf 'busy' | HOME="$TNY_HOME" CODEX_REMOTE_TOKEN="$token" "$TNY" --cwd "$tmp/ws" --backend codex \
       --codex-ws "$url" ask --json --yolo --no-save \
       > "$tmp/run3.out" 2> "$tmp/run3.err"
rc3=$?
[ $rc3 -eq 0 ] && ok "run 3 exit 0 after -32001 backoff" || bad "run 3 exit $rc3"
grep -q 'CODEX-MOCK-OK' "$tmp/run3.out" \
  && ok "run 3 recovered from -32001 on thread/start and turn/start" \
  || bad "run 3 did not recover from -32001"

# --- run 4: ~/.tny/codex-host.json discovery attaches, no --codex-ws -----
reg="$TNY_HOME/.tny/codex-host.json"
mkdir -p "$TNY_HOME/.tny"
printf '{"ws":"%s","pid":%d}\n' "$url" "$mock_pid" > "$reg"
HOME="$TNY_HOME" CODEX_REMOTE_TOKEN="$token" "$TNY" --cwd "$tmp/ws" --backend codex \
       ask --json --yolo --no-save "registry" > "$tmp/run4.out" 2> "$tmp/run4.err"
rc4=$?
[ $rc4 -eq 0 ] && ok "run 4 exit 0 via registry attach" || bad "run 4 exit $rc4"
grep -q 'CODEX-MOCK-OK' "$tmp/run4.out" \
  && ok "run 4 attached to the registered host" \
  || bad "run 4 did not reach the registered host"
grep -q "\"pid\":$mock_pid" "$reg" 2>/dev/null \
  && ok "run 4 left the foreign host's registry entry alone" \
  || bad "run 4 removed or rewrote a registry entry it does not own"

# --- models: the catalog surfaces per-model reasoning efforts ------------
HOME="$TNY_HOME" CODEX_REMOTE_TOKEN="$token" "$TNY" --cwd "$tmp/ws" --backend codex \
       --codex-ws "$url" models --json > "$tmp/models.out" 2> "$tmp/models.err"
rcm=$?
[ $rcm -eq 0 ] && ok "models exit 0" || bad "models exit $rcm"
grep -q '"efforts":\["low","medium","high","xhigh"\]' "$tmp/models.out" \
  && ok "models surfaced supportedReasoningEfforts" \
  || bad "models json lacked the efforts list: $(cat "$tmp/models.out")"
grep -q '"default_effort":"medium"' "$tmp/models.out" \
  && ok "models surfaced the default effort" \
  || bad "models json lacked default_effort"
grep -q 'mock-hidden-model' "$tmp/models.out" \
  && bad "models listed a hidden catalog entry" \
  || ok "models skipped the hidden catalog entry"

# --- run 5: stale registry falls back to spawn (stub codex bin) ----------
# The registry names a live pid but a dead port; tny must attach-fail fast,
# spawn its own host (the stub wraps the mock), publish its entry, and
# unpublish it on the way out.
cat > "$tmp/codex-stub" <<EOF
#!/bin/sh
# fake codex CLI: expects app-server --listen ws://127.0.0.1:PORT.
# Real HOME again: tny runs with the throwaway one, but python3 version
# managers only resolve under the real home (see the mock note above).
url="\$3"
HOME="$HOME" MOCK_TOKEN= MOCK_CONNECTIONS=1 MOCK_BUSY_CONN=0 MOCK_EXPECT_EFFORT= MOCK_FAST_CONN=0 \
  exec "$PYTHON" "$here/mock_codex_ws.py" "\${url##*:}"
EOF
chmod +x "$tmp/codex-stub"
printf '{"ws":"ws://127.0.0.1:1","pid":%d}\n' "$$" > "$reg"
HOME="$TNY_HOME" TNY_CODEX_BIN="$tmp/codex-stub" "$TNY" --cwd "$tmp/ws" --backend codex \
       ask --json --yolo --no-save "spawnfall" > "$tmp/run5.out" 2> "$tmp/run5.err"
rc5=$?
[ $rc5 -eq 0 ] && ok "run 5 exit 0 after stale-registry fallback" || bad "run 5 exit $rc5"
grep -q 'CODEX-MOCK-OK' "$tmp/run5.out" \
  && ok "run 5 spawned its own host past the stale entry" \
  || bad "run 5 did not fall back to a spawned host"
[ ! -f "$reg" ] \
  && ok "run 5 unpublished its registry entry on exit" \
  || bad "run 5 left $(cat "$reg" 2>/dev/null) behind in the registry"

# --- run 6: --fast must ride thread/start as serviceTier=priority --------
HOME="$TNY_HOME" CODEX_REMOTE_TOKEN="$token" "$TNY" --cwd "$tmp/ws" --backend codex \
       --codex-ws "$url" --fast ask --json --yolo --no-save "speedy" \
       > "$tmp/run6.out" 2> "$tmp/run6.err"
rc6=$?
[ $rc6 -eq 0 ] && ok "run 6 exit 0 with --fast" || bad "run 6 exit $rc6"
grep -q 'CODEX-MOCK-OK' "$tmp/run6.out" \
  && ok "run 6 streamed CODEX-MOCK-OK" \
  || bad "run 6 output did not contain CODEX-MOCK-OK"

# --- mock verdict --------------------------------------------------------
wait "$mock_pid"
mock_rc=$?
mock_pid=""

grep -q 'MOCK-NOTE thread/resume ok' "$tmp/mock.err" \
  && ok "mock saw thread/resume with the stored threadId" \
  || bad "mock never saw a matching thread/resume"
grep -q 'MOCK-NOTE approval answered decision=accept' "$tmp/mock.err" \
  && ok "approval was answered with decision=accept (--yolo)" \
  || bad "approval was not answered with decision=accept"
grep -q 'MOCK-NOTE bearer accepted' "$tmp/mock.err" \
  && ok "bearer token was sent on the upgrade" \
  || bad "mock did not see the Authorization bearer"
grep -q 'MOCK-NOTE turn/start effort=xhigh ok' "$tmp/mock.err" \
  && ok "run 1 carried effort=xhigh on turn/start" \
  || bad "mock never saw turn/start effort=xhigh"
grep -q 'MOCK-NOTE thread/start serviceTier=priority ok' "$tmp/mock.err" \
  && ok "--fast rode thread/start as serviceTier=priority" \
  || bad "mock never saw serviceTier=priority on the --fast run"

if [ $mock_rc -ne 0 ]; then
  bad "mock app-server reported protocol failures"
fi

if [ $fails -ne 0 ]; then
  echo "--- mock log ---" >&2
  cat "$tmp/mock.err" >&2
  for f in run1 run2 run3 run4 run5 run6; do
    [ -f "$tmp/$f.err" ] || continue
    echo "--- $f stderr ---" >&2
    cat "$tmp/$f.err" >&2
    echo "--- $f stdout ---" >&2
    cat "$tmp/$f.out" >&2
  done
  echo "FAILED: $fails check(s)" >&2
  exit 1
fi

echo "PASS: tny --backend codex speaks codex app-server"
exit 0
