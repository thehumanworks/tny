#!/usr/bin/env python3
"""`tny ask --events=jsonl`: the canonical foreground event stream (ADR 0090).

Every case drives a real turn against a deterministic loopback provider under
a throwaway HOME with fake credentials. Stdlib only; no live provider, no
network beyond 127.0.0.1.
"""

from __future__ import annotations

import fcntl
import json
import os
import re
import selectors
import signal
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TNY = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", "build/tny")
TNY = os.path.abspath(TNY)
RUN_TIMEOUT = float(os.environ.get("TNY_TEST_TURN_TIMEOUT", "60"))

# One canonical envelope key set (sdk/schema/events.json) plus the numeric
# kind the public event view reports.
ENVELOPE = [
    "schema_version",
    "sequence",
    "timestamp_ms",
    "provider",
    "session_id",
    "turn_id",
    "type",
    "kind",
]
PAYLOAD = {
    "text_delta": ["text", "message_id"],
    "thinking": ["text", "message_id"],
    "tool_start": ["tool_name", "tool_id", "tool_detail"],
    "tool_end": ["tool_name", "tool_id", "tool_detail", "tool_ok"],
    "permission_request": [
        "permission_id",
        "permission_summary",
        "permission_options",
    ],
    "plan": ["text", "message_id"],
    "usage": [
        "input_tokens",
        "output_tokens",
        "context_used",
        "context_size",
        "cost",
        "has_cost",
    ],
    "turn_end": ["stop_reason"],
    "error": ["text", "error_code"],
    "status": ["text", "message_id"],
    "steer_rejected": ["text", "message_id"],
    "custom_message": ["text", "message_id", "message_type"],
    "user_message": ["text", "message_id"],
    "tool_progress": ["tool_name", "tool_id", "tool_detail"],
}
KIND_OF = {
    "text_delta": 0,
    "thinking": 1,
    "tool_start": 2,
    "tool_end": 3,
    "permission_request": 4,
    "plan": 5,
    "usage": 6,
    "turn_end": 7,
    "error": 8,
    "status": 9,
    "steer_rejected": 10,
    "custom_message": 11,
    "user_message": 12,
    "tool_progress": 13,
}


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(str(msg))


# ---------------------------------------------------------------- fixture


def sse(obj):
    return f"data: {json.dumps(obj)}\n\n".encode()


def text_frames(chunks, usage=True):
    frames = [{"choices": [{"index": 0, "delta": {"content": c}}]} for c in chunks]
    final = {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}
    if usage:
        final["usage"] = {"prompt_tokens": 11, "completion_tokens": 2}
    frames.append(final)
    return frames


def tool_call_frames(name, arguments, call_id="call_1"):
    return [
        {
            "choices": [
                {
                    "index": 0,
                    "delta": {
                        "role": "assistant",
                        "tool_calls": [
                            {
                                "index": 0,
                                "id": call_id,
                                "type": "function",
                                "function": {
                                    "name": name,
                                    "arguments": json.dumps(arguments),
                                },
                            }
                        ],
                    },
                }
            ]
        },
        {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]},
    ]


class Handler(BaseHTTPRequestHandler):
    """Scenario chosen by the prompt text; every scenario is deterministic."""

    protocol_version = "HTTP/1.1"
    status = 200
    flood_chunks = 96
    flood_width = 4096
    hold_seconds = 0.0

    def log_message(self, *_args):
        pass

    def _chunk(self, data):
        self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
        self.wfile.flush()

    def _stream(self, frames, hold=0.0):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for frame in frames:
            self._chunk(sse(frame))
            if hold:
                time.sleep(hold)
        self._chunk(b"data: [DONE]\n\n")
        self._chunk(b"")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length) or b"{}")
        messages = body.get("messages") or []
        prompt = " ".join(
            str(m.get("content"))
            for m in messages
            if m.get("role") == "user" and isinstance(m.get("content"), str)
        )
        answered_tool = any(m.get("role") == "tool" for m in messages)
        type(self).seen_requests += 1
        if type(self).status != 200:
            payload = json.dumps({"error": {"message": "fixture refuses"}}).encode()
            self.send_response(type(self).status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
        if "EMPTY" in prompt:
            self._stream(text_frames([""]))
        elif "FLOOD" in prompt:
            body_chunks = [
                f"{i:04d}" + "x" * (type(self).flood_width - 4)
                for i in range(type(self).flood_chunks)
            ]
            self._stream(text_frames(body_chunks), hold=type(self).hold_seconds)
        elif "TOOL" in prompt and not answered_tool:
            self._stream(tool_call_frames("list_files", {"path": "."}))
        elif "SHELL" in prompt and not answered_tool:
            self._stream(
                tool_call_frames("terminal", {"command": "echo permission-probe"})
            )
        else:
            self._stream(text_frames(["hello ", "world"]))


class QuietServer(ThreadingHTTPServer):
    """A consumer that hangs up mid-stream is a case here, not an error."""

    def handle_error(self, request, client_address):
        kind = sys.exc_info()[0]
        if kind is not None and issubclass(
            kind, (BrokenPipeError, ConnectionResetError)
        ):
            return
        super().handle_error(request, client_address)


def start_fixture(**attrs):
    for key, value in attrs.items():
        setattr(Handler, key, value)
    Handler.seen_requests = 0
    server = QuietServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


def stop_fixture(server, thread):
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)
    Handler.status = 200
    Handler.hold_seconds = 0.0


