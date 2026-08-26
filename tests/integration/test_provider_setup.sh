#!/bin/sh
# test_provider_setup.sh — `tny provider setup` (docs/adr/0018): the
# noninteractive flag path, the stored key driving a real turn against the
# strict openai mock, error paths, and secret hygiene. Runs against any
# $TNY — the native binary and the wasm build both.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TNY=${1:-${TNY:-$ROOT/build/tny}}
MOCK=$ROOT/tests/integration/mock_openai.py
PY=${PYTHON:-python3}

TMP=$(mktemp -d)
trap 'kill $MPID 2>/dev/null; rm -rf "$TMP"' EXIT
mkdir -p "$TMP/ws" "$TMP/home"
touch "$TMP/ws/a.txt" "$TMP/ws/b.txt"

fail() { echo "FAIL: $*" >&2; exit 1; }
contains() { case "$1" in *"$2"*) ;; *) fail "missing '$2' in: $1" ;; esac; }

PORT=$("$PY" -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
MOCK_EXPECT_WIRE=responses "$PY" "$MOCK" "$PORT" > "$TMP/mock.out" 2> "$TMP/mock.err" &
MPID=$!
# macOS CI pythons cold-start slowly (see run.sh's cursor note): allow 30 s
i=0
while [ $i -lt 300 ]; do
    grep -q "ready" "$TMP/mock.out" 2>/dev/null && break
    i=$((i + 1))
    sleep 0.1
done
grep -q "ready" "$TMP/mock.out" \
    || fail "openai mock did not start ($(tail -3 "$TMP/mock.err" 2>/dev/null))"

# every run below must resolve the key from settings.json, never the shell
unset OPENAI_API_KEY OPENAI_BASE_URL || true

# ---- setup writes the profile, makes it the default, chmods 0600 --------
OUT=$(HOME="$TMP/home" "$TNY" provider setup opencode \
    --base-url "http://127.0.0.1:$PORT/v1" --api-key sk-setup-not-real \
    --model mock-model 2>&1) || fail "setup exited $? ($OUT)"
contains "$OUT" "provider 'opencode'"
grep -q '"api_key": "sk-setup-not-real"' "$TMP/home/.tny/settings.json" \
    || fail "key not stored: $(cat "$TMP/home/.tny/settings.json")"
grep -q '"last_provider": "opencode"' "$TMP/home/.tny/settings.json" \
    || fail "last_provider not set"
case "$(ls -l "$TMP/home/.tny/settings.json" | cut -c1-10)" in
    -rw-------) ;;
    *) fail "settings.json not 0600 after storing a key: $(ls -l "$TMP/home/.tny/settings.json")" ;;
esac
echo "ok  setup wrote the profile (0600, last_provider)"

# ---- a bare ask runs on the stored profile with no env at all -----------
OUT=$(HOME="$TMP/home" "$TNY" --cwd "$TMP/ws" ask --json --no-save \
    "list files in ." 2> "$TMP/err1") || fail "ask exited $? ($(cat "$TMP/err1"))"
contains "$OUT" '"provider":"opencode"'
contains "$OUT" "MOCK-OK"
echo "ok  bare ask used the stored provider + key"

# ---- the stored key must never leak into output -------------------------
case "$OUT$(cat "$TMP/err1")" in
    *sk-setup-not-real*) fail "stored key leaked into ask output" ;;
esac
echo "ok  key stayed out of the output"

# ---- error paths: host provider, missing base url, no tty no flags ------
HOME="$TMP/home" "$TNY" provider setup codex --base-url http://h/v1 \
    > /dev/null 2> "$TMP/err2" && fail "host provider accepted"
contains "$(cat "$TMP/err2")" "host provider"
HOME="$TMP/home" "$TNY" provider setup fresh --api-key sk-x \
    > /dev/null 2> "$TMP/err3" && fail "profile without base url accepted"
contains "$(cat "$TMP/err3")" "base-url"
HOME="$TMP/home" "$TNY" provider setup < /dev/null \
    > /dev/null 2> "$TMP/err4" && fail "no name, no tty accepted"
contains "$(cat "$TMP/err4")" "needs a NAME"
echo "ok  error paths refuse cleanly"

# ---- wire-api rides the profile; bad values refused ---------------------
HOME="$TMP/home" "$TNY" provider setup opencode --wire-api chat \
    > /dev/null 2>&1 || fail "wire-api update failed"
grep -q '"wire_api": "chat"' "$TMP/home/.tny/settings.json" \
    || fail "wire_api not stored"
HOME="$TMP/home" "$TNY" provider setup opencode --wire-api grpc \
    > /dev/null 2> "$TMP/err5" && fail "bad wire-api accepted"
contains "$(cat "$TMP/err5")" "responses|chat"
HOME="$TMP/home" "$TNY" provider setup opencode --wire-api responses \
    > /dev/null 2>&1 || fail "wire-api reset failed"
echo "ok  --wire-api stored and validated"

# ---- an https base url is accepted (no request is made at setup time) ---
HOME="$TMP/home" "$TNY" provider setup securecorp \
    --base-url https://secure.example/v1 --api-key-env SECURECORP_KEY \
    > /dev/null 2>&1 || fail "https base url refused"
grep -q '"https://secure.example/v1"' "$TMP/home/.tny/settings.json" \
    || fail "https base url not stored"
echo "ok  https base url accepted"

# ---- a dangling flag (no value) is an error, not a silent skip ----------
HOME="$TMP/home" "$TNY" provider setup dangler --base-url \
    < /dev/null > /dev/null 2> "$TMP/err6" && fail "dangling --base-url accepted"
grep -q "dangler" "$TMP/home/.tny/settings.json" && fail "dangler profile written"
echo "ok  dangling flag refused"

