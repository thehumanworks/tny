#!/usr/bin/env python3
"""Real PTY + saturated HTTP stream reproductions for session cancellation.

No credentials or external services: the server never finishes its response.
Checks stored status AND connection/lock release, never just UI text. SIGSTOP
models a runner unable to process signals or socket cancellation requests.
"""

import fcntl
import json
import os
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from test_tui import BANNER, TNY, Term, base_env, clean

TNY = os.path.abspath(TNY)  # fixtures run the child from an isolated workspace


class Stream:
    def __init__(self):
        self.flood = threading.Event()
        self.closed = threading.Event()
        self.stopping = threading.Event()
        self.bytes_sent = 0
        stream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                assert body["stream"] is True
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Connection", "close")
                self.end_headers()
                wire_chat = self.path.endswith("chat/completions")

                def event(text):
                    value = (
                        {"choices": [{"index": 0, "delta": {"content": text}}]}
                        if wire_chat
                        else {"type": "response.output_text.delta", "delta": text}
                    )
                    return ("data: " + json.dumps(value) + "\n\n").encode()

                try:
                    self.wfile.write(event("INTERRUPT-READY\n"))
                    # Synchronize on the first rendered token, then saturate
                    # the socket without any token pacing or final SSE event.
                    while not stream.stopping.is_set():
                        if stream.flood.wait(0.01):
                            chunk = event("token " * 8 + "\n") * 1024
                            self.wfile.write(chunk)
                            stream.bytes_sent += len(chunk)
                        else:
                            self.wfile.write(b": waiting\n\n")
                except OSError:
                    pass
                finally:
                    stream.closed.set()
                    self.close_connection = True

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.stopping.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def lock_free(sdir):
    with (sdir / "lock").open() as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return False
        return True


def wait_for(predicate, term, seconds, label):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        if term:
            term.pump(0.02)
            # A flood should not make the test's output recorder unbounded.
            term.buf = term.buf[-65536:]
        else:
            time.sleep(0.02)
    raise AssertionError(f"{label} did not complete within {seconds}s")


