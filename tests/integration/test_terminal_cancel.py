#!/usr/bin/env python3
"""Interrupting a synchronous terminal tool under `tny ask --events=jsonl`.

The in-process event path runs tools inside the engine dispatch, so a turn
that is waiting on a long `terminal` command must still see the interrupt,
stop the command and everything it started, and report an interrupted turn.
Each case drives a real turn against a deterministic loopback provider under
a throwaway HOME with fake credentials. Stdlib only; no live provider, no
network beyond 127.0.0.1.
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TNY = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", "build/tny")
TNY = os.path.abspath(TNY)
# The interrupt must land well inside these; a case that needs them has failed.
SIGNAL_DEADLINE = float(os.environ.get("TNY_TEST_CANCEL_DEADLINE", "20"))
CHILD_DEADLINE = 10.0


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(str(msg))


# ---------------------------------------------------------------- fixture


def sse(obj):
    return f"data: {json.dumps(obj)}\n\n".encode()


class Handler(BaseHTTPRequestHandler):
    """One terminal call per turn; the command comes from the case."""

    protocol_version = "HTTP/1.1"
    command = "true"

    def log_message(self, *_args):
        pass

    def _chunk(self, data):
        self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
        self.wfile.flush()

    def _stream(self, frames):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for frame in frames:
            self._chunk(sse(frame))
        self._chunk(b"data: [DONE]\n\n")
        self._chunk(b"")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length) or b"{}")
        messages = body.get("messages") or []
        answered_tool = any(m.get("role") == "tool" for m in messages)
        type(self).seen_requests += 1
        if answered_tool:
            # A cancelled turn must not come back for more; if it does, the
            # case sees the extra request count.
            self._stream(
                [
                    {"choices": [{"index": 0, "delta": {"content": "done"}}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]},
                ]
            )
            return
        self._stream(
            [
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "role": "assistant",
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "id": "call_1",
                                        "type": "function",
                                        "function": {
                                            "name": "terminal",
                                            "arguments": json.dumps(
                                                {"command": type(self).command}
                                            ),
                                        },
                                    }
                                ],
                            },
                        }
                    ]
                },
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]},
            ]
        )


class QuietServer(ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        kind = sys.exc_info()[0]
        if kind is not None and issubclass(
            kind, (BrokenPipeError, ConnectionResetError)
        ):
            return
        super().handle_error(request, client_address)


def start_fixture():
    Handler.seen_requests = 0
    server = QuietServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


# ---------------------------------------------------------------- harness


def workspace_env(tmp, port):
    home = os.path.join(tmp, "home")
    workspace = os.path.join(tmp, "ws")
    os.makedirs(home, exist_ok=True)
    os.makedirs(workspace, exist_ok=True)
    env = dict(os.environ)
    for key in list(env):
        if key.endswith("_API_KEY") or key.endswith("_BASE_URL"):
            env.pop(key)
    env.pop("TNY_TOOLS", None)
    env.update(
        {
            "HOME": home,
            "OPENAI_API_KEY": "integration-test-not-real",
            "OPENAI_BASE_URL": f"http://127.0.0.1:{port}/v1",
            "OPENAI_WIRE_API": "chat",
            "OPENAI_DEFAULT_MODEL": "mock-model",
            "TNY_TEST_SUITE": "terminal-cancel",
        }
    )
    return env, workspace


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def read_pid(path):
    try:
        with open(path, encoding="utf-8") as handle:
            return int(handle.read().strip())
    except (OSError, ValueError):
        return 0


def wait_for_pids(paths, deadline_s):
    """The command reports the processes it owns; wait for all of them."""
    deadline = time.monotonic() + deadline_s
    pids = {}
    while time.monotonic() < deadline:
        pids = {name: read_pid(path) for name, path in paths.items()}
        if all(pids.values()):
            return pids
        time.sleep(0.05)
    return pids


def sweep(pids):
    """Only ever called after a case has reached its verdict: a run that
    caught tny before it cleaned up must not leave the command behind."""
    for pid in pids:
        if pid and alive(pid):
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass


def parse_stream(stdout: bytes):
    text = stdout.decode("utf-8")
    check(text == "" or text.endswith("\n"), f"stream cut mid-line: {text[-120:]!r}")
    events = []
    for line in text.splitlines():
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError as exc:
            raise Fail(f"line is not JSON: {line!r}: {exc}") from None
    return events


def run_cancelled_turn(env, workspace, label, command, pid_paths, prompt):
    """Start a turn whose terminal command is `command`, interrupt it once the
    command has reported its own processes, and collect the outcome."""
    Handler.command = command
    sentinel = subprocess.Popen(
        [sys.executable, "-c", "import time; time.sleep(300)"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    proc = subprocess.Popen(
        [TNY, "--provider", "openai", "ask", "--ephemeral", "--events=jsonl", prompt],
        cwd=workspace,
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    chunks = []
    reader = threading.Thread(
        target=lambda: chunks.append(proc.stdout.read()), daemon=True
    )
    reader.start()
    try:
        pids = wait_for_pids(pid_paths, SIGNAL_DEADLINE)
        check(
            all(pids.values()), f"the {label} command never reported its pids: {pids}"
        )
        check(proc.poll() is None, "the turn ended before the interrupt")
        started = time.monotonic()
        os.kill(proc.pid, signal.SIGINT)
        while proc.poll() is None and time.monotonic() - started < SIGNAL_DEADLINE:
            time.sleep(0.05)
        elapsed = time.monotonic() - started
        check(
            proc.poll() is not None,
            f"the interrupt never reached the running terminal tool "
            f"({label}: {elapsed:.1f}s)",
        )
        stderr = proc.stderr.read()
        reader.join(timeout=10)
        proc.wait(timeout=10)
        return proc.returncode, b"".join(chunks), stderr, pids, sentinel, elapsed
    except BaseException:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=10)
        sentinel.kill()
        sentinel.wait(timeout=10)
        sweep(read_pid(path) for path in pid_paths.values())
        raise


def assert_cancelled_turn(code, stdout, pids, sentinel, elapsed):
    try:
        check(code == 130, f"interrupted turn exited {code}, not 130")
        check(elapsed < SIGNAL_DEADLINE, f"cancellation took {elapsed:.1f}s")
        events = parse_stream(stdout)
        terminals = [e for e in events if e.get("type") == "turn_end"]
        check(len(terminals) == 1, [e.get("type") for e in events])
        # TNY_STOP_INTERRUPTED: one consumed signal still ends the turn.
        check(terminals[0]["stop_reason"] == 1, terminals[0])
        ends = [e for e in events if e.get("type") == "tool_end"]
        check(len(ends) == 1, [e.get("type") for e in events])
        detail = ends[0]["tool_detail"]
        check("cancelled" in detail, f"no cancellation in the result: {detail!r}")
        check("130" in detail, f"the result hid the interrupted status: {detail!r}")
        deadline = time.monotonic() + CHILD_DEADLINE
        for name, pid in pids.items():
            while alive(pid) and time.monotonic() < deadline:
                time.sleep(0.05)
            check(not alive(pid), f"owned {name} process {pid} survived the interrupt")
        check(sentinel.poll() is None, "an unrelated process was killed")
    finally:
        sentinel.kill()
        sentinel.wait(timeout=10)
        sweep(pids.values())


def case_interrupt_while_the_command_streams(env, workspace):
    """Output is still arriving: the drain must consult the cancellation
    probe, not run to the command's own completion."""
    main = os.path.join(workspace, "stream-main.pid")
    child = os.path.join(workspace, "stream-child.pid")
    daemon = os.path.join(workspace, "stream-daemon.pid")
    command = (
        f'echo $$ > "{main}"; '
        f'sleep 300 & echo $! > "{child}"; '
        f'{sys.executable} -c "import os,sys,time;os.setsid();'
        f"open(sys.argv[1],'w').write(str(os.getpid()));time.sleep(300)\" "
        f'"{daemon}" & '
        "while :; do echo tick; sleep 0.2; done"
    )
    outcome = run_cancelled_turn(
        env,
        workspace,
        "streaming",
        command,
        {"main": main, "child": child, "daemon": daemon},
        "run the long command",
    )
    code, stdout, _stderr, pids, sentinel, elapsed = outcome
    assert_cancelled_turn(code, stdout, pids, sentinel, elapsed)