# ---------------------------------------------------------------- harness


class Workspace:
    """Throwaway HOME + workspace with fake credentials pointed at the mock."""

    def __init__(self, tmp, port):
        self.home = os.path.join(tmp, "home")
        self.workspace = os.path.join(tmp, "ws")
        os.makedirs(self.home, exist_ok=True)
        os.makedirs(self.workspace, exist_ok=True)
        env = dict(os.environ)
        for key in list(env):
            if key.endswith("_API_KEY") or key.endswith("_BASE_URL"):
                env.pop(key)
        env.pop("TNY_TOOLS", None)
        env.update(
            {
                "HOME": self.home,
                "OPENAI_API_KEY": "integration-test-not-real",
                "OPENAI_BASE_URL": f"http://127.0.0.1:{port}/v1",
                "OPENAI_WIRE_API": "chat",
                "OPENAI_DEFAULT_MODEL": "mock-model",
                "TNY_TEST_SUITE": "ask-events",
            }
        )
        self.env = env

    def argv(self, *args):
        return [TNY, "--provider", "openai", *args]

    def run(self, *args, stdin=b"", timeout=RUN_TIMEOUT, env=None):
        return subprocess.run(
            self.argv(*args),
            cwd=self.workspace,
            env=env or self.env,
            input=stdin,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )


def parse_stream(stdout: bytes):
    """Strict per-line JSON. A trailing partial line is a protocol error."""
    text = stdout.decode("utf-8")
    check(
        text == "" or text.endswith("\n"),
        f"stream did not end on a line: {text[-80:]!r}",
    )
    events = []
    for line in text.splitlines():
        check(line.strip() != "", "empty line in the event stream")
        try:
            event = json.loads(line)
        except json.JSONDecodeError as exc:
            raise Fail(f"line is not JSON: {line!r}: {exc}") from None
        check(isinstance(event, dict), f"line is not an object: {line!r}")
        events.append(event)
    return events


def assert_envelope(events):
    previous = None
    for event in events:
        for key in ENVELOPE:
            check(key in event, f"envelope key {key} missing: {event}")
        check(event["schema_version"] == 1, event)
        check(isinstance(event["sequence"], int), event)
        check(isinstance(event["timestamp_ms"], int), event)
        check(event["provider"] == "openai", event)
        check(isinstance(event["session_id"], str), event)
        check(isinstance(event["turn_id"], str), event)
        check(event["type"] in PAYLOAD, event)
        check(event["kind"] == KIND_OF[event["type"]], event)
        for key in PAYLOAD[event["type"]]:
            check(key in event, f"{event['type']} is missing {key}: {event}")
        extra = set(event) - set(ENVELOPE) - set(PAYLOAD[event["type"]])
        check(not extra, f"unknown field(s) {sorted(extra)} on {event['type']}")
        if previous is not None:
            check(event["sequence"] > previous, f"sequence not monotonic: {event}")
        previous = event["sequence"]


def one_terminal(events):
    terminals = [e for e in events if e["type"] == "turn_end"]
    check(len(terminals) == 1, f"expected exactly one turn_end, got {len(terminals)}")
    check(events[-1] is terminals[0], "turn_end was not the last event")
    return terminals[0]


