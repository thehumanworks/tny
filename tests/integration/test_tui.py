#!/usr/bin/env python3
"""pty-driven integration test for the interactive TUI (docs/tui.md).

Covers:
  1. first paint happens with no backend connect (runs with no API key at all)
  2. a typed prompt streams a real turn through the mock provider ("MOCK-OK")
  3. the /quit palette entry exits 0 and the terminal is left cooked
  4. the slash palette filters and dispatches commands
  5. the y/a/n approval UI blocks a sensitive tool and 'n' denies it
  6. the `--version` fast path is untouched

Pure stdlib: pty, os, select, subprocess, termios.
"""
import json
import os
import pty
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import fcntl
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TNY = (sys.argv[1] if len(sys.argv) > 1 else
       os.environ.get("TNY", os.path.join(ROOT, "build", "tny")))
MOCK = os.path.join(HERE, "mock_openai.py")

# The version is derived from git at build time (docs/adr/0014); read it from
# the binary once and assert against that, never against a literal.
VERSION = subprocess.run([TNY, "--version"], capture_output=True, text=True,
                         timeout=10).stdout.strip()
BANNER = f"tny {VERSION}"

ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]|\x1b[()][B0]|\r")


def clean(s):
    return ANSI.sub("", s)


class Screen:
    """Tiny terminal emulator for the escape subset tny emits (tui_draw.c:
    CR, LF, CUU, CUF, ED, SGR, cursor home, 2J/3J). Renders what a user
    would actually SEE, which is the only way to assert that transient
    blocks (popover, overlay) really left the screen."""

    SEQ = re.compile(r"\x1b\[([0-9;?]*)([a-zA-Z])")

    def __init__(self, rows=40, cols=100):
        self.rows, self.cols = rows, cols
        self.grid = [[" "] * cols for _ in range(rows)]
        self.r = self.c = 0

    def _put(self, ch):
        if self.c >= self.cols:
            self.c = 0
            self._lf()
        self.grid[self.r][self.c] = ch
        self.c += 1

    def _lf(self):
        if self.r == self.rows - 1:
            self.grid.pop(0)
            self.grid.append([" "] * self.cols)
        else:
            self.r += 1

    def feed(self, s):
        i = 0
        while i < len(s):
            ch = s[i]
            if ch == "\x1b":
                m = self.SEQ.match(s, i)
                if not m:
                    i += 1
                    continue
                args, fin = m.group(1), m.group(2)
                n = int(args.split(";")[0]) if args and args[0].isdigit() else None
                if fin == "A":
                    self.r = max(0, self.r - (n or 1))
                elif fin == "C":
                    self.c = min(self.cols, self.c + (n or 1))
                elif fin == "J":
                    if n in (None, 0):
                        for j in range(self.c, self.cols):
                            self.grid[self.r][j] = " "
                        for rr in range(self.r + 1, self.rows):
                            self.grid[rr] = [" "] * self.cols
                    else:  # 2J / 3J
                        self.grid = [[" "] * self.cols for _ in range(self.rows)]
                elif fin == "H":
                    self.r = self.c = 0
                # SGR (m) and cursor-visibility are ignored
                i = m.end()
                continue
            if ch == "\r":
                self.c = 0
            elif ch == "\n":
                self.c = 0
                self._lf()
            elif ch == "\b":
                self.c = max(0, self.c - 1)
            elif ch >= " ":
                self._put(ch)
            i += 1

    def text(self):
        return "\n".join("".join(row).rstrip() for row in self.grid)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Term:
    """A child process attached to a pseudo-terminal."""

    def __init__(self, argv, env, cwd):
        self.master, self.slave = pty.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 100, 0, 0))
        self.before = termios.tcgetattr(self.slave)
        self.proc = subprocess.Popen(
            argv, stdin=self.slave, stdout=self.slave, stderr=self.slave,
            env=env, cwd=cwd, close_fds=True, start_new_session=True)
        self.buf = ""

    def pump(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.master], [], [], max(0.0, end - time.time()))
            if not r:
                return
            try:
                data = os.read(self.master, 65536)
            except OSError:
                return
            if not data:
                return
            self.buf += data.decode("utf-8", "replace")

    def expect(self, needle, timeout=15.0, absent=None):
        end = time.time() + timeout
        while time.time() < end:
            if needle in clean(self.buf):
                if absent and absent in clean(self.buf):
                    raise AssertionError("unexpected %r in output:\n%s"
                                         % (absent, clean(self.buf)))
                return
            self.pump(0.25)
        raise AssertionError("timed out waiting for %r; got:\n%s"
                             % (needle, clean(self.buf)))

    def send(self, s):
        os.write(self.master, s.encode())

    def screen(self):
        s = Screen()
        s.feed(self.buf)
        return s.text()

    def expect_on_screen(self, needle, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.screen():
                return
            self.pump(0.25)
        raise AssertionError("timed out waiting for %r on screen; screen:\n%s"
                             % (needle, self.screen()))

    def expect_gone_from_screen(self, needle, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if needle not in self.screen():
                return
            self.pump(0.25)
        raise AssertionError("%r still on screen; screen:\n%s"
                             % (needle, self.screen()))

    def wait(self, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if self.proc.poll() is not None:
                break
            self.pump(0.2)
        else:
            self.proc.kill()
            raise AssertionError("child did not exit; output:\n%s" % clean(self.buf))
        self.pump(0.3)
        return self.proc.returncode

    def restored(self):
        """True if the tty is back to a cooked state (ECHO + ICANON on)."""
        after = termios.tcgetattr(self.slave)
        return bool(after[3] & termios.ECHO) and bool(after[3] & termios.ICANON)

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
        os.close(self.master)
        os.close(self.slave)


def base_env(home, extra=None):
    env = {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "HOME": home,
        "TERM": "xterm-256color",
        "LANG": "en_US.UTF-8",
    }
    if extra:
        env.update(extra)
    return env


def test_first_paint_is_lazy(home, ws):
    """No API key at all: the shell must still paint, proving connect() is
    deferred until the first prompt."""
    t = Term([TNY], base_env(home), ws)
    try:
        t.expect(BANNER, 10.0, absent="no API key")
        t.expect("/help for commands")
        assert "openai" in clean(t.buf), clean(t.buf)
        t.send("/quit\r")
        rc = t.wait()
        assert rc == 0, "exit %s\n%s" % (rc, clean(t.buf))
        assert t.restored(), "terminal left in raw mode"
    finally:
        t.close()
    print("ok  first paint without backend connect, /quit exits 0, tty restored")


def test_turn_streams(home, ws, port):
    env = base_env(home, {
        "OPENAI_BASE_URL": "http://127.0.0.1:%d/v1" % port,
        "OPENAI_API_KEY": "test-key-not-a-secret",
    })
    t = Term([TNY], env, ws)
    try:
        t.expect(BANNER)
        t.send("list the files here\r")
        t.expect("list_files", 20.0)       # tool one-liner from the mock turn 1
        t.expect("pondering", 20.0)        # reasoning summary rendered dim
        t.expect("MOCK-OK", 20.0)          # streamed answer from turn 2
        assert "✓" in t.buf, "no tool-ok marker:\n%s" % clean(t.buf)
        t.send("/quit\r")
        rc = t.wait()
        assert rc == 0, "exit %s\n%s" % (rc, clean(t.buf))
        assert t.restored(), "terminal left in raw mode"
    finally:
        t.close()
    # the turn must have produced a session on disk
    sess = os.path.join(home, ".tny", "sessions")
    assert os.path.isdir(sess), "no sessions dir"
    hist = os.path.join(home, ".tny", "history")
    assert os.path.isfile(hist), "history not written"
    assert "list the files here" in open(hist).read()
    print("ok  prompt streamed a full tool turn, session + history persisted")


def test_slash_palette(home, ws):
    # /provider's hint must list the providers detected here: builtins, a
    # settings.json profile, and a NAME_BASE_URL env provider.
    os.makedirs(os.path.join(home, ".tny"), exist_ok=True)
    with open(os.path.join(home, ".tny", "settings.json"), "w") as f:
        f.write('{"openrouter":{"base_url":"https://openrouter.test/v1"},'
                '"acp":{"agents":{"claude-code":'
                '{"command":["claude-agent-acp"]}}}}')
    t = Term([TNY], base_env(home, {"ORWELL_BASE_URL": "https://orwell.test/v1"}), ws)
    try:
        t.expect(BANNER)
        t.send("/")
        t.expect("clear the screen", 5.0)   # palette listed commands
        t.send("prov")
        # The hint is clipped to the terminal width; prove the settings ACP
        # profile is included before the env-only tail that may be off-screen.
        t.expect("openai|cursor|codex|acp|claude|grok|openrouter|acp:claude-code|", 5.0)
        t.send("\x7f" * 4)                 # back to a bare "/"
        t.send("help\r")
        t.expect("ctrl-o transcript", 5.0)
        t.send("/permissions auto\r")
        t.expect("permission mode: auto", 5.0)
        t.send("/quit\r")
        assert t.wait() == 0
    finally:
        t.close()
        os.remove(os.path.join(home, ".tny", "settings.json"))
    print("ok  slash palette filters, /help and /permissions work")


class ApprovalHandler(BaseHTTPRequestHandler):
    """Asks for a write_file (not a safe tool) so the y/a/n UI has to run.
    Speaks the default Responses API wire (docs/adr/0016); anything hitting
    /chat/completions is a 404-class failure the test will surface."""
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _chunk(self, data):
        self.wfile.write(("%x\r\n" % len(data)).encode() + data + b"\r\n")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n))
        assert self.path.endswith("/responses"), self.path
        answered = any(i.get("type") == "function_call_output"
                       for i in req["input"])
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        if not answered:
            args = json.dumps({"path": "note.txt", "content": "hi"})
            events = [
                {"type": "response.output_item.added", "output_index": 0,
                 "item": {"type": "function_call", "id": "fc_w",
                          "call_id": "call_w", "name": "write_file",
                          "arguments": args}},
                {"type": "response.completed",
                 "response": {"status": "completed"}},
            ]
        else:
            events = [{"type": "response.output_text.delta", "output_index": 0,
                       "delta": "DENIED-OK"},
                      {"type": "response.completed",
                       "response": {"status": "completed"}}]
        for e in events:
            self._chunk(("data: %s\n\n" % json.dumps(e)).encode())
        self._chunk(b"")


def test_approval_ui(home, ws):
    """Explicit --permission-mode ask still gets the y/a/n gate (opt-in since
    docs/adr/0001 made yolo the default)."""
    srv = HTTPServer(("127.0.0.1", 0), ApprovalHandler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    env = base_env(home, {
        "OPENAI_BASE_URL": "http://127.0.0.1:%d/v1" % srv.server_port,
        "OPENAI_API_KEY": "test-key-not-a-secret",
    })
    t = Term([TNY, "--permission-mode", "ask"], env, ws)
    try:
        t.expect(BANNER)
        t.send("write a note\r")
        t.expect("approve?", 20.0)
        t.expect("write_file", 5.0)
        t.send("n")
        t.expect("denied", 10.0)
        t.expect("stopped: permission denied", 20.0)
        assert not os.path.exists(os.path.join(ws, "note.txt")), "denied write happened"
        t.send("/quit\r")
        assert t.wait() == 0
        assert t.restored(), "terminal left in raw mode"
    finally:
        t.close()
        srv.shutdown()
    print("ok  approval prompt shown, 'n' denies and stops before another request")


def test_yolo_default_auto_approves(home, ws):
    """Out of the box tny runs yolo (docs/adr/0001): the sensitive tool runs
    with no prompt and no 'auto-approved' chatter in the transcript."""
    note = os.path.join(ws, "note.txt")
    if os.path.exists(note):
        os.unlink(note)
    srv = HTTPServer(("127.0.0.1", 0), ApprovalHandler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    env = base_env(home, {
        "OPENAI_BASE_URL": "http://127.0.0.1:%d/v1" % srv.server_port,
        "OPENAI_API_KEY": "test-key-not-a-secret",
    })
    t = Term([TNY], env, ws)
    try:
        t.expect(BANNER)
        assert "yolo" in clean(t.buf), "banner does not show yolo:\n%s" % clean(t.buf)
        t.send("write a note\r")
        # the mock's completion marker; "approve?" must never have appeared
        t.expect("DENIED-OK", 20.0, absent="approve?")
        assert "auto-approved" not in clean(t.buf), clean(t.buf)
        end = time.time() + 5
        while time.time() < end and not os.path.exists(note):
            time.sleep(0.1)
        assert os.path.exists(note), "yolo did not run the write_file tool"
        t.send("/quit\r")
        assert t.wait() == 0
    finally:
        t.close()
        srv.shutdown()
    print("ok  default mode is yolo: sensitive tool ran silently, no prompt")


def test_menu_overlay_transient(home, ws):
    """The /help menu is an overlay: esc dismisses it, any submit clears it,
    and nothing of it survives in the visible buffer (the report in
    docs/adr/0003)."""
    t = Term([TNY], base_env(home), ws)
    try:
        t.expect(BANNER)
        t.send("/help\r")
        t.expect_on_screen("(esc hides this menu)")
        t.expect_on_screen("keys: enter submit")

        t.send("\x1b")  # esc dismisses the menu...
        t.expect_gone_from_screen("(esc hides this menu)")
        assert "keys: enter submit" not in t.screen(), t.screen()
        assert BANNER in t.screen(), "transcript was wiped:\n%s" % t.screen()

        t.send("/help\r")  # ...and so does running the next command
        t.expect_on_screen("(esc hides this menu)")
        t.send("/sandbox\r")
        t.expect_gone_from_screen("(esc hides this menu)")
        t.expect_on_screen("sandbox:")
        scr = t.screen()
        assert "/workspace" not in scr, "menu rows left in the buffer:\n%s" % scr
        assert "keys: enter submit" not in scr, scr
        t.send("/quit\r")
        assert t.wait() == 0
        assert t.restored(), "terminal left in raw mode"
    finally:
        t.close()
    print("ok  /help menu is transient: esc and the next command both clear it")


COLOR_SGR = re.compile(r"\x1b\[(?:[0-9]+;)*[349][0-9]m")


def test_no_color_keeps_the_status_bar(home, ws):
    """The sandbox screenshot: a pty with NO_COLOR set must keep structural
    SGR — the status bar's reverse video, the banner's bold — while colors
    (the green composer prompt) disappear (docs/adr/0026)."""
    t = Term([TNY], base_env(home, {"NO_COLOR": "1"}), ws)
    try:
        t.expect(BANNER)
        t.expect("yolo")                       # the status row painted
        assert "\x1b[7m" in t.buf, "status bar lost reverse video:\n%r" % t.buf
        assert "\x1b[1m" in t.buf, "banner lost bold:\n%r" % t.buf
        assert not COLOR_SGR.search(t.buf), \
            "color SGR leaked under NO_COLOR:\n%r" % t.buf
        t.send("/quit\r")
        assert t.wait() == 0
        assert t.restored(), "terminal left in raw mode"
    finally:
        t.close()
    print("ok  NO_COLOR keeps the status bar's reverse video, drops colors")


def test_color_never_drops_all_sgr(home, ws):
    """--color=never: zero SGR; the status row falls back to ── delimiters
    so it never reads as ordinary transcript text (docs/adr/0026)."""
    t = Term([TNY, "--color=never"], base_env(home), ws)
    try:
        t.expect(BANNER)
        assert not re.search(r"\x1b\[[0-9;]*m", t.buf), \
            "SGR leaked under --color=never:\n%r" % t.buf
        t.expect_on_screen("── openai  default  yolo")
        t.send("/quit\r")
        assert t.wait() == 0
        assert not re.search(r"\x1b\[[0-9;]*m", t.buf), \
            "SGR leaked during --color=never shutdown:\n%r" % t.buf
    finally:
        t.close()
    print("ok  --color=never: no SGR, status row reads ── … ──")


def test_clicolor_force_beats_no_color(home, ws):
    """CLICOLOR_FORCE recovers full styling without touching NO_COLOR
    (docs/adr/0026)."""
    env = base_env(home, {"NO_COLOR": "1", "CLICOLOR_FORCE": "1"})
    t = Term([TNY], env, ws)
    try:
        t.expect(BANNER)
        assert "\x1b[32m" in t.buf, "composer green not forced on:\n%r" % t.buf
        assert "\x1b[7m" in t.buf, "status bar lost reverse video:\n%r" % t.buf
        t.send("/quit\r")
        assert t.wait() == 0
    finally:
        t.close()
    print("ok  CLICOLOR_FORCE beats NO_COLOR")


def test_dumb_mode_announces_itself(home, ws):
    """No pty: the shell says why the status bar is missing, stays plain,
    and exits 0 on stdin EOF. /dev/null polls back POLLNVAL on macOS —
    treating it as anything but EOF livelocks the loop (docs/adr/0026)."""
    proc = subprocess.run(
        [TNY], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, env=base_env(home), cwd=ws, timeout=15)
    out = proc.stdout.decode("utf-8", "replace")
    assert proc.returncode == 0, (proc.returncode, out)
    assert "not a terminal: status bar disabled" in out, out
    assert "\x1b" not in out, "escape leaked into a pipe:\n%r" % out
    print("ok  dumb mode announces the missing status bar, zero escapes")


def test_dumb_mode_turn_status(home, ws, port):
    """Dumb mode has no status row, so a turn leaves a plain status line in
    the transcript when it ends (docs/adr/0026)."""
    env = base_env(home, {
        "OPENAI_BASE_URL": "http://127.0.0.1:%d/v1" % port,
        "OPENAI_API_KEY": "test-key-not-a-secret",
    })
    proc = subprocess.Popen([TNY], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env, cwd=ws)
    try:
        proc.stdin.write(b"list the files here\n")
        proc.stdin.flush()
        out = b""
        end = time.time() + 20
        while time.time() < end and b" tok " not in out:
            r, _, _ = select.select([proc.stdout], [], [], 0.25)
            if r:
                chunk = os.read(proc.stdout.fileno(), 65536)
                if not chunk:
                    break
                out += chunk
        text = out.decode("utf-8", "replace")
        assert "not a terminal: status bar disabled" in text, text
        assert "MOCK-OK" in text, text          # the turn actually ran
        assert "── openai" in text and " tok ──" in text, text
        assert "\x1b" not in text, "escape leaked into a pipe:\n%r" % text
        proc.stdin.close()
        assert proc.wait(timeout=10) == 0
    finally:
        if proc.poll() is None:
            proc.kill()
    print("ok  dumb mode prints a plain status line when the turn ends")


def children_of(pid):
    out = subprocess.run(["ps", "-ax", "-o", "pid=,ppid=,command="],
                         capture_output=True, text=True).stdout
    kids = []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] == str(pid):
            kids.append((int(parts[0]), parts[2]))
    return kids


def test_prewarm_spawns_acp_agent(home, ws):
    """docs/adr/0002: with a host provider selected, the TUI spawns and
    initializes the host right after the first paint — before any prompt."""
    agent = os.path.join(HERE, "fake_acp_agent.py")
    settings = os.path.join(home, ".tny", "settings.json")
    os.makedirs(os.path.dirname(settings), exist_ok=True)
    previous = open(settings, "rb").read() if os.path.exists(settings) else None
    with open(settings, "w") as f:
        json.dump({"acp": {"agents": {"tui-fixture": {
            # Resolve the real interpreter: the pty env overrides HOME, which
            # breaks version-manager shims used by /usr/bin/env python3.
            "command": [sys.executable, agent], "model": "selected-model"
        }}}}, f)
    state = os.path.join(home, "acp-prewarm-state.json")
    t = Term([TNY, "--provider", "acp:tui-fixture"],
             base_env(home, {"FAKE_ACP_STATE": state}), ws)
    try:
        t.expect(BANNER)
        end = time.time() + 8
        spawned = []
        while time.time() < end:
            spawned = [c for c in children_of(t.proc.pid) if "fake_acp_agent" in c[1]]
            if spawned:
                break
            t.pump(0.2)
        assert spawned, ("agent not pre-warmed after startup; children: %r\n%s"
                        % (children_of(t.proc.pid), clean(t.buf)))
        # the warm host is adopted by the first turn, not respawned
        t.send("hello\r")
        t.expect("Hello from the fake ACP agent.", 20.0)
        assert json.load(open(state))["model_at_prompt"] == "selected-model"
        agents = [c for c in children_of(t.proc.pid) if "fake_acp_agent" in c[1]]
        assert len(agents) == 1, "prewarmed agent was not adopted: %r" % agents
        assert agents[0][0] == spawned[0][0], "agent was respawned for the turn"
        # /new drops the bound backend and re-warms: a fresh agent must be
        # up (with its session created) before the next prompt is typed
        first_pid = agents[0][0]
        t.send("/new\r")
        t.expect("new session")
        end = time.time() + 8
        fresh = []
        while time.time() < end:
            fresh = [c for c in children_of(t.proc.pid)
                     if "fake_acp_agent" in c[1] and c[0] != first_pid]
            if fresh:
                break
            t.pump(0.2)
        assert fresh, ("no re-warmed agent after /new; children: %r\n%s"
                       % (children_of(t.proc.pid), clean(t.buf)))
        t.send("/quit\r")
        assert t.wait() == 0
    finally:
        t.close()
        if previous is None:
            if os.path.exists(settings):
                os.remove(settings)
        else:
            with open(settings, "wb") as f:
                f.write(previous)
    print("ok  named acp profile selected its model, pre-warmed, adopted, and re-warmed")


def test_version_fast_path():
    out = subprocess.run([TNY, "--version"], capture_output=True, text=True, timeout=10)
    assert out.returncode == 0, out
    got = out.stdout.strip()
    # shape: no v prefix, one line, starts like a version or a bare hash
    assert got and "\n" not in got and not got.startswith("v"), out
    assert re.match(r"^[0-9a-zA-Z][0-9a-zA-Z.+-]*$", got), out
    # a build from this checkout must agree with git describe
    desc = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"],
                          capture_output=True, text=True, cwd=ROOT, timeout=10)
    if desc.returncode == 0 and desc.stdout.strip():
        # tolerate a dirty-flag flip between build and test run
        want = desc.stdout.strip().lstrip("v").replace("-dirty", "")
        assert got.replace("-dirty", "") == want, (got, want)
    # --help carries the same version, not a stale constant
    hlp = subprocess.run([TNY, "--help"], capture_output=True, text=True, timeout=10)
    assert f"tny v{got}" in hlp.stdout, hlp.stdout[:200]
    print("ok  --version fast path reports the build version (%s)" % got)



def test_provider_setup_wizard(home, ws, port):
    """/provider setup (docs/adr/0018): the composer Q&A writes a settings
    profile, switches to it, and the next turn runs on the new provider with
    the stored key — no OPENAI_* env at all."""
    wizhome = tempfile.mkdtemp(prefix="tny-wizhome-")
    t = Term([TNY], base_env(wizhome), ws)
    try:
        t.expect(BANNER)
        t.send("/provider setup wizprov\r")
        t.expect("base url", 10.0)
        t.send("http://127.0.0.1:%d/v1\r" % port)
        t.expect("api key", 10.0)
        t.send("sk-wiz-not-a-secret\r")
        t.expect("default model", 10.0)
        t.send("\r")
        t.expect("provider 'wizprov' ready", 10.0)
        settings = open(os.path.join(wizhome, ".tny", "settings.json")).read()
        assert '"wizprov"' in settings and '"sk-wiz-not-a-secret"' in settings, settings
        t.send("list the files here\r")
        t.expect("MOCK-OK", 20.0)
        # /cancel aborts a wizard without touching settings
        t.send("/provider setup droppedprov\r")
        t.expect("base url", 10.0)
        t.send("/cancel\r")
        t.expect("cancelled", 10.0)
        assert "droppedprov" not in open(
            os.path.join(wizhome, ".tny", "settings.json")).read()
        t.send("/quit\r")
        rc = t.wait()
        assert rc == 0, "exit %s\n%s" % (rc, clean(t.buf))
        print("ok  /provider setup wizard: profile written, turn ran, "
              "/cancel left settings alone")
    finally:
        t.close()
        shutil.rmtree(wizhome, ignore_errors=True)


def test_steer_mid_turn(home, ws):
    """Enter during a native-loop turn steers (docs/adr/0011): the text lands
    as a user message after the tool result, the transcript shows it with
    the `steer` tag, and nothing about "a turn is already running" is
    printed."""
    port = free_port()
    mock = subprocess.Popen(
        [sys.executable, MOCK, str(port)],
        env=dict(os.environ, MOCK_SLOW_MS="1500", MOCK_EXPECT_STEER="also count them"),
        stdout=subprocess.PIPE, text=True)
    try:
        assert "ready" in mock.stdout.readline()
        env = base_env(home, {
            "OPENAI_BASE_URL": "http://127.0.0.1:%d/v1" % port,
            "OPENAI_API_KEY": "test-key-not-a-secret",
        })
        t = Term([TNY, "--provider", "openai"], env, ws)
        try:
            t.expect(BANNER)
            t.send("list the files here\r")
            t.expect("working", 5.0)            # the turn is live (mock is slow)
            t.send("also count them\r")
            t.expect("steer", 5.0)              # echoed with the steer tag
            t.expect("STEER-OK", 20.0)          # mock saw it as the last user msg
            assert "already running" not in clean(t.buf), clean(t.buf)
            t.send("/quit\r")
            assert t.wait() == 0
        finally:
            t.close()
    finally:
        mock.terminate()
        mock.wait(timeout=5)
    print("ok  enter mid-turn steers the native loop after the tool result")


def test_queue_sends_after_turn(home, ws):
    """A backend without steer (ACP) queues: the second message shows in the
    queue row, not the transcript, and is sent once the first turn ends. Esc
    during a turn drops whatever is queued."""
    agent = os.path.join(HERE, "fake_acp_agent.py")
    env = base_env(home, {"FAKE_ACP_SLOW_MS": "1500",
                          "FAKE_ACP_STATE": os.path.join(home, "acp-state.json")})
    t = Term([TNY, "--provider", "acp", "--agent", sys.executable, "--", agent],
             env, ws)
    try:
        t.expect(BANNER)
        t.send("first question\r")
        t.expect("working", 5.0)
        t.send("second question\r")
        t.expect("queued (1): second question", 5.0)
        assert "already running" not in clean(t.buf), clean(t.buf)
        t.expect("[asked: first question]", 20.0)
        t.expect("[asked: second question]", 20.0)   # sent after turn 1 ended
        t.expect("ALLOWED.", 20.0)
        # esc while a turn runs drops the queue
        t.send("third question\r")
        t.expect("working", 5.0)
        t.send("fourth question\r")
        t.expect("queued (1): fourth question", 5.0)
        t.send("\x1b")
        t.expect("dropped 1 queued message", 10.0)
        t.expect("DENIED.", 20.0)   # the fake agent finishes turn 3 cancelled
        time.sleep(1.0)             # long enough for a wrongly-sent turn 4 to echo
        assert "[asked: fourth question]" not in clean(t.buf), clean(t.buf)
        t.send("/quit\r")
        assert t.wait() == 0
    finally:
        t.close()
    print("ok  queued message sent after the turn; esc drops the queue")


def test_clipboard_image_pastes_path(home, ws):
    """Ctrl-V materializes clipboard pixels but submits only their path.

    ACP deliberately advertises no image prompt capability, so this also
    proves the paste never enters tny's provider-specific image channel and
    cannot poison later sends (including after /clear).
    """
    agent = os.path.join(HERE, "fake_acp_agent.py")
    helpers = tempfile.mkdtemp(prefix="tny-clipboard-")
    helper_body = (
        "#!%s\n" % sys.executable +
        "import os, sys\n"
        "data = b'\\x89PNG\\r\\n\\x1a\\n' + b'\\x00' * 8\n"
        "if os.path.basename(sys.argv[0]) == 'pngpaste':\n"
        "    open(sys.argv[1], 'wb').write(data)\n"
        "else:\n"
        "    sys.stdout.buffer.write(data)\n"
    )
    for name in ("pngpaste", "wl-paste", "xclip"):
        helper = os.path.join(helpers, name)
        with open(helper, "w") as f:
            f.write(helper_body)
        os.chmod(helper, 0o755)

    env = base_env(home, {"PATH": helpers + os.pathsep +
                          os.environ.get("PATH", "/usr/bin:/bin")})
    t = Term([TNY, "--provider", "acp", "--agent", sys.executable, "--", agent],
             env, ws)
    pasted = None
    try:
        t.expect(BANNER)
        t.send("\x16")                  # Ctrl-V
        prefix = "tny-paste-%d-" % t.proc.pid
        end = time.time() + 10
        while time.time() < end:
            matches = [name for name in os.listdir("/tmp")
                       if name.startswith(prefix) and name.endswith(".png")]
            if matches:
                pasted = os.path.join("/tmp", matches[0])
                break
            t.pump(0.1)
        assert pasted, "clipboard helper did not materialize an image"
        t.expect_on_screen(pasted)
        assert "[Image #" not in t.screen(), t.screen()

        t.send("\r")
        t.expect("[asked: `%s`]" % pasted, 20.0,
                 absent="image prompts are not supported")
        t.expect("Hello from the fake ACP agent.", 20.0)

        t.send("/clear\r")
        t.send("after clear\r")
        t.expect("[asked: after clear]", 20.0,
                 absent="image prompts are not supported")
        t.send("/quit\r")
        assert t.wait() == 0
    finally:
        t.close()
        if pasted and os.path.exists(pasted):
            os.unlink(pasted)
        shutil.rmtree(helpers, ignore_errors=True)
    print("ok  ctrl-v image pasted a path; image-less ACP and post-clear send work")


def test_codex_steer_mid_turn(home, ws):
    """codex: Enter during a turn rides turn/steer with the active turn id
    (docs/adr/0011); the mock validates the request and echoes STEER-OK."""
    mock_ws = os.path.join(HERE, "mock_codex_ws.py")
    mock = subprocess.Popen(
        [sys.executable, mock_ws, "0"],
        env=dict(os.environ, MOCK_CONNECTIONS="1", MOCK_BUSY_CONN="0",
                 MOCK_STEER_WAIT_MS="2500"),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        line = mock.stdout.readline()
        assert "ready" in line, line
        port = int(line.split()[-1])
        t = Term([TNY, "--provider", "codex", "--codex-ws",
                  "ws://127.0.0.1:%d" % port], base_env(home), ws)
        try:
            t.expect(BANNER)
            t.send("hello codex\r")
            t.expect("thinking", 20.0)            # turn/start accepted, streaming
            t.send("and steer this\r")
            t.expect("steer", 5.0)                # transcript tag
            t.expect("STEER-OK:and steer this", 20.0)
            assert "already running" not in clean(t.buf), clean(t.buf)
            t.expect("ls -la", 20.0)              # the turn's tool item: turn ending
            time.sleep(1.0)
            t.send("/quit\r")
            assert t.wait() == 0
        finally:
            t.close()
        mock.wait(timeout=10)
        err = mock.stderr.read()
        assert mock.returncode == 0, "mock reported protocol failures:\n%s" % err
        assert "turn/steer ok" in err, err
    finally:
        if mock.poll() is None:
            mock.terminate()
            mock.wait(timeout=5)
    print("ok  codex: enter mid-turn rides turn/steer with the active turn id")


def test_codex_steer_rejected_requeues(home, ws, mode):
    """codex: a steer the host refuses must come back as the next turn's
    prompt, never be lost (docs/adr/0013). mode="now" is a plain JSON-RPC
    error mid-turn; mode="late" delivers the error only after turn/completed
    — the ordering race where the old TUI-side bookkeeping dropped the
    text."""
    mock_ws = os.path.join(HERE, "mock_codex_ws.py")
    mock = subprocess.Popen(
        [sys.executable, mock_ws, "0"],
        env=dict(os.environ, MOCK_CONNECTIONS="1", MOCK_BUSY_CONN="0",
                 MOCK_STEER_WAIT_MS="2500", MOCK_STEER_REJECT=mode,
                 MOCK_EXPECT_RESEND="please requeue me"),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        line = mock.stdout.readline()
        assert "ready" in line, line
        port = int(line.split()[-1])
        t = Term([TNY, "--provider", "codex", "--codex-ws",
                  "ws://127.0.0.1:%d" % port], base_env(home), ws)
        try:
            t.expect(BANNER)
            t.send("hello codex\r")
            t.expect("thinking", 20.0)          # turn 1 accepted, streaming
            t.send("please requeue me\r")
            t.expect("steer", 5.0)              # sent as turn/steer first
            t.expect("TURN1-DONE", 20.0)
            # the rejected text is re-queued and submitted as turn 2
            t.expect("TURN2-DONE", 20.0)
            assert "already running" not in clean(t.buf), clean(t.buf)
            t.send("/quit\r")
            assert t.wait() == 0
        finally:
            t.close()
        mock.wait(timeout=10)
        err = mock.stderr.read()
        assert mock.returncode == 0, "mock reported protocol failures:\n%s" % err
        assert "turn/start ok (prompt='please requeue me')" in err, err
    finally:
        if mock.poll() is None:
            mock.terminate()
            mock.wait(timeout=5)
    print("ok  codex: %s steer rejection re-queued the text as the next turn" % mode)


def main():
    if not os.access(TNY, os.X_OK):
        print("build first: make BUILD=build-tui release", file=sys.stderr)
        return 1
    port = free_port()
    mock = subprocess.Popen([sys.executable, MOCK, str(port)],
                            stdout=subprocess.PIPE, text=True)
    try:
        line = mock.stdout.readline()
        assert "ready" in line, line
        home = tempfile.mkdtemp(prefix="tny-home-")
        ws = tempfile.mkdtemp(prefix="tny-ws-")
        open(os.path.join(ws, "a.txt"), "w").write("alpha\n")
        open(os.path.join(ws, "b.txt"), "w").write("beta\n")
        try:
            test_version_fast_path()
            test_first_paint_is_lazy(home, ws)
            test_turn_streams(home, ws, port)
            test_slash_palette(home, ws)
            test_approval_ui(home, ws)
            test_yolo_default_auto_approves(home, ws)
            test_menu_overlay_transient(home, ws)
            test_no_color_keeps_the_status_bar(home, ws)
            test_color_never_drops_all_sgr(home, ws)
            test_clicolor_force_beats_no_color(home, ws)
            test_dumb_mode_announces_itself(home, ws)
            test_dumb_mode_turn_status(home, ws, port)
            test_prewarm_spawns_acp_agent(home, ws)
            test_provider_setup_wizard(home, ws, port)
            test_steer_mid_turn(home, ws)
            test_queue_sends_after_turn(home, ws)
            test_clipboard_image_pastes_path(home, ws)
            test_codex_steer_mid_turn(home, ws)
            test_codex_steer_rejected_requeues(home, ws, "now")
            test_codex_steer_rejected_requeues(home, ws, "late")
        finally:
            shutil.rmtree(home, ignore_errors=True)
            shutil.rmtree(ws, ignore_errors=True)
    finally:
        mock.terminate()
        mock.wait(timeout=5)
    print("\nall tui integration tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