def run_case(
    action,
    *,
    isolated=True,
    frozen=False,
    wire="chat",
    cli=False,
    draft=False,
    slow_lock=False,
):
    if slow_lock and sys.platform != "darwin":
        print("skip: slow-lock deadline injection uses the Darwin loader")
        return
    with tempfile.TemporaryDirectory(prefix="tny-interrupt-") as home:
        ws = Path(home) / "ws"
        ws.mkdir()
        stream = Stream()
        env = base_env(
            home,
            {
                "OPENAI_API_KEY": "test-key-not-real",
                "OPENAI_BASE_URL": f"http://127.0.0.1:{stream.server.server_port}/v1",
                "TNY_OPENAI_WIRE": wire,
                "TNY_ISOLATE": "1" if isolated else "0",
            },
        )
        if slow_lock:
            fixture = Path(__file__).resolve().parents[1] / "fixtures/slow_lock.c"
            library = Path(home) / "slow-lock.dylib"
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CC", "cc")),
                    "-std=c11",
                    "-D_DARWIN_C_SOURCE",
                    "-dynamiclib",
                    str(fixture),
                    "-o",
                    str(library),
                ],
                check=True,
                capture_output=True,
                timeout=30,
            )
            env["DYLD_INSERT_LIBRARIES"] = str(library)
        term = None
        terminal_closed = False
        pid = None
        try:
            term = Term([TNY, "ask", "interrupt test"] if cli else [TNY], env, str(ws))
            if not cli:
                term.expect(BANNER)
                term.send("interrupt test\r")
            term.expect("INTERRUPT-READY")
            sdir = next((Path(home) / ".tny/sessions").glob("*/*"))
            if isolated:
                pid = int((sdir / "pid").read_text())
                assert not lock_free(sdir), "runner must hold its writer lock"
            if frozen:
                assert pid
                os.kill(pid, signal.SIGSTOP)
            else:
                stream.flood.set()
                wait_for(
                    lambda: stream.bytes_sent >= 1024 * 1024, term, 5, "stream flood"
                )

            if draft:
                term.send("/sta")  # an open command palette must not swallow Ctrl-C

            started = time.monotonic()
            if action == "ctrl-c":
                if cli:
                    term.proc.send_signal(signal.SIGINT)
                else:
                    term.send("\x03")
            elif action == "double-ctrl-c":
                if cli:
                    term.proc.send_signal(signal.SIGINT)
                else:
                    term.send("\x03")
                term.expect("cancelling", timeout=2)
                if cli:
                    term.proc.send_signal(signal.SIGINT)
                else:
                    term.send("\x03")
            elif action == "ctrl-d":
                term.send("\x04")
            elif action == "hangup":
                term.proc.send_signal(signal.SIGHUP)
            elif action == "terminal-close":
                os.close(term.master)
                os.close(term.slave)
                terminal_closed = True
            elif action == "session-kill":
                stopped = subprocess.run(
                    [TNY, "--cwd", str(ws), "session", "stop", sdir.name, "--kill"],
                    env=env,
                    capture_output=True,
                    timeout=10,
                )
                assert stopped.returncode == 0, stopped.stderr.decode()
            else:
                raise AssertionError(action)

            deadline = (
                8
                if action in ("ctrl-d", "hangup", "terminal-close", "session-kill")
                or action == "ctrl-c"
                and frozen
                else 3
            )

            def stopped():
                if not isolated:
                    return "interrupted" in clean(term.buf)
                doc = json.loads((sdir / "session.json").read_text())
                # Graceful TUI cancellation keeps the idle runner warm;
                # every forced stop or exit must release its writer lock.
                warm = not frozen and not cli and action == "ctrl-c"
                return doc.get("status") == "interrupted" and (warm or lock_free(sdir))

            reader = None if terminal_closed else term
            wait_for(stopped, reader, deadline, "session interruption")
            wait_for(stream.closed.is_set, reader, 3, "provider connection close")
            elapsed = time.monotonic() - started
            doc = json.loads((sdir / "session.json").read_text())
            if frozen:
                assert doc["exit_code"] == 137, doc.get("exit_code")
                assert lock_free(sdir), "forced stop left a live runner"
            if terminal_closed:
                assert term.proc.wait(timeout=3) in (0, 129)
            elif cli or action in ("ctrl-d", "hangup"):
                rc = term.wait()
                assert rc == (129 if action == "hangup" else 130 if cli else 0), (
                    rc,
                    doc.get("exit_code"),
                    clean(term.buf)[-2000:],
                )
                assert term.restored(), "terminal left in raw mode"
            else:
                term.send("\x15/quit\r")  # clear any preserved draft before quitting
                assert term.wait() == 0
            print(
                f"ok: {action}, {'frozen' if frozen else 'saturated'} runner, "
                f"{'CLI' if cli else 'TUI'}, {wire}, isolate={isolated}: "
                f"interrupted and disconnected in {elapsed:.3f}s",
                flush=True,
            )
        finally:
            if pid:
                try:
                    os.killpg(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            if term:
                if terminal_closed:
                    if term.proc.poll() is None:
                        term.proc.kill()
                else:
                    term.close()
                term.proc.wait(timeout=5)
            stream.close()


def main():
    if os.environ.get("TNY_TEST_EXPECT_WASM") == "1":
        print("test_interrupt: native PTY/process tests; wasm has no runner")
        return
    cases = [
        ("flood-ctrl-c", "ctrl-c", {}),
        ("flood-ctrl-d", "ctrl-d", {}),
        ("responses-ctrl-c", "ctrl-c", {"wire": "responses"}),
        ("in-process-ctrl-c", "ctrl-c", {"isolated": False}),
        ("palette-ctrl-c", "ctrl-c", {"draft": True}),
        ("draft-ctrl-d", "ctrl-d", {"draft": True}),
        ("frozen-double-ctrl-c", "double-ctrl-c", {"frozen": True}),
        ("frozen-timeout", "ctrl-c", {"frozen": True}),
        ("frozen-ctrl-d", "ctrl-d", {"frozen": True}),
        ("frozen-slow-lock", "ctrl-d", {"frozen": True, "slow_lock": True}),
        ("frozen-hangup", "hangup", {"frozen": True}),
        ("frozen-terminal-close", "terminal-close", {"frozen": True}),
        ("cli-double-ctrl-c", "double-ctrl-c", {"frozen": True, "cli": True}),
        ("cli-flood-ctrl-c", "ctrl-c", {"cli": True}),
        ("cli-frozen-hangup", "hangup", {"frozen": True, "cli": True}),
        ("external-kill", "session-kill", {"frozen": True}),
    ]
    for name, action, options in cases:
        if os.environ.get("TNY_INTERRUPT_CASE", name) == name:
            run_case(action, **options)


if __name__ == "__main__":
    main()