def answer_text(events):
    return "".join(e["text"] for e in events if e["type"] == "text_delta")


# ---------------------------------------------------------------- cases


def case_stream_shape(ws):
    run = ws.run("ask", "--ephemeral", "--events=jsonl", "say hello")
    check(run.returncode == 0, f"exit {run.returncode}: {run.stderr!r}")
    events = parse_stream(run.stdout)
    assert_envelope(events)
    terminal = one_terminal(events)
    check(terminal["stop_reason"] == 0, terminal)
    check(answer_text(events) == "hello world", events)
    types = [e["type"] for e in events]
    check(types.index("usage") < types.index("turn_end"), types)
    check(types.index("text_delta") < types.index("usage"), types)
    usage = [e for e in events if e["type"] == "usage"][0]
    check(usage["input_tokens"] == 11 and usage["output_tokens"] == 2, usage)
    check(usage["cost"] is None and usage["has_cost"] is False, usage)
    # ephemeral turns keep the envelope keys even where a value is empty
    check(all(e["turn_id"] for e in events), "ephemeral turn_id vanished")
    deltas = [e for e in events if e["type"] == "text_delta"]
    check(all(e["message_id"] == "" for e in deltas), "absent message_id must be empty")
    # nothing human on stdout: every byte belongs to a canonical event
    check(b"hello world" not in run.stdout, "the rendered answer leaked onto stdout")


def case_empty_text(ws):
    run = ws.run("ask", "--ephemeral", "--events=jsonl", "EMPTY answer please")
    check(run.returncode == 0, f"exit {run.returncode}: {run.stderr!r}")
    events = parse_stream(run.stdout)
    assert_envelope(events)
    one_terminal(events)
    check(answer_text(events) == "", events)


def case_tool_events(ws):
    run = ws.run("ask", "--ephemeral", "--events=jsonl", "TOOL please list files")
    check(run.returncode == 0, f"exit {run.returncode}: {run.stderr!r}")
    events = parse_stream(run.stdout)
    assert_envelope(events)
    one_terminal(events)
    starts = [e for e in events if e["type"] == "tool_start"]
    ends = [e for e in events if e["type"] == "tool_end"]
    check(len(starts) == 1 and len(ends) == 1, [e["type"] for e in events])
    check(starts[0]["tool_name"] == "list_files", starts)
    check(ends[0]["tool_ok"] is True, ends)
    check(ends[0]["tool_id"] == starts[0]["tool_id"], (starts, ends))
    check(
        events.index(starts[0]) < events.index(ends[0]),
        "tool_end preceded tool_start",
    )
    # the human tool lines stay on stderr
    check(b"list_files" in run.stderr, run.stderr)


def case_permission_event(ws):
    run = ws.run(
        "--permission-mode",
        "ask",
        "ask",
        "--ephemeral",
        "--events=jsonl",
        "SHELL run something",
    )
    events = parse_stream(run.stdout)
    assert_envelope(events)
    one_terminal(events)
    requests = [e for e in events if e["type"] == "permission_request"]
    check(len(requests) == 1, [e["type"] for e in events])
    check(requests[0]["permission_id"] != "", requests)
    check(isinstance(requests[0]["permission_options"], int), requests)
    check(requests[0]["permission_options"] > 0, requests)
    ends = [e for e in events if e["type"] == "tool_end"]
    check(ends and ends[0]["tool_ok"] is False, ends)
    check(b"denying" in run.stderr, run.stderr)


def case_provider_error(ws_factory):
    ws, server, thread = ws_factory(status=401)
    try:
        run = ws.run("ask", "--ephemeral", "--events=jsonl", "say hello")
        check(run.returncode == 2, f"exit {run.returncode}: {run.stderr!r}")
        events = parse_stream(run.stdout)
        assert_envelope(events)
        terminal = one_terminal(events)
        errors = [e for e in events if e["type"] == "error"]
        check(len(errors) >= 1, [e["type"] for e in events])
        # TNY_STATUS_AUTH, the same value libtny's reader reports
        check(errors[0]["error_code"] == -6, errors)
        check(errors[0]["text"] != "", errors)
        check(terminal["stop_reason"] == 4, terminal)
    finally:
        stop_fixture(server, thread)