def case_interrupt_after_the_command_closes_stdout(env, workspace):
    """The pipe reaches real EOF while the command is still alive: every
    process holding the write end closes it, so the tool is left waiting on a
    live child with nothing to read. That wait must stay interruptible
    instead of blocking in waitpid until the command decides to exit."""
    main = os.path.join(workspace, "quiet-main.pid")
    child = os.path.join(workspace, "quiet-child.pid")
    # `exec sleep` on both sides, so each recorded pid *is* the long-lived
    # process rather than a shell in front of one.
    command = (
        f'echo $$ > "{main}"; '
        f'( exec >&- 2>&-; exec sleep 300 ) & echo $! > "{child}"; '
        "echo going-quiet; exec >&- 2>&-; exec sleep 300"
    )
    outcome = run_cancelled_turn(
        env,
        workspace,
        "quiet",
        command,
        {"main": main, "child": child},
        "run the quiet command",
    )
    code, stdout, _stderr, pids, sentinel, elapsed = outcome
    assert_cancelled_turn(code, stdout, pids, sentinel, elapsed)


# ---------------------------------------------------------------- driver


def main():
    if not os.access(TNY, os.X_OK):
        print(f"test_terminal_cancel: {TNY} is not executable", file=sys.stderr)
        return 1
    if os.environ.get("TNY_TEST_EXPECT_WASM") == "1":
        print("skip (wasm): no fork, no owned children, no POSIX signals")
        return 0
    with tempfile.TemporaryDirectory(prefix="tny-terminal-cancel-") as tmp:
        server, thread = start_fixture()
        try:
            env, workspace = workspace_env(tmp, server.server_address[1])
            case_interrupt_while_the_command_streams(env, workspace)
            case_interrupt_after_the_command_closes_stdout(env, workspace)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
    print("ok  ask --events=jsonl: a running terminal tool yields to SIGINT")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (Fail, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1) from None