# ---- bare `provider` / `provider list` route to the listing -------------
SETTINGS="$TMP/home/.tny/settings.json" "$PY" -c '
import json, os
path = os.environ["SETTINGS"]
data = json.load(open(path))
data["acp"] = {"agents": {"fixture": {"command": ["python3", "-V"],
                                         "model": "selected-model"}}}
with open(path, "w") as fh:
    json.dump(data, fh)
'
OUT=$(HOME="$TMP/home" "$TNY" provider --json 2>&1) || fail "provider listing failed"
contains "$OUT" "opencode"
contains "$OUT" '"name":"acp:fixture"'
contains "$OUT" '"backend":"acp"'
OUT=$(HOME="$TMP/home" "$TNY" provider list --json 2>&1) || fail "provider list failed"
contains "$OUT" "opencode"
contains "$OUT" '"name":"acp:fixture"'
HOME="$TMP/home" "$TNY" provider frobnicate > /dev/null 2> "$TMP/err7" \
    && fail "unknown subcommand accepted"
contains "$(cat "$TMP/err7")" "unknown subcommand"
echo "ok  provider / provider list / unknown subcommand"

# ---- interactive prompts through a pty (docs/adr/0018) ------------------
# wasm-node is a CI vehicle, not an interactive surface: Emscripten's fgets
# over a node pty stalls on line delivery, and interactive setup on wasm is
# the TUI wizard (pty-tested in test_tui.py, browser-tested in
# test_site_wasm.py). Prompts here are asserted native-only.
if grep -q "exec node" "$TNY" 2>/dev/null; then
    echo "ok  interactive prompts skipped on wasm-node (TUI wizard covers it)"
else
"$PY" - "$TNY" "$TMP" "$PORT" <<'PYEOF' || fail "interactive pty flow failed"
import os, pty, select, sys, time
tny, tmp, port = sys.argv[1], sys.argv[2], sys.argv[3]
home = os.path.join(tmp, "ptyhome")
os.makedirs(home, exist_ok=True)
env = {k: v for k, v in os.environ.items()
       if k not in ("OPENAI_API_KEY", "OPENAI_BASE_URL")}
env["HOME"] = home

def drive(args, steps, expect_rc=0, refuse=None):
    """steps: (prompt-substring, answer) pairs; each answer is typed only
    after its own prompt paints, so echoed URLs never desync the flow."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(tny, [tny] + args, env)
    buf = b""
    for want, ans in steps:
        end = time.time() + 15
        while want.encode() not in buf:
            if time.time() > end:
                sys.stderr.write("no %r prompt; got: %r\n" % (want, buf[-400:]))
                sys.exit(1)
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try:
                    buf += os.read(fd, 65536)
                except OSError:
                    break
        buf = buf[buf.index(want.encode()) + len(want):]
        os.write(fd, ans.encode() + b"\r")
    end = time.time() + 15
    rc = None
    reaped = False
    while rc is None:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                chunk = b""  # EIO after child exit; reap below
            buf += chunk
        if not reaped:
            wpid, status = os.waitpid(pid, os.WNOHANG)
            if wpid == pid and os.WIFEXITED(status):
                rc = os.WEXITSTATUS(status)
                reaped = True
        if rc is None and time.time() > end:
            sys.stderr.write("pty run hung; got: %r\n" % buf[-400:])
            sys.exit(1)
    if rc != expect_rc:
        sys.stderr.write("rc %d != %d; got: %r\n" % (rc, expect_rc, buf[-400:]))
        sys.exit(1)
    if refuse and refuse.encode() in buf:
        sys.stderr.write("%r echoed to the terminal\n" % refuse)
        sys.exit(1)
    return buf

# full flow, key via $ENV: name -> base url -> key -> model
drive(["provider", "setup"],
      [("provider name", "ptyprov"),
       ("base url", "http://127.0.0.1:%s/v1" % port),
       ("api key", "$PTYPROV_KEY_VAR"),
       ("default model", "pty-model")])
s = open(os.path.join(home, ".tny", "settings.json")).read()
assert '"ptyprov"' in s and '"PTYPROV_KEY_VAR"' in s and '"pty-model"' in s, s

# a raw key answer is stored AND never echoed back (echo off)
drive(["provider", "setup", "maskprov"],
      [("base url", "http://127.0.0.1:%s/v1" % port),
       ("api key", "sk-masked-secret"),
       ("default model", "")],
      refuse="sk-masked-secret")
s = open(os.path.join(home, ".tny", "settings.json")).read()
assert '"sk-masked-secret"' in s, s

# existing provider: the base url prompt is skipped (api key comes first)
buf = drive(["provider", "setup", "maskprov"],
            [("api key", ""), ("default model", "")])
assert b"base url" not in buf, buf[-400:]

# empty name cancels with exit 1
drive(["provider", "setup"], [("provider name", "")], expect_rc=1)
print("pty flows ok")
PYEOF
echo "ok  interactive prompts (pty): env-key, masked key, existing skip, cancel"
fi

# ---- --api-key-env replaces a stored key; env var feeds the turn --------
HOME="$TMP/home" "$TNY" provider setup opencode \
    --api-key-env OPENCODE_KEY_VAR > /dev/null 2>&1 || fail "key-env update failed"
grep -q '"api_key"' "$TMP/home/.tny/settings.json" \
    && fail "stored key survived --api-key-env"
OUT=$(HOME="$TMP/home" OPENCODE_KEY_VAR=sk-env-not-real "$TNY" --cwd "$TMP/ws" \
    ask --json --no-save "list files in ." 2>&1) || fail "env-key ask failed: $OUT"
contains "$OUT" "MOCK-OK"
echo "ok  --api-key-env replaced the stored key"

echo "all provider-setup integration tests passed"