def case_startup_diagnostics(ws):
    """Failures before an accepted turn: stable stderr JSON, no fake events."""
    cases = [
        (("ask", "--events=jsonl", "--json", "x"), 1, "option_conflict"),
        (("ask", "--events=jsonl", "-B", "x"), 1, "option_conflict"),
        (("ask", "--events=jsonl", "--ephemeral", "--flubber"), 1, "invalid_option"),
        (("ask", "--events=jsonl", "--task", "no-such-task", "x"), 1, "invalid_option"),
        (
            (
                "ask",
                "--events=jsonl",
                "--ephemeral",
                "--output-schema",
                "/no/schema.json",
                "x",
            ),
            1,
            "invalid_option",
        ),
        (("ask", "--events=jsonl", "--resume", "0123456789abcdef", "x"), 1, "session"),
        (("ask", "--events=jsonl", "--ephemeral"), 1, "no_prompt"),
    ]
    for args, code, diag_code in cases:
        run = ws.run(*args, stdin=b"")
        check(run.returncode == code, f"{args}: exit {run.returncode}: {run.stderr!r}")
        check(run.stdout == b"", f"{args} wrote stdout: {run.stdout!r}")
        line = run.stderr.decode().strip().splitlines()[-1]
        diag = json.loads(line)
        check(diag["kind"] == "ask_error", diag)
        check(diag["schema_version"] == 1, diag)
        check(diag["code"] == diag_code, (args, diag))
        check(diag["message"], diag)
        check(
            "sequence" not in diag and "type" not in diag,
            f"diagnostic posed as an event: {diag}",
        )


def case_connect_failure_is_a_real_event(ws):
    """An accepted turn that cannot reach the provider reports the engine's
    own error and terminal — the CLI invents neither."""
    env = dict(ws.env, OPENAI_BASE_URL="http://127.0.0.1:1/v1")
    run = ws.run("ask", "--ephemeral", "--events=jsonl", "say hello", env=env)
    check(run.returncode == 2, f"exit {run.returncode}: {run.stderr!r}")
    events = parse_stream(run.stdout)
    assert_envelope(events)
    terminal = one_terminal(events)
    check(terminal["stop_reason"] == 4, terminal)
    errors = [e for e in events if e["type"] == "error"]
    check(errors and errors[0]["error_code"] == -7, errors)  # TNY_STATUS_IO


def case_progress_none(ws):
    quiet = ws.run(
        "ask", "--ephemeral", "--events=jsonl", "--progress=none", "TOOL list files"
    )
    check(quiet.returncode == 0, f"exit {quiet.returncode}: {quiet.stderr!r}")
    check(quiet.stderr == b"", f"progress=none wrote stderr: {quiet.stderr!r}")
    events = parse_stream(quiet.stdout)
    check(
        [e["type"] for e in events if e["type"].startswith("tool_")]
        == ["tool_start", "tool_end"],
        "progress=none dropped tool events from the stream",
    )
    loud = ws.run("ask", "--ephemeral", "--events=jsonl", "TOOL list files")
    check(loud.stderr != b"", "default progress printed nothing")

    # progress=none is independent of the event stream: it works the same on
    # the Markdown and final-JSON paths, and never touches their stdout.
    legacy = ws.run("ask", "--ephemeral", "--progress=none", "TOOL list files")
    check(legacy.returncode == 0, legacy.stderr)
    check(legacy.stderr == b"", f"legacy progress=none wrote stderr: {legacy.stderr!r}")
    check(legacy.stdout.strip() == b"hello world", legacy.stdout)
    blob = ws.run("ask", "--ephemeral", "--json", "--progress=none", "TOOL list files")
    check(blob.returncode == 0, blob.stderr)
    check(blob.stderr == b"", f"--json progress=none wrote stderr: {blob.stderr!r}")
    check(json.loads(blob.stdout)["output"] == "hello world", blob.stdout)


def case_progress_none_keeps_errors(ws_factory):
    ws, server, thread = ws_factory(status=500)
    try:
        run = ws.run(
            "ask", "--ephemeral", "--events=jsonl", "--progress=none", "say hello"
        )
        check(run.returncode == 2, run.returncode)
        check(b"tny:" in run.stderr, f"progress=none hid the failure: {run.stderr!r}")
        events = parse_stream(run.stdout)
        check(any(e["type"] == "error" for e in events), [e["type"] for e in events])
    finally:
        stop_fixture(server, thread)


