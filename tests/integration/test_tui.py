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

ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]|\x1b[()][B0]|\r")


def clean(s):
    return ANSI.sub("", s)


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
        t.expect("tny 0.1.0", 10.0, absent="no API key")
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
        t.expect("tny 0.1.0")
        t.send("list the files here\r")
        t.expect("list_files", 20.0)       # tool one-liner from the mock turn 1
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
    t = Term([TNY], base_env(home), ws)
    try:
        t.expect("tny 0.1.0")
        t.send("/")
        t.expect("clear the screen", 5.0)   # palette listed commands
        t.send("help\r")
        t.expect("ctrl-o transcript", 5.0)
        t.send("/permissions auto\r")
        t.expect("permission mode: auto", 5.0)
        t.send("/quit\r")
        assert t.wait() == 0
    finally:
        t.close()
    print("ok  slash palette filters, /help and /permissions work")


class ApprovalHandler(BaseHTTPRequestHandler):
    """Asks for a write_file (not a safe tool) so the y/a/n UI has to run."""
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _chunk(self, data):
        self.wfile.write(("%x\r\n" % len(data)).encode() + data + b"\r\n")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n))
        answered = any(m.get("role") == "tool" for m in req["messages"])
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        if not answered:
            args = json.dumps({"path": "note.txt", "content": "hi"})
            frames = [
                {"choices": [{"index": 0, "delta": {"role": "assistant", "tool_calls": [
                    {"index": 0, "id": "call_w", "type": "function",
                     "function": {"name": "write_file", "arguments": args}}]}}]},
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]},
            ]
        else:
            frames = [{"choices": [{"index": 0, "delta": {"content": "DENIED-OK"}}]},
                      {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}]
        for f in frames:
            self._chunk(("data: %s\n\n" % json.dumps(f)).encode())
        self._chunk(b"data: [DONE]\n\n")
        self._chunk(b"")


def test_approval_ui(home, ws):
    srv = HTTPServer(("127.0.0.1", 0), ApprovalHandler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    env = base_env(home, {
        "OPENAI_BASE_URL": "http://127.0.0.1:%d/v1" % srv.server_port,
        "OPENAI_API_KEY": "test-key-not-a-secret",
    })
    t = Term([TNY], env, ws)
    try:
        t.expect("tny 0.1.0")
        t.send("write a note\r")
        t.expect("approve?", 20.0)
        t.expect("write_file", 5.0)
        t.send("n")
        t.expect("denied", 10.0)
        t.expect("DENIED-OK", 20.0)
        assert not os.path.exists(os.path.join(ws, "note.txt")), "denied write happened"
        t.send("/quit\r")
        assert t.wait() == 0
        assert t.restored(), "terminal left in raw mode"
    finally:
        t.close()
        srv.shutdown()
    print("ok  approval prompt shown, 'n' denies and the turn continues")


def test_version_fast_path():
    out = subprocess.run([TNY, "--version"], capture_output=True, text=True, timeout=10)
    assert out.returncode == 0 and out.stdout.strip() == "0.1.0", out
    print("ok  --version fast path untouched")


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