def case_legacy_stdout(ws):
    plain = ws.run("ask", "--ephemeral", "say hello")
    check(plain.returncode == 0, plain.stderr)
    check(plain.stdout == b"hello world\n", plain.stdout)
    blob = ws.run("ask", "--ephemeral", "--json", "say hello")
    check(blob.returncode == 0, blob.stderr)
    payload = json.loads(blob.stdout)
    check(payload["output"] == "hello world", payload)
    check(payload["exit_code"] == 0 and payload["ephemeral"] is True, payload)
    check(payload["session_id"] == "", payload)
    saved = ws.run("ask", "say hello")
    check(saved.returncode == 0, saved.stderr)
    check(saved.stdout == b"hello world\n", saved.stdout)


def case_saved_session(ws):
    run = ws.run("ask", "--events=jsonl", "say hello")
    check(run.returncode == 0, f"exit {run.returncode}: {run.stderr!r}")
    events = parse_stream(run.stdout)
    assert_envelope(events)
    one_terminal(events)
    session_id = events[0]["session_id"]
    check(session_id != "", "saved turn had no session id")
    stored = ws.run("--json", "session", session_id)
    check(stored.returncode == 0, stored.stderr)
    document = json.loads(stored.stdout)
    check(document["id"] == session_id, document)
    check(
        any(m.get("content") == "hello world" for m in document["messages"]),
        document,
    )


def case_closed_consumer(ws):
    """A reader that hangs up: non-success, one diagnostic, no success claim."""
    read_fd, write_fd = os.pipe()
    os.close(read_fd)  # nobody will ever read this stream
    try:
        proc = subprocess.Popen(
            ws.argv("ask", "--ephemeral", "--events=jsonl", "FLOOD the pipe"),
            cwd=ws.workspace,
            env=ws.env,
            stdin=subprocess.DEVNULL,
            stdout=write_fd,
            stderr=subprocess.PIPE,
        )
    finally:
        os.close(write_fd)
    _, stderr = proc.communicate(timeout=RUN_TIMEOUT)
    check(proc.returncode == 2, f"closed consumer exit {proc.returncode}")
    diag = json.loads(stderr.decode().strip().splitlines()[-1])
    check(diag["code"] == "stream_io", diag)


def case_slow_reader(ws):
    """Backpressure delays delivery; it never drops or reorders an event."""
    read_fd, write_fd = os.pipe()
    proc = subprocess.Popen(
        ws.argv("ask", "--ephemeral", "--events=jsonl", "FLOOD the reader"),
        cwd=ws.workspace,
        env=ws.env,
        stdin=subprocess.DEVNULL,
        stdout=write_fd,
        stderr=subprocess.PIPE,
    )
    os.close(write_fd)
    peak_rss = [0]
    stop = threading.Event()

    def sample():
        while not stop.is_set():
            try:
                out = subprocess.run(
                    ["ps", "-o", "rss=", "-p", str(proc.pid)],
                    capture_output=True,
                    text=True,
                    timeout=5,
                )
                value = int(out.stdout.strip() or 0)
            except (ValueError, subprocess.SubprocessError):
                value = 0
            peak_rss[0] = max(peak_rss[0], value)
            time.sleep(0.05)

    sampler = threading.Thread(target=sample, daemon=True)
    sampler.start()
    chunks = []
    deadline = time.monotonic() + RUN_TIMEOUT
    selector = selectors.DefaultSelector()
    selector.register(read_fd, selectors.EVENT_READ)
    try:
        while time.monotonic() < deadline:
            if not selector.select(0.5):
                continue
            block = os.read(read_fd, 4096)
            if not block:
                break
            chunks.append(block)
            time.sleep(0.02)  # a consumer far slower than the producer
    finally:
        selector.close()
        os.close(read_fd)
        stop.set()
        sampler.join(timeout=5)
    _, stderr = proc.communicate(timeout=RUN_TIMEOUT)
    check(proc.returncode == 0, f"slow reader exit {proc.returncode}: {stderr!r}")
    events = parse_stream(b"".join(chunks))
    assert_envelope(events)
    one_terminal(events)
    deltas = [e["text"] for e in events if e["type"] == "text_delta"]
    check(len(deltas) == Handler.flood_chunks, f"{len(deltas)} deltas survived")
    for index, text in enumerate(deltas):
        check(text.startswith(f"{index:04d}"), f"delta {index} arrived out of order")
    # the retained bytes are bounded by the engine queue, not by the backlog
    check(
        0 < peak_rss[0] < 300 * 1024, f"peak RSS {peak_rss[0]} KiB while backpressured"
    )


def mcp_server_script(path):
    script = r"""#!/bin/sh
n=0
while IFS= read -r line; do
  case "$line" in
  *'"method":"initialize"'*) n=$((n+1));
    printf '{"jsonrpc":"2.0","id":%s,"result":{"protocolVersion":"2025-06-18"}}\n' "$n" ;;
  *'"method":"tools/list"'*) n=$((n+1));
    printf '{"jsonrpc":"2.0","id":%s,"result":{"tools":[]}}\n' "$n" ;;
  esac
done
"""
    with open(path, "w", encoding="utf-8") as stream:
        stream.write(script)
    os.chmod(path, 0o755)


def descendants(pid):
    out = subprocess.run(
        ["ps", "-ax", "-o", "pid=,ppid=,command="], capture_output=True, text=True
    )
    rows = []
    for line in out.stdout.splitlines():
        match = re.match(r"\s*(\d+)\s+(\d+)\s+(.*)", line)
        if match:
            rows.append((int(match.group(1)), int(match.group(2)), match.group(3)))
    return [r for r in rows if r[1] == pid]


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def case_interrupt_while_blocked(ws):
    """SIGINT reaches a writer stalled on a full pipe, and only owned work dies."""
    marker = os.path.join(ws.home, "owned-mcp.sh")
    mcp_server_script(marker)
    os.makedirs(os.path.join(ws.home, ".tny"), exist_ok=True)
    with open(os.path.join(ws.home, ".tny", "mcp.json"), "w", encoding="utf-8") as cfg:
        json.dump({"servers": {"owned": {"command": [marker]}}}, cfg)

    sentinel = subprocess.Popen(
        [sys.executable, "-c", "import time; time.sleep(120)"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    read_fd, write_fd = os.pipe()
    before_flags = fcntl.fcntl(write_fd, fcntl.F_GETFL)
    proc = subprocess.Popen(
        ws.argv("ask", "--ephemeral", "--events=jsonl", "FLOOD and stall"),
        cwd=ws.workspace,
        env=ws.env,
        stdin=subprocess.DEVNULL,
        stdout=write_fd,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    os.close(write_fd)
    try:
        # Wait until the pipe is full and the writer is stuck: the child stops
        # making progress while we read nothing at all.
        time.sleep(4.0)
        check(proc.poll() is None, "the run finished before the pipe filled")
        mcp_children = [
            row for row in descendants(proc.pid) if marker in row[2] or "sh" in row[2]
        ]
        check(mcp_children, "the owned MCP child never started")
        owned_pid = mcp_children[0][0]
        started = time.monotonic()
        os.kill(proc.pid, signal.SIGINT)
        # A writer that ignores the interrupt while stalled would wait for a
        # reader that never comes: assert that outcome instead of hanging.
        while proc.poll() is None and time.monotonic() - started < 15:
            time.sleep(0.1)
        elapsed = time.monotonic() - started
        check(
            proc.poll() is not None,
            f"the interrupt never reached the blocked writer ({elapsed:.1f}s)",
        )
        _, stderr = proc.communicate(timeout=10)
        check(proc.returncode == 130, f"interrupted exit {proc.returncode}")
        diag = json.loads(stderr.decode().strip().splitlines()[-1])
        check(diag["code"] == "cancelled", diag)
        deadline = time.monotonic() + 10
        while alive(owned_pid) and time.monotonic() < deadline:
            time.sleep(0.1)
        check(
            not alive(owned_pid), f"owned MCP child {owned_pid} survived the interrupt"
        )
        check(sentinel.poll() is None, "an unrelated process was killed")
        check(
            fcntl.fcntl(read_fd, fcntl.F_GETFL) & os.O_NONBLOCK == 0
            and before_flags & os.O_NONBLOCK == 0,
            "the inherited stdout description was left non-blocking",
        )
    finally:
        os.close(read_fd)
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        sentinel.kill()
        sentinel.wait(timeout=5)
        os.remove(os.path.join(ws.home, ".tny", "mcp.json"))


def case_interrupt_on_an_initially_full_pipe(ws):
    """stdout is already full before the first event, and the interrupt comes
    later than any grace period a writer might keep for itself: a descriptor
    the seam reports as not-writable is backpressure, never an excuse for a
    blocking write() that the signal cannot reach."""
    read_fd, write_fd = os.pipe()
    before_flags = fcntl.fcntl(write_fd, fcntl.F_GETFL)
    fcntl.fcntl(write_fd, fcntl.F_SETFL, before_flags | os.O_NONBLOCK)
    filled = 0
    try:
        while True:
            filled += os.write(write_fd, b"x" * 4096)
    except BlockingIOError:
        pass
    fcntl.fcntl(write_fd, fcntl.F_SETFL, before_flags)
    check(filled > 0, "the pipe would not fill")

    sentinel = subprocess.Popen(
        [sys.executable, "-c", "import time; time.sleep(120)"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    proc = subprocess.Popen(
        ws.argv("ask", "--ephemeral", "--events=jsonl", "FLOOD a full pipe"),
        cwd=ws.workspace,
        env=ws.env,
        stdin=subprocess.DEVNULL,
        stdout=write_fd,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    os.close(write_fd)
    try:
        # Past the 500ms mark where a writer used to give up on the seam, with
        # not one byte accepted since the process started.
        time.sleep(1.2)
        check(proc.poll() is None, "the run finished with a full pipe")
        started = time.monotonic()
        os.kill(proc.pid, signal.SIGINT)
        while proc.poll() is None and time.monotonic() - started < 15:
            time.sleep(0.1)
        elapsed = time.monotonic() - started
        check(
            proc.poll() is not None,
            f"the interrupt never reached a writer stalled on an "
            f"already-full pipe ({elapsed:.1f}s)",
        )
        _, stderr = proc.communicate(timeout=10)
        check(proc.returncode == 130, f"interrupted exit {proc.returncode}")
        diag = json.loads(stderr.decode().strip().splitlines()[-1])
        check(diag["code"] == "cancelled", diag)
        check(sentinel.poll() is None, "an unrelated process was killed")
        check(
            fcntl.fcntl(read_fd, fcntl.F_GETFL) & os.O_NONBLOCK == 0,
            "the inherited stdout description was left non-blocking",
        )
    finally:
        os.close(read_fd)
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        sentinel.kill()
        sentinel.wait(timeout=5)


# ---------------------------------------------------------------- driver


def main():
    if not os.access(TNY, os.X_OK):
        print(f"test_ask_events: {TNY} is not executable", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="tny-ask-events-") as tmp:

        def factory(**attrs):
            server, thread = start_fixture(**attrs)
            port = server.server_address[1]
            root = tempfile.mkdtemp(prefix="case-", dir=tmp)
            return Workspace(root, port), server, thread

        server, thread = start_fixture()
        port = server.server_address[1]
        ws = Workspace(os.path.join(tmp, "main"), port)
        # The browser build has no fork, no owned child processes and no POSIX
        # signal delivery to exercise; its stream cases still must hold.
        wasm = os.environ.get("TNY_TEST_EXPECT_WASM") == "1"
        try:
            case_stream_shape(ws)
            case_empty_text(ws)
            case_startup_diagnostics(ws)
            case_connect_failure_is_a_real_event(ws)
            case_legacy_stdout(ws)
            if wasm:
                print(
                    "    skip (wasm): tools, permissions, saved sessions, pipes, signals",
                    file=sys.stderr,
                )
            else:
                case_tool_events(ws)
                case_permission_event(ws)
                case_progress_none(ws)
                case_saved_session(ws)
                case_closed_consumer(ws)
                case_slow_reader(ws)
                case_interrupt_while_blocked(ws)
                case_interrupt_on_an_initially_full_pipe(ws)
        finally:
            stop_fixture(server, thread)
        case_provider_error(factory)
        if not wasm:
            case_progress_none_keeps_errors(factory)
    print("ok  ask --events=jsonl: envelope, order, options, backpressure, cancel")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (Fail, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1) from None
