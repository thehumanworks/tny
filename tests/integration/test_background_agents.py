#!/usr/bin/env python3
"""Real PTY/runner fixture: completed-tool checkpoint, exec and same-turn attach."""

import base64
import fcntl
import json
import os
import signal
import struct
import subprocess
import tempfile
import termios
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from code_mode_fixture import code_chat_frames, lua_string
from test_tui import BANNER, TNY, Screen, Term, base_env, clean

TNY = os.path.abspath(TNY)


def until(predicate, term=None, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        if term:
            term.pump(0.05)
        else:
            time.sleep(0.02)
    raise AssertionError(
        "condition timed out" + ("\n" + term.buf[-12000:] if term else "")
    )


def saved_turn_complete(session, turns):
    # The backend saves its incremented turn count before runtime finalization
    # restores the named provider and the runner publishes the completed status.
    state = json.loads(session.read_text())
    return state.get("turns") == turns and state.get("status") == "done"


class Provider:
    def __init__(
        self, ws, no_tools=False, steer=False, images=False, transformed=False
    ):
        self.requests = []
        self.errors = []
        self.finish = threading.Event()
        self.second = threading.Event()
        fixture = self
        image_bytes = base64.b64decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScLbtAAAAABJRU5ErkJggg=="
        )
        if images:
            (ws / "image.png").write_bytes(image_bytes)

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_POST(self):
                try:
                    body = json.loads(
                        self.rfile.read(int(self.headers["Content-Length"]))
                    )
                    fixture.requests.append(body)
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.end_headers()

                    def event(value):
                        value = code_chat_frames([value])[0]
                        self.wfile.write(
                            ("data: " + json.dumps(value) + "\n\n").encode()
                        )
                        self.wfile.flush()

                    if len(fixture.requests) == 3:
                        assert (
                            body["messages"][-1]["content"] == "FOLLOWUP-AFTER-DONE"
                        ), body
                        event(
                            {
                                "choices": [
                                    {
                                        "delta": {"content": "LIVE-FOLLOWUP-OK"},
                                        "finish_reason": "stop",
                                    }
                                ]
                            }
                        )
                        self.wfile.write(b"data: [DONE]\n\n")
                        return
                    if len(fixture.requests) == 1 and not no_tools:
                        calls = [
                            {
                                "index": 0,
                                "id": "first",
                                "type": "function",
                                "function": {
                                    "name": "terminal",
                                    "arguments": json.dumps(
                                        {
                                            "command": "printf first\\n >> effects; touch started; while [ ! -f release ]; do sleep 0.05; done",
                                            "timeout_s": 30,
                                        }
                                    ),
                                },
                            },
                            {
                                "index": 1,
                                "id": "second",
                                "type": "function",
                                "function": {
                                    "name": "terminal",
                                    "arguments": json.dumps(
                                        {"command": "printf second\\n >> effects"}
                                    ),
                                },
                            },
                        ]
                        if images:
                            # Capture inside the execution server before the shell
                            # parks. Active shell-to-owner image IO is forbidden;
                            # the later path rewrite must not change captured pixels.
                            first = calls[0]["function"]
                            first["name"] = "run_code"
                            first["arguments"] = json.dumps(
                                {
                                    "code": 'print(tools.call("read_image", \'{"path":"image.png"}\'))\n'
                                    + f'print(tools.call("terminal", {lua_string(first["arguments"])}))',
                                    "timeout_ms": 30000,
                                }
                            )
                        event(
                            {
                                "choices": [
                                    {
                                        "delta": {
                                            "reasoning_content": "REASONING-KEEP",
                                            "tool_calls": calls,
                                        }
                                    }
                                ]
                            }
                        )
                        event(
                            {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]}
                        )
                    else:
                        if not no_tools:
                            messages = body["messages"]
                            results = [m for m in messages if m.get("role") == "tool"]
                            assert [m["tool_call_id"] for m in results] == [
                                "first",
                                "second",
                            ], messages
                            if transformed:
                                assert results[0]["content"] == "EFFECTIVE-RESULT", (
                                    results
                                )
                            if images:
                                parts = [
                                    m["content"]
                                    for m in messages
                                    if m.get("role") == "user"
                                    and isinstance(m.get("content"), list)
                                ]
                                urls = [
                                    part["image_url"]["url"]
                                    for batch in parts
                                    for part in batch
                                    if part.get("type") == "image_url"
                                ]
                                assert urls == [
                                    "data:image/png;base64,"
                                    + base64.b64encode(image_bytes).decode()
                                ], parts
                            assert body["model"] == "fixture-model", body
                            assert body["reasoning_effort"] == "high", body
                            if steer:
                                assert messages[-1] == {
                                    "role": "user",
                                    "content": "STEER-KEEP",
                                }, messages
                            assert "REASONING-KEEP" in json.dumps(messages), messages
                            assert len(
                                [m for m in messages if m.get("role") == "user"]
                            ) == 1 + int(steer) + int(images), messages
                            assert (ws / "effects").read_text().count("first") == 1
                            assert (ws / "effects").read_text().count("second") == 1
                        event(
                            {"choices": [{"delta": {"content": "SAME-TURN-RUNNING\n"}}]}
                        )
                        fixture.second.set()
                        assert fixture.finish.wait(30), (
                            "test did not release final response"
                        )
                        event(
                            {
                                "choices": [
                                    {
                                        "delta": {"content": "FINISHED-ONCE\n"},
                                        "finish_reason": "stop",
                                    }
                                ]
                            }
                        )
                    self.wfile.write(b"data: [DONE]\n\n")
                except (BrokenPipeError, ConnectionResetError):
                    pass
                except Exception as exc:
                    fixture.errors.append(repr(exc))

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.finish.set()
        self.server.shutdown()
        self.server.server_close()


def run_case(
    no_tools=False,
    permission=False,
    steer=False,
    images=False,
    transformed=False,
    trigger="\x1b[D",
):
    """An active turn detaches immediately and keeps its runner and owner lock."""
    with tempfile.TemporaryDirectory(prefix="tny-agents-") as home:
        ws = Path(home) / "ws"
        ws.mkdir()
        provider = Provider(ws, no_tools, steer, images, transformed)
        env = base_env(
            home,
            {
                "OPENAI_API_KEY": "fixture-secret",
                "OPENAI_BASE_URL": f"http://127.0.0.1:{provider.server.server_port}/v1",
                "OPENAI_WIRE_API": "chat",
                "TNY_ISOLATE": "1",
                "TNY_PROVIDER_RETRIES": "0",
            },
        )
        if transformed:
            ext = Path(home) / ".tny/extensions/effective.py"
            ext.parent.mkdir(parents=True, exist_ok=True)
            ext.write_text(
                "from tny_ext import PostToolUseEvent, replace_tool_result\n"
                "import json, os\n"
                "def setup(api):\n"
                "    initialized = False\n"
                "    @api.on('*')\n"
                "    def record(event):\n"
                "        nonlocal initialized\n"
                "        if event.type == 'session_start': initialized = True\n"
                "        with open(os.environ['EVENTS_PATH'], 'a') as f:\n"
                "            f.write(json.dumps({'type': event.type, 'reason': getattr(event, 'reason', None), 'initialized': initialized}) + '\\n')\n"
                "    @api.on(PostToolUseEvent)\n"
                "    def replace(event):\n"
                "        assert initialized, 'host received a hook before initialization'\n"
                "        if event.tool_id == 'first': return replace_tool_result('EFFECTIVE-RESULT')\n"
            )
            env["EVENTS_PATH"] = str(Path(home) / "events.jsonl")
            env["TNY_EXTENSION_HOST"] = str(
                Path(__file__).resolve().parents[2] / "python/tny_extension_host.py"
            )
        term = Term(
            [
                TNY,
                "--provider",
                "openai",
                *([] if transformed else ["--no-extensions"]),
                "--max-steps",
                "3",
                "--model",
                "fixture-model",
                "--effort",
                "high",
                "--permission-mode",
                "ask" if permission else "yolo",
            ],
            env,
            str(ws),
        )
        attached = None
        session = None
        try:
            term.expect(BANNER)
            term.send("perform fixture\r")
            if permission:
                term.expect("approve?")
                # Focused permission Left must neither deny nor detach.
                term.send("\x1b[")
                term.pump(0.03)
                term.send("D")
                term.pump(0.1)
                assert not (ws / "started").exists()
                term.send("y")
            if no_tools:
                term.expect("SAME-TURN-RUNNING")
            else:
                until(lambda: (ws / "started").exists(), term)
            session = next((Path(home) / ".tny/sessions").glob("*/*/session.json"))
            pid = int((session.parent / "pid").read_text())
            candidates = [session.parent / "sock"]
            for root in (env.get("TMPDIR", "/tmp"), "/tmp"):
                candidates.append(
                    Path(root) / f"tny-{os.getuid()}" / f"{session.parent.name}.sock"
                )
            socket_path = next(path for path in candidates if path.exists())
            socket_identity = socket_path.stat()

            def assert_same_owner():
                assert int((session.parent / "pid").read_text()) == pid
                current = socket_path.stat()
                assert (current.st_dev, current.st_ino) == (
                    socket_identity.st_dev,
                    socket_identity.st_ino,
                )
                assert writer_live(session), "handoff released writer authority"

            assert_same_owner()
            if steer:
                term.send("STEER-KEEP\r")
                term.expect("steer")
            term.send(trigger)
            term.expect("Agents — all saved sessions", timeout=5)
            assert_clean_dashboard(term, BANNER, "REASONING-KEEP", "FINISHED-ONCE")
            saved = json.loads(session.read_text())
            assert saved["background"] is True and saved["status"] == "running", saved
            assert "continuation" not in saved, saved
            assert_same_owner()
            # The dashboard opens before the active tool or response completes.
            assert len(provider.requests) == 1, provider.requests
            if not no_tools:
                assert not (ws / "release").exists()
                assert "second" not in (ws / "effects").read_text()
                if images:
                    (ws / "image.png").write_bytes(b"later bytes must not be reopened")
                (ws / "release").touch()
            term.send("\x04")
            assert term.wait() == 0
            assert_same_owner()
            attached = Term([TNY, "agents"], env, str(ws))
            attached.expect("Agents — all saved sessions")
            attached.send("\r")
            attached.expect("Attached " + session.parent.name)
            if permission:
                attached.expect("approve?")
                attached.pump(0.1)
                assert not provider.second.is_set(), "reattach widened ask to yolo"
                attached.send("y")
            attached.expect("SAME-TURN-RUNNING")
            until(provider.second.is_set, attached)
            expected_requests = 1 if no_tools else 2
            assert len(provider.requests) == expected_requests, provider.requests
            assert_same_owner()

            rival = Term([TNY, "agents"], env, str(ws))
            try:
                rival.expect("Agents — all saved sessions")
                rival.send("\r")
                rival.expect("Saved read-only")
                before = len(provider.requests)
                rival.send("/continue\r")
                rival.expect_next("Still read-only: owner unavailable")
                assert len(provider.requests) == before
                assert_same_owner()
                rival.send("/quit\r")
                assert rival.wait() == 0
            finally:
                rival.close()
            for detach in ("/agents\r", "\x18", "\x1b[D"):
                attached.send(detach)
                attached.expect_next("Agents — all saved sessions")
                assert_same_owner()
                attached.send("\r")
                attached.expect_next("Attached")
            provider.finish.set()
            attached.expect("FINISHED-ONCE")
            until(
                lambda: json.loads(session.read_text()).get("status") == "done",
                attached,
            )
            final = json.loads(session.read_text())
            assert final["turns"] == 1 and "continuation" not in final, final
            assert final["result"]["output"].count("FINISHED-ONCE") == 1, final
            assert_same_owner()
            listed = subprocess.run(
                [TNY, "agents", "--json"],
                env=env,
                cwd=ws,
                capture_output=True,
                text=True,
                timeout=5,
            )
            live_row = json.loads(listed.stdout)["agents"][0]
            assert live_row["status"] == "done" and live_row["live"], live_row
            if transformed:
                records = [
                    json.loads(line)
                    for line in Path(env["EVENTS_PATH"]).read_text().splitlines()
                ]
                assert all(record["initialized"] for record in records), records
                events = [record["type"] for record in records]
                assert events.count("session_start") == 1, events
                assert events.count("user_prompt_submit") == 1, events
            if not no_tools:
                attached.send("FOLLOWUP-AFTER-DONE\r")
                attached.expect("LIVE-FOLLOWUP-OK")
                until(lambda: saved_turn_complete(session, 2), attached)
                assert len(provider.requests) == 3, provider.requests
                assert_same_owner()
            attached.send("/quit\r")
            assert attached.wait() == 0
            until(lambda: not writer_live(session))
            listed = subprocess.run(
                [TNY, "agents", "--json"],
                env=env,
                cwd=ws,
                capture_output=True,
                text=True,
                timeout=5,
            )
            row = json.loads(listed.stdout)["agents"][0]
            assert row["status"] == "done" and not row["running"], row
            final = json.loads(session.read_text())
            final["status"] = "running"
            session.write_text(json.dumps(final))
            listed = subprocess.run(
                [TNY, "agents", "--json"],
                env=env,
                cwd=ws,
                capture_output=True,
                text=True,
                timeout=5,
            )
            assert json.loads(listed.stdout)["agents"][0]["status"] == "stale"
            assert not provider.errors, provider.errors
        finally:
            provider.close()
            for client in (term, attached):
                if client:
                    client.close()
            if session and (session.parent / "pid").exists():
                subprocess.run(
                    [TNY, "session", "stop", session.parent.name, "--kill"],
                    env=env,
                    cwd=ws,
                    capture_output=True,
                    timeout=12,
                )
    print(
        "PASS immediate background",
        "no-tools"
        if no_tools
        else "permission"
        if permission
        else "steer"
        if steer
        else "image"
        if images
        else "transformed"
        if transformed
        else "streaming tool",
    )


def writer_live(session):
    with (session.parent / "lock").open() as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return True
        return False


def continue_owner(term, sid):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        start = len(clean(term.buf))
        term.send("/continue\r")
        until(
            lambda: any(
                reply in clean(term.buf)[start:]
                for reply in ("Attached " + sid, "Still read-only:")
            ),
            term,
            seconds=5,
        )
        if "Attached " + sid in clean(term.buf)[start:]:
            term.expect_next("Attached " + sid)
            return
        term.expect_next("Still read-only:")
    raise AssertionError("owner slot never became available\n" + term.buf[-4000:])


def snapshot_state(home):
    """Private persistence only, not mutable PTY output; detect new artifacts too."""
    root = Path(home) / ".tny"
    return {
        str(path.relative_to(root)): (path.read_bytes(), path.stat().st_mtime_ns)
        for path in root.rglob("*")
        if path.is_file()
    }


def saved_fixture(
    home, ws, backend="fixture", checkpoint=False, sid="1234567890abcdef"
):
    # Match the physical cwd used by tny, including macOS /var -> /private/var.
    ws = ws.resolve()
    value = 0xCBF29CE484222325
    for byte in str(ws).encode():
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    directory = Path(home) / ".tny/sessions" / f"{value:016x}" / sid
    directory.mkdir(parents=True)
    session = directory / "session.json"
    body = {
        "id": sid,
        "workspace": str(ws),
        "backend": backend,
        "model": "saved-model",
        "title": "saved conversation",
        "background": True,
        "status": "done",
        "turns": 1,
        "updated": "2026-09-19T00:00:00Z",
        "messages": [
            {"role": "user", "content": "ORIGINAL-QUESTION"},
            {"role": "assistant", "content": "SAVED-ANSWER"},
        ],
    }
    if checkpoint:
        body["continuation"] = {"_resume": {"resumable": True}}
    session.write_text(json.dumps(body))
    (directory / "lock").touch()
    return session


class ContinuationProvider:
    """Immediate loopback turns, with observable inference and OAuth requests."""

    def __init__(self, workspace_probe=False):
        self.requests = []
        self.refreshes = []
        self.workspace_probe = workspace_probe
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_POST(self):
                raw = self.rfile.read(int(self.headers["Content-Length"]))
                if self.path == "/oauth/token":
                    fixture.refreshes.append(raw)
                    self.send_response(400)
                    self.end_headers()
                    self.wfile.write(b'{"error":"fixture refuses refresh"}')
                    return
                body = json.loads(raw)
                fixture.requests.append(body)
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                if fixture.workspace_probe and len(fixture.requests) == 1:
                    event = {
                        "choices": [
                            {
                                "delta": {
                                    "tool_calls": [
                                        {
                                            "index": 0,
                                            "id": "workspace-probe",
                                            "type": "function",
                                            "function": {
                                                "name": "terminal",
                                                "arguments": json.dumps(
                                                    {
                                                        "command": "pwd > global-resume-proof"
                                                    }
                                                ),
                                            },
                                        }
                                    ]
                                },
                                "finish_reason": "tool_calls",
                            }
                        ]
                    }
                else:
                    event = {
                        "choices": [
                            {
                                "delta": {
                                    "content": f"SAVED-TURN-{len(fixture.requests)}"
                                },
                                "finish_reason": "stop",
                            }
                        ]
                    }
                event = code_chat_frames([event])[0]
                self.wfile.write(("data: " + json.dumps(event) + "\n\n").encode())
                self.wfile.write(b"data: [DONE]\n\n")

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def env(self, home):
        return base_env(
            home,
            {
                "FIXTURE_API_KEY": "synthetic-only",
                "FIXTURE_BASE_URL": f"http://127.0.0.1:{self.server.server_port}/v1",
                "FIXTURE_WIRE_API": "chat",
                "TNY_ISOLATE": "1",
                "TNY_PROVIDER_RETRIES": "0",
            },
        )

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def stop_fixture(session, env, ws):
    if writer_live(session):
        stopped = subprocess.run(
            [TNY, "session", "stop", session.parent.name, "--kill"],
            env=env,
            cwd=ws,
            capture_output=True,
            text=True,
            timeout=12,
        )
        assert stopped.returncode == 0, stopped.stderr
    until(lambda: not writer_live(session))


def global_saved_sessions():
    """All workspaces and ordinary saved sessions are visible and resumable."""
    with tempfile.TemporaryDirectory(prefix="tny-agents-global-") as home:
        launch = Path(home) / "launch"
        other = Path(home) / "other\nworkspace"
        third = Path(home) / "third"
        for path in (launch, other, third):
            path.mkdir()
        foreground = saved_fixture(home, other, sid="1234567890abcdea")
        foreground_state = json.loads(foreground.read_text())
        foreground_state.pop("background")
        foreground_state.pop("status")
        foreground_state["title"] = "foreground\nsession"
        foreground_state["updated"] = "2026-09-20T00:00:00Z"
        foreground.write_text(json.dumps(foreground_state))
        background = saved_fixture(home, third, sid="1234567890abcdeb")
        no_auth = base_env(home, {"TNY_ISOLATE": "1"})
        listed = subprocess.run(
            [TNY, "agents", "--json"],
            env=no_auth,
            cwd=launch,
            capture_output=True,
            text=True,
            timeout=10,
        )
        assert listed.returncode == 0, listed.stderr
        rows = json.loads(listed.stdout)["agents"]
        assert len(rows) == 2, rows
        by_id = {row["session_id"]: row for row in rows}
        assert by_id[foreground.parent.name]["workspace"] == str(other.resolve())
        assert by_id[foreground.parent.name]["status"] == "saved"
        assert by_id[background.parent.name]["workspace"] == str(third.resolve())
        assert by_id[background.parent.name]["status"] == "done"
        plain = subprocess.run(
            [TNY, "agents"],
            env=no_auth,
            cwd=launch,
            capture_output=True,
            text=True,
            timeout=10,
        )
        assert plain.returncode == 0 and len(plain.stdout.splitlines()) == 2, (
            plain.stdout,
            plain.stderr,
        )
        assert (
            "other workspace" in plain.stdout and "foreground session" in plain.stdout
        )
        assert "\x1b" not in plain.stdout
        provider = ContinuationProvider(workspace_probe=True)
        env = provider.env(home)
        term = Term([TNY, "agents"], env, str(launch))
        try:
            term.expect("Agents — all saved sessions")
            term.expect_on_screen("other workspace")
            term.send("\r")  # newest row is the ordinary saved foreground session
            term.expect("Saved read-only " + foreground.parent.name)
            term.expect("SAVED-ANSWER")
            term.send("FOLLOWUP-FROM-ELSEWHERE\r")
            term.expect("SAVED-TURN-2")
            until(lambda: saved_turn_complete(foreground, 2), term)
            assert (other / "global-resume-proof").read_text().strip() == str(
                other.resolve()
            )
            assert not (launch / "global-resume-proof").exists()
            assert len(provider.requests) == 2, provider.requests
            assert provider.requests[0]["messages"][-1]["content"] == (
                "FOLLOWUP-FROM-ELSEWHERE"
            )
            term.send("/quit\r")
            assert term.wait() == 0
        finally:
            term.close()
            stop_fixture(foreground, env, other)
            provider.close()
    print("PASS global saved sessions across workspaces and original-cwd continuation")


def workspace_sections_and_fuzzy_filter():
    """The visible dashboard groups cwd first and opens the filtered identity."""
    with tempfile.TemporaryDirectory(prefix="tny-nav-", dir="/tmp") as home:
        current = Path(home) / "current"
        remote = Path(home) / "quartz-research"
        last = Path(home) / "z-last"
        for workspace in (current, remote, last):
            workspace.mkdir()
        entries = []
        for workspace, sid, title, updated, answer in (
            (current, "1111111111111111", "LOCAL-NEW", "2026-09-20", "LOCAL-ANSWER"),
            (
                current,
                "2222222222222222",
                "LOCAL-OLD",
                "2026-09-19",
                "LOCAL-OLD-ANSWER",
            ),
            (remote, "1111111111111111", "REMOTE-NEW", "2026-09-22", "FILTERED-ANSWER"),
            (
                remote,
                "3333333333333333",
                "REMOTE-OLD",
                "2026-09-21",
                "REMOTE-OLD-ANSWER",
            ),
            (last, "4444444444444444", "LAST-ROW", "2026-09-18", "LAST-ANSWER"),
        ):
            session = saved_fixture(home, workspace, sid=sid)
            body = json.loads(session.read_text())
            body.update(title=title, updated=updated + "T00:00:00Z")
            body["messages"][-1]["content"] = answer
            session.write_text(json.dumps(body))
            entries.append(session)
        before = snapshot_state(home)
        term = Term([TNY, "agents"], base_env(home), str(current))
        try:
            term.expect_on_screen("LAST-ROW")
            lines = term.screen().splitlines()
            local_heading = lines.index(str(current.resolve()))
            remote_heading = lines.index(str(remote.resolve()))
            local_new = next(i for i, line in enumerate(lines) if "LOCAL-NEW" in line)
            local_old = next(i for i, line in enumerate(lines) if "LOCAL-OLD" in line)
            remote_new = next(i for i, line in enumerate(lines) if "REMOTE-NEW" in line)
            assert (
                local_heading < local_new < local_old < remote_heading < remote_new
            ), lines
            assert lines[local_new].startswith("  > "), lines
            assert lines[local_old].startswith("    "), lines
            assert lines.count(str(current.resolve())) == 1, lines
            assert lines.count(str(remote.resolve())) == 1, lines

            # Noncontiguous, case-insensitive matching, with q accepted as text.
            term.send("qRZ")
            term.expect_gone_from_screen("LOCAL-NEW")
            term.expect_on_screen("REMOTE-NEW")
            assert term.proc.poll() is None
            assert "LAST-ROW" not in term.screen()
            term.send("X")
            term.expect_on_screen("No matching workspaces")
            term.send("\r")
            term.pump(0.1)
            assert "Saved read-only" not in term.buf
            term.send("\x7f")
            term.expect_on_screen("REMOTE-NEW")
            term.send("\x1b[B")
            term.expect_on_screen("  > 3333333333333333")
            # Refresh changes recency but must preserve the selected bucket/id.
            body = json.loads(entries[3].read_text())
            body["updated"] = "2026-09-23T00:00:00Z"
            entries[3].write_text(json.dumps(body))
            before = snapshot_state(home)
            paints = term.buf.count("Agents — all saved sessions")
            until(lambda: term.buf.count("Agents — all saved sessions") > paints, term)
            term.expect_on_screen("  > 3333333333333333")
            term.send("\x1b[A")  # now clamped on the newly first row
            term.send("\r")
            term.expect("REMOTE-OLD-ANSWER")
            term.expect("Workspace: " + str(remote.resolve()))
            term.send("/agents\r")
            term.expect_next("Agents — all saved sessions")
            term.send("\x04")
            assert term.wait() == 0
            assert term.restored()
            assert snapshot_state(home) == before
        finally:
            term.close()
    print("PASS cwd-first sections, fuzzy paths, empty results and refreshed selection")


def workspace_filter_paste_and_small_viewport():
    """Every selected session retains its heading while scrolling/resizing."""
    with tempfile.TemporaryDirectory(prefix="tny-nav-small-", dir="/tmp") as home:
        workspace = Path(home) / "café-project"
        workspace.mkdir()
        for index in range(16):
            session = saved_fixture(home, workspace, sid=f"{index + 1:016x}")
            body = json.loads(session.read_text())
            body.update(
                title=f"SCROLL-{index:02d}",
                updated=f"2026-09-{20 - index:02d}T00:00:00Z",
            )
            session.write_text(json.dumps(body))
        term = Term([TNY, "agents"], base_env(home), str(workspace))
        try:
            term.expect_on_screen("SCROLL-00")
            term.send("\x1b[200~café\x1b[201~")
            term.expect_on_screen("café")
            term.send("\x7f")  # removes one Unicode character, never a partial byte
            term.send("\x1b[200~é\x1b[201~")
            term.pump(0.2)
            assert "No matching workspaces" not in term.screen()
            assert "�" not in term.screen()
            # Clear the query without exiting, then resize and cross old 8-row pages.
            term.send("\x1b")
            term.pump(0.2)
            assert term.proc.poll() is None
            fcntl.ioctl(
                term.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 10, 100, 0, 0)
            )
            os.kill(term.proc.pid, signal.SIGWINCH)
            term.pump(0.2)
            term.buf = ""  # emulate the new screen dimensions from the next repaint
            for index in range(1, 16):
                term.send("\x1b[B")
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    term.pump(0.05)
                    screen = Screen(rows=10, cols=100)
                    screen.feed(term.buf)
                    rows = screen.text().splitlines()
                    selected = next(
                        (i for i, row in enumerate(rows) if row.startswith("  > ")),
                        None,
                    )
                    if selected is not None and f"SCROLL-{index:02d}" in rows[selected]:
                        heading = rows.index(str(workspace.resolve()))
                        assert heading < selected, rows
                        break
                else:
                    raise AssertionError(f"selected row {index} hidden: {rows}")
            term.send("\x04")
            assert term.wait() == 0
            assert term.restored()
        finally:
            term.close()
    print("PASS pasted Unicode workspace filter and small-screen section scrolling")


def legacy_session_without_workspace():
    with tempfile.TemporaryDirectory(prefix="tny-agents-legacy-") as home:
        original = Path(home) / "original"
        launch = Path(home) / "launch"
        original.mkdir()
        launch.mkdir()
        session = saved_fixture(home, original)
        body = json.loads(session.read_text())
        body.pop("workspace")
        session.write_text(json.dumps(body))
        # Its physical bucket identifies the current workspace even without old
        # document metadata: filtering that known path must keep the session.
        local_view = Term([TNY, "agents"], base_env(home), str(original))
        try:
            local_view.expect_on_screen(str(original.resolve()))
            local_view.send("orgnl")
            local_view.expect_on_screen("path: orgnl")
            local_view.expect_on_screen(session.parent.name)
            assert "No matching workspaces" not in local_view.screen()
            local_view.send("\r")
            local_view.expect("SAVED-ANSWER")
            local_view.send("/quit\r")
            assert local_view.wait() == 0
            assert "workspace" not in json.loads(session.read_text())
        finally:
            local_view.close()
        provider = ContinuationProvider(workspace_probe=True)
        env = provider.env(home)
        term = None
        try:
            listed = subprocess.run(
                [TNY, "agents", "--json"],
                env=base_env(home),
                cwd=launch,
                capture_output=True,
                text=True,
                timeout=10,
            )
            assert listed.returncode == 0, listed.stderr
            row = json.loads(listed.stdout)["agents"][0]
            assert row["session_id"] == session.parent.name
            assert row["workspace"] is None
            assert row["workspace_bucket"] == session.parent.parent.name
            term = Term([TNY, "agents"], env, str(launch))
            term.expect("Agents — all saved sessions")
            term.send("\r")
            term.expect("Saved read-only " + session.parent.name)
            term.expect("Saved workspace unknown; continuing uses current cwd:")
            term.expect("SAVED-ANSWER")
            term.send("LEGACY-FOLLOWUP\r")
            term.expect("SAVED-TURN-2")
            until(lambda: saved_turn_complete(session, 2), term)
            assert (launch / "global-resume-proof").read_text().strip() == str(
                launch.resolve()
            )
            assert not (original / "global-resume-proof").exists()
            assert len(provider.requests) == 2
            term.send("/quit\r")
            assert term.wait() == 0
        finally:
            if term:
                term.close()
            stop_fixture(session, env, original)
            provider.close()
    print("PASS legacy session opens by physical bucket and continues in current cwd")


def duplicate_ids_keep_selected_bucket():
    with tempfile.TemporaryDirectory(prefix="tny-agents-duplicate-") as home:
        first = Path(home) / "first"
        second = Path(home) / "second"
        launch = Path(home) / "launch"
        for path in (first, second, launch):
            path.mkdir()
        newest = saved_fixture(home, first)
        older = saved_fixture(home, second)  # same ID in another physical bucket
        for session, title, answer, updated in (
            (newest, "newer row", "NEWER-ANSWER", "2026-09-22T00:00:00Z"),
            (older, "selected row", "SELECTED-ANSWER", "2026-09-21T00:00:00Z"),
        ):
            body = json.loads(session.read_text())
            body["title"] = title
            body["updated"] = updated
            body["messages"][-1]["content"] = answer
            session.write_text(json.dumps(body))
        term = Term([TNY, "agents"], base_env(home), str(launch))
        try:
            term.expect("Agents — all saved sessions")
            term.send("\x1b[B")
            term.expect_on_screen("selected row")
            paints = term.buf.count("Agents — all saved sessions")
            until(lambda: term.buf.count("Agents — all saved sessions") > paints, term)
            term.send("\r")
            term.expect("SELECTED-ANSWER")
            assert "Workspace: " + str(second.resolve()) in clean(term.buf)
            term.send("/quit\r")
            assert term.wait() == 0
        finally:
            term.close()
    print("PASS duplicate session IDs retain selected physical workspace")


def completed_session_continuation():
    """A1/A3/A6: a real once-runner exits; a fresh dashboard continues its ID."""
    with tempfile.TemporaryDirectory(prefix="tny-agents-completed-") as home:
        ws = Path(home) / "ws"
        ws.mkdir()
        provider = ContinuationProvider()
        env = provider.env(home)
        term = None
        session = None
        try:
            launched = subprocess.run(
                [
                    TNY,
                    "--provider",
                    "fixture",
                    "--model",
                    "saved-model",
                    "ask",
                    "-B",
                    "FIRST",
                ],
                env=env,
                cwd=ws,
                capture_output=True,
                text=True,
                timeout=10,
            )
            assert launched.returncode == 0, launched.stderr
            session = next((Path(home) / ".tny/sessions").glob("*/*/session.json"))
            until(lambda: not writer_live(session))
            initial = json.loads(session.read_text())
            assert initial["status"] == "done" and initial["turns"] == 1, initial
            assert len(provider.requests) == 1
            before = snapshot_state(home)
            # Launch choices must not override the selected session's selectors.
            term = Term(
                [TNY, "--provider", "openai", "--model", "WRONG", "agents"],
                env,
                str(ws),
            )
            term.expect("Agents — all saved sessions")
            term.send("\r")
            term.expect("Saved read-only " + initial["id"])
            term.expect("SAVED-TURN-1")
            assert not writer_live(session)
            assert snapshot_state(home) == before
            assert len(provider.requests) == 1
            term.send("SECOND\r")
            term.expect("SAVED-TURN-2")
            until(lambda: saved_turn_complete(session, 2), term)
            final = json.loads(session.read_text())
            assert final["id"] == initial["id"]
            assert final["messages"][: len(initial["messages"])] == initial["messages"]
            assert final["backend"] == "fixture" and final["model"] == "saved-model", (
                final
            )
            assert len(provider.requests) == 2, provider.requests
            assert provider.requests[1]["model"] == "saved-model"
            assert provider.requests[1]["messages"][-1]["content"] == "SECOND"
            term.send("/quit\r")
            assert term.wait() == 0
        finally:
            if term:
                term.close()
            if session:
                stop_fixture(session, env, ws)
            provider.close()
    print(
        "PASS completed_session_continuation: same ID/history, one additional turn/request"
    )


def locked_saved_inspection_retry():
    """A2/A4/A5: done while lock held; no socket; latest state wins on retry."""
    with tempfile.TemporaryDirectory(prefix="tny-agents-held-") as home:
        ws = Path(home) / "ws"
        ws.mkdir()
        session = saved_fixture(home, ws, backend="missing-fixture-profile")
        provider = ContinuationProvider()
        env = provider.env(home)
        lock = (session.parent / "lock").open("r+b")
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        # A stale listener must not be unlinked by a failed continuation.
        listener = session.parent / "sock"
        listener.write_text("unreachable listener sentinel")
        identity = listener.stat().st_ino
        term = Term([TNY, "agents"], env, str(ws))
        try:
            term.expect("Agents — all saved sessions")
            term.send("\r")
            term.expect("Saved read-only")
            term.expect("SAVED-ANSWER")
            before = snapshot_state(home)
            for command in (
                "/new",
                "/reset",
                "/rename BAD",
                "/compact",
                "/model BAD",
                "/provider openai",
                "/permissions yolo",
                "/workspace add /tmp",
                "/effort low",
                "/fast",
                "/max-steps set 1",
                "/task clear",
                "/ssh off",
                "/worktree BAD",
                "/resume last",
                "/setup",
                "/logout",
                "/login",
                "/status",
                "/undo",
                "/optimise BAD",
                "/dictate",
            ):
                term.send(command + "\r")
                term.expect_next("read-only session replica:")
                term.send("\x05\x15")  # optimisation intentionally retains its draft
                assert snapshot_state(home) == before, command
            for command in ("NOT-SUBMITTED", "/continue"):
                term.send(command + "\r")
                term.expect_next("Still read-only: owner unavailable")
                assert snapshot_state(home) == before
            assert listener.stat().st_ino == identity
            assert len(provider.requests) == 0
            # Exit does not save this replica, including failed execution paths.
            term.send("/quit\r")
            assert term.wait() == 0
            assert snapshot_state(home) == before
            term.close()
            term = Term([TNY, "agents"], env, str(ws))
            term.expect("Agents — all saved sessions")
            term.send("\r")
            term.expect("SAVED-ANSWER")
            term.send("/continue\r")
            term.expect("Still read-only: owner unavailable")
            # The actual writer publishes newer history/selectors *after* viewing.
            latest = json.loads(session.read_text())
            latest["messages"] += [
                {"role": "user", "content": "FRESH-QUESTION"},
                {"role": "assistant", "content": "FRESH-ANSWER"},
            ]
            latest["turns"] = 2
            latest["model"] = "fresh-model"
            latest["backend"] = "fixture"
            session.write_text(json.dumps(latest))
            lock.close()
            until(lambda: not writer_live(session), term)
            term.send("AFTER-RELEASE\r")
            term.expect("SAVED-TURN-1")
            until(lambda: saved_turn_complete(session, 3), term)
            final = json.loads(session.read_text())
            assert final["id"] == latest["id"]
            assert final["messages"][:4] == latest["messages"]
            assert len(provider.requests) == 1
            request = provider.requests[0]
            assert request["model"] == "fresh-model", request
            assert "FRESH-ANSWER" in json.dumps(request)
            assert request["messages"][-1]["content"] == "AFTER-RELEASE", request
            term.send("/quit\r")
            assert term.wait() == 0
        finally:
            lock.close()
            term.close()
            stop_fixture(session, env, ws)
            provider.close()
    print(
        "PASS locked_saved_inspection_retry: guards, exit, retry, fresh-under-lock history/model"
    )


def dashboard_cancels_provider_wizard():
    """A5: Ctrl-X from normal setup cannot carry writable wizard state into a replica."""
    with tempfile.TemporaryDirectory(prefix="tny-agents-wizard-") as home:
        ws = Path(home) / "ws"
        ws.mkdir()
        session = saved_fixture(home, ws)
        settings = Path(home) / ".tny/settings.json"
        settings.write_text(json.dumps({"models": {"fixture": "launch-model"}}))
        provider = ContinuationProvider()
        env = {**provider.env(home), "WIZARD_LEAK_API_KEY": "synthetic-only"}
        lock = (session.parent / "lock").open("r+b")
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        term = Term([TNY, "--provider", "fixture", "--no-extensions"], env, str(ws))
        try:
            term.expect(BANNER)
            term.send("/provider setup wizard-leak\r")
            term.expect("base url (OpenAI-compatible")
            before = snapshot_state(home)
            term.send("\x18")
            term.expect("Agents — all saved sessions")
            term.send("\r")
            term.expect("Saved read-only " + session.parent.name)
            term.expect("SAVED-ANSWER")
            assert snapshot_state(home) == before
            for answer in (
                f"http://127.0.0.1:{provider.server.server_port}/v1",
                "WIZARD_LEAK_API_KEY",
                "wizard-leak-model",
                "/continue",
            ):
                term.send(answer + "\r")
                term.expect_next("Still read-only: owner unavailable")
                term.expect_on_screen("fixture  saved-model")
                assert snapshot_state(home) == before, answer
                assert not provider.requests and not provider.refreshes
            assert "wizard-leak" not in settings.read_text()
            lock.close()
            until(lambda: not writer_live(session), term)
            term.send("/continue\r")
            term.expect_next("Continuing " + session.parent.name)
            assert not provider.requests
            term.send("AFTER-WIZARD\r")
            term.expect("SAVED-TURN-1")
            until(lambda: saved_turn_complete(session, 2), term)
            final = json.loads(session.read_text())
            assert final["id"] == session.parent.name
            assert final["backend"] == "fixture" and final["model"] == "saved-model", (
                final
            )
            assert len(provider.requests) == 1
            request = provider.requests[0]
            assert request["model"] == "saved-model"
            assert request["messages"][-1]["content"] == "AFTER-WIZARD"
            assert "wizard-leak" not in json.dumps(request)
            term.send("/quit\r")
            assert term.wait() == 0
        finally:
            lock.close()
            term.close()
            stop_fixture(session, env, ws)
            provider.close()
    print(
        "PASS dashboard_cancels_provider_wizard: normal TUI transition, no setup writes, /continue routes"
    )


def killed_idle_runner_continuation(stale_client=False):
    """A1/A4/A6: a dead idle runner cannot pin the UI to stale history or model."""
    with tempfile.TemporaryDirectory(prefix="tny-agents-dead-idle-") as home:
        ws = Path(home) / "ws"
        ws.mkdir()
        session = saved_fixture(home, ws)
        provider = ContinuationProvider()
        env = provider.env(home)
        term = Term([TNY, "agents"], env, str(ws))
        try:
            term.expect("Agents — all saved sessions")
            term.send("\r")
            term.expect("Saved read-only")
            term.send("BEFORE-RUNNER-DEATH\r")
            term.expect("SAVED-TURN-1")
            until(lambda: saved_turn_complete(session, 2), term)
            term.expect_gone_from_screen("working")
            initial = json.loads(session.read_text())
            assert initial["status"] == "done", initial
            assert writer_live(session)
            old_pid = int((session.parent / "pid").read_text())
            if stale_client:
                # Freeze our owned frontend and observe its stop event, not PID
                # liveness. Queue input before it can consume the socket EOF.
                os.kill(term.proc.pid, signal.SIGSTOP)
                stopped, status = os.waitpid(term.proc.pid, os.WUNTRACED)
                assert stopped == term.proc.pid and os.WIFSTOPPED(status)
            os.kill(old_pid, signal.SIGKILL)
            until(lambda: not writer_live(session))
            if not stale_client:
                term.expect_on_screen("read-only  fixture  saved-model")
            # Publish a fresh selector/history under the writer lock after the
            # UI's old runner and replica were created. Continuation must reload.
            with (session.parent / "lock").open("r+b") as writer:
                fcntl.flock(writer, fcntl.LOCK_EX | fcntl.LOCK_NB)
                latest = json.loads(session.read_text())
                assert latest["messages"] == initial["messages"]
                latest["model"] = "after-death-model"
                latest["messages"] += [
                    {"role": "user", "content": "NEWER-SAVED-QUESTION"},
                    {"role": "assistant", "content": "NEWER-SAVED-ANSWER"},
                ]
                latest["turns"] = 3
                session.write_text(json.dumps(latest))
            term.send("AFTER-RUNNER-DEATH\r")
            if stale_client:
                os.kill(term.proc.pid, signal.SIGCONT)
            term.expect("SAVED-TURN-2")
            until(lambda: json.loads(session.read_text()).get("turns") == 4, term)
            final = json.loads(session.read_text())
            assert final["id"] == initial["id"]
            assert final["messages"][: len(latest["messages"])] == latest["messages"]
            assert final["model"] == "after-death-model"
            assert int((session.parent / "pid").read_text()) != old_pid
            assert len(provider.requests) == 2
            request = provider.requests[1]
            assert request["model"] == "after-death-model", request
            assert "NEWER-SAVED-ANSWER" in json.dumps(request)
            assert request["messages"][-1]["content"] == "AFTER-RUNNER-DEATH"
            term.send("/quit\r")
            assert term.wait() == 0
        finally:
            term.close()
            stop_fixture(session, env, ws)
            provider.close()
    print(
        "PASS killed_idle_runner_continuation:",
        "stale-client" if stale_client else "observed-EOF",
    )


def provider_free_inspection():
    """A3/A7/A8: no secrets/profile, no OAuth refresh, no checkpoint activation."""
    provider = ContinuationProvider()
    try:
        for backend in ("missing-fixture-profile", "cursor", "codex"):
            with tempfile.TemporaryDirectory(prefix="tny-agents-noauth-") as home:
                ws = Path(home) / "ws"
                ws.mkdir()
                session = saved_fixture(home, ws, backend=backend)
                auth = Path(home) / ".tny/codex-auth.json"
                auth.write_text(
                    json.dumps(
                        {
                            "tokens": {
                                "access_token": "expired-fixture",
                                "refresh_token": "fixture",
                            },
                            "expires_at": "2020-01-01T00:00:00Z",
                            "last_refresh": "2020-01-01T00:00:00Z",
                        }
                    )
                )
                env = base_env(
                    home,
                    {
                        "TNY_CODEX_OAUTH_ISSUER": f"http://127.0.0.1:{provider.server.server_port}",
                        "TNY_CODEX_BASE_URL": f"http://127.0.0.1:{provider.server.server_port}",
                    },
                )
                before = snapshot_state(home)
                term = Term([TNY, "agents"], env, str(ws))
                try:
                    term.expect("Agents — all saved sessions")
                    term.send("\r")
                    term.expect("Saved read-only")
                    term.expect("SAVED-ANSWER")
                    if backend != "codex":
                        term.send("/continue\r")
                        term.expect(
                            "Still read-only: execution configuration is unavailable"
                        )
                    term.send("/quit\r")
                    assert term.wait() == 0
                    assert snapshot_state(home) == before
                    assert not writer_live(session)
                    assert not provider.requests and not provider.refreshes
                finally:
                    term.close()
            print("PASS provider_free_inspection:", backend)
        for checkpoint, isolate, late in (
            (True, "1", False),
            (True, "1", True),
            (False, "0", False),
        ):
            with tempfile.TemporaryDirectory(prefix="tny-agents-noactivation-") as home:
                ws = Path(home) / "ws"
                ws.mkdir()
                session = saved_fixture(home, ws, checkpoint=checkpoint and not late)
                env = {**provider.env(home), "TNY_ISOLATE": isolate}
                before = snapshot_state(home)
                term = Term([TNY, "agents"], env, str(ws))
                try:
                    term.expect("Agents — all saved sessions")
                    term.send("\r")
                    term.expect("SAVED-ANSWER")
                    assert snapshot_state(home) == before
                    if late:
                        # Inspection is stale: pending work appeared before execution intent.
                        with (session.parent / "lock").open() as writer:
                            fcntl.flock(writer, fcntl.LOCK_EX | fcntl.LOCK_NB)
                            body = json.loads(session.read_text())
                            body["continuation"] = {"_resume": {"resumable": True}}
                            session.write_text(json.dumps(body))
                        before = snapshot_state(home)
                    term.send("NOT-QUEUED\r")
                    term.expect(
                        "Your prompt was not submitted or queued"
                        if checkpoint
                        else "continuation requires a native session runner"
                    )
                    term.send("/continue\r")
                    term.expect_next(
                        "checkpoint cannot be replayed"
                        if checkpoint
                        else "continuation requires a native session runner"
                    )
                    term.send("/quit\r")
                    assert term.wait() == 0
                    assert snapshot_state(home) == before
                    assert not writer_live(session)
                    assert not provider.requests and not provider.refreshes
                finally:
                    term.close()
            print(
                "PASS no_activation:",
                "late-checkpoint"
                if late
                else "checkpoint"
                if checkpoint
                else "in-process",
            )
    finally:
        provider.close()


def cancellation_after_handoff():
    with tempfile.TemporaryDirectory(prefix="tny-background-cancel-") as home:
        ws = Path(home) / "ws"
        ws.mkdir()
        provider = Provider(ws)
        env = base_env(
            home,
            {
                "OPENAI_API_KEY": "fixture",
                "OPENAI_BASE_URL": f"http://127.0.0.1:{provider.server.server_port}/v1",
                "OPENAI_WIRE_API": "chat",
            },
        )
        term = Term([TNY, "--no-extensions", "--provider", "openai"], env, str(ws))
        try:
            term.expect(BANNER)
            # Idle Left still edits the ordinary composer.
            term.send("ac\x1b[Db\r")
            until(lambda: (ws / "started").exists(), term)
            assert provider.requests[0]["messages"][-1]["content"] == "abc"
            session = next((Path(home) / ".tny/sessions").glob("*/*/session.json"))
            term.send("draft ac\x1b[Db")
            term.pump(0.2)
            assert "Background armed" not in term.buf
            assert "draft abc" in term.screen(), term.screen()
            term.send("\x05\x15")  # end then clear draft; empty-composer Left detaches
            term.send("\x1b[D")
            term.expect("Agents — all saved sessions")
            saved = json.loads(session.read_text())
            assert saved["background"] is True, saved
            assert writer_live(session)
            term.send("\r")
            term.expect("Attached " + session.parent.name)
            term.send("\x03")
            until(
                lambda: json.loads(session.read_text()).get("status") == "interrupted",
                term,
                8,
            )
            saved = json.loads(session.read_text())
            assert saved.get("background"), saved
            assert "second" not in (ws / "effects").read_text()
            term.send("\x04")
            assert term.wait() == 0
        finally:
            provider.close()
            term.close()
            session = next(
                (Path(home) / ".tny/sessions").glob("*/*/session.json"), None
            )
            if session:
                subprocess.run(
                    [TNY, "session", "stop", session.parent.name, "--kill"],
                    env=env,
                    cwd=ws,
                    capture_output=True,
                    timeout=12,
                )
    print("PASS active cancellation after handoff; draft Left edits")


def idle_left_dashboard():
    with tempfile.TemporaryDirectory(prefix="tny-agents-idle-left-") as home:
        ws = Path(home) / "ws"
        ws.mkdir()
        env = base_env(home)
        term = Term([TNY, "--no-extensions", "--provider", "openai"], env, str(ws))
        try:
            term.expect(BANNER)
            term.send("\x1b[D")
            term.expect("Agents — all saved sessions")
            term.expect("No saved sessions")
            assert not (Path(home) / ".tny/sessions").exists()
            term.send("\x04")
            assert term.wait() == 0
        finally:
            term.close()
    print("PASS idle empty Left opens all-saved dashboard")


def terminal_loss_after_handoff():
    with tempfile.TemporaryDirectory(prefix="tny-agents-hangup-") as home:
        ws = Path(home) / "ws"
        ws.mkdir()
        provider = Provider(ws, no_tools=True)
        env = base_env(
            home,
            {
                "OPENAI_API_KEY": "fixture",
                "OPENAI_BASE_URL": f"http://127.0.0.1:{provider.server.server_port}/v1",
                "OPENAI_WIRE_API": "chat",
            },
        )
        term = Term([TNY, "--provider", "openai", "--no-extensions"], env, str(ws))
        attached = None
        session = None
        try:
            term.expect(BANNER)
            term.send("hangup fixture\r")
            term.expect("SAME-TURN-RUNNING")
            session = next((Path(home) / ".tny/sessions").glob("*/*/session.json"))
            pid = int((session.parent / "pid").read_text())
            candidates = [session.parent / "sock"]
            for root in (env.get("TMPDIR", "/tmp"), "/tmp"):
                candidates.append(
                    Path(root) / f"tny-{os.getuid()}" / f"{session.parent.name}.sock"
                )
            sock = next(path for path in candidates if path.exists())
            identity = sock.stat().st_ino
            term.send("\x1b[D")
            term.expect("Agents — all saved sessions")
            assert json.loads(session.read_text())["background"] is True
            assert writer_live(session) and sock.stat().st_ino == identity
            # Closing the PTY master models terminal loss after the durable
            # acknowledgement, while the provider response is still pending.
            os.close(term.master)
            term.master = os.open(os.devnull, os.O_RDONLY)  # Term.close owns this fd
            term.proc.wait(timeout=5)
            assert int((session.parent / "pid").read_text()) == pid
            assert writer_live(session) and sock.stat().st_ino == identity
            provider.finish.set()
            until(lambda: saved_turn_complete(session, 1), seconds=10)
            final = json.loads(session.read_text())
            assert final["result"]["output"].count("FINISHED-ONCE") == 1, final
            assert len(provider.requests) == 1 and not provider.errors
            attached = Term([TNY, "agents"], env, str(ws))
            attached.expect("Agents — all saved sessions")
            attached.send("\r")
            attached.expect("FINISHED-ONCE")
            assert "Attached " + session.parent.name in clean(
                attached.buf
            ) or "Saved read-only " + session.parent.name in clean(attached.buf)
            attached.send("/quit\r")
            assert attached.wait() == 0
        finally:
            term.close()
            if attached:
                attached.close()
            if session:
                stop_fixture(session, env, ws)
            provider.close()
    print("PASS terminal hangup after handoff preserves the active runner")


def unsupported_in_process():
    with tempfile.TemporaryDirectory(prefix="tny-background-inprocess-") as home:
        ws = Path(home) / "ws"
        ws.mkdir()
        provider = Provider(ws, no_tools=True)
        env = base_env(
            home,
            {
                "OPENAI_API_KEY": "fixture",
                "OPENAI_BASE_URL": f"http://127.0.0.1:{provider.server.server_port}/v1",
                "OPENAI_WIRE_API": "chat",
                "TNY_ISOLATE": "0",
            },
        )
        term = Term([TNY, "--provider", "openai", "--no-extensions"], env, str(ws))
        try:
            term.expect(BANNER)
            term.send("inprocess fixture\r")
            term.expect("SAME-TURN-RUNNING")
            for command in ("\x1b[D", "\x18", "/agents\r"):
                term.send(command)
                term.expect_next("background requires a saved native session runner")
                assert "Agents — all saved sessions" not in term.buf
                assert "\x1b[2J" not in term.buf, "refused handoff cleared the chat"
            provider.finish.set()
            term.expect("FINISHED-ONCE")
            session = next((Path(home) / ".tny/sessions").glob("*/*/session.json"))
            term.send("/agents\r")
            term.expect("Agents — all saved sessions")
            term.expect(session.parent.name)
            term.send("\x04")
            assert term.wait() == 0
            assert not provider.errors, provider.errors
            assert not json.loads(session.read_text()).get("background")
        finally:
            provider.close()
            term.close()
    print("PASS in-process Left/CtrlX/agents reject safely and preserve active turn")


def assert_clean_dashboard(term, *old_text):
    term.expect_on_screen("Agents — all saved sessions")
    screen = term.screen()
    assert screen.splitlines()[0].startswith("Agents — all saved sessions"), screen
    for text in old_text:
        assert text not in screen, screen
    assert "\x1b[2J" in term.buf and "\x1b[3J" in term.buf, term.buf
    clears = term.buf.count("\x1b[2J")
    paints = term.buf.count("Agents — all saved sessions")
    # Observe a repaint, not a fixed sleep: idle polling can delay the 500 ms
    # refresh. It must neither clear again nor restore old chat text.
    until(
        lambda: term.buf.count("Agents — all saved sessions") > paints, term, seconds=5
    )
    assert term.buf.count("\x1b[2J") == clears, term.buf
    screen = term.screen()
    assert screen.splitlines()[0].startswith("Agents — all saved sessions"), screen
    for text in old_text:
        assert text not in screen, screen


def empty_dashboard():
    with tempfile.TemporaryDirectory(prefix="tny-agents-empty-") as home:
        env = base_env(home, {"OPENAI_BASE_URL": "http://127.0.0.1:1"})
        for command in (None, "\x18", "/agents\r"):
            for color in ("auto", "never"):
                args = [
                    TNY,
                    "--no-extensions",
                    "--provider",
                    "openai",
                    f"--color={color}",
                ]
                if command is None:
                    args.append("agents")
                # Seed the physical terminal, not just our captured transcript.
                term = Term(args, env, home, prelude=b"OLD-SHELL-TEXT\n")
                try:
                    if command:
                        term.expect_on_screen(BANNER)
                        term.send(command)
                    term.expect("No saved sessions")
                    assert_clean_dashboard(term, "OLD-SHELL-TEXT", BANNER)
                    assert not (Path(home) / ".tny/sessions").exists(), (
                        "dashboard prewarmed a runner"
                    )
                    term.send("\x04")
                    assert term.wait() == 0
                    assert term.restored(), "dashboard left the terminal raw"
                finally:
                    term.close()
        for args in (["agents"], ["agents", "--json"]):
            result = subprocess.run(
                [TNY, *args],
                env=env,
                cwd=home,
                capture_output=True,
                text=True,
                timeout=10,
            )
            assert result.returncode == 0, result.stderr
            assert "\x1b" not in result.stdout, result.stdout
            if "--json" in args:
                assert json.loads(result.stdout) == {"kind": "agents", "agents": []}
            else:
                assert result.stdout == "No saved sessions.\n"
    print("PASS clean full-screen dashboard, no provider work, plain/JSON unchanged")


def unattended_permission():
    from test_background import start_mock

    mock, port = start_mock(MOCK_SENSITIVE="1")
    try:
        with tempfile.TemporaryDirectory(prefix="tny-background-ask-") as home:
            env = base_env(
                home,
                {
                    "OPENAI_BASE_URL": f"http://127.0.0.1:{port}/v1",
                    "OPENAI_API_KEY": "fixture",
                    "OPENAI_WIRE_API": "responses",
                },
            )
            launched = subprocess.run(
                [
                    TNY,
                    "--provider",
                    "openai",
                    "--permission-mode",
                    "ask",
                    "ask",
                    "-B",
                    "list files in .",
                ],
                env=env,
                cwd=home,
                capture_output=True,
                text=True,
                timeout=5,
            )
            assert launched.returncode == 0, launched.stderr
            session = next((Path(home) / ".tny/sessions").glob("*/*/session.json"))
            try:
                until(
                    lambda: json.loads(session.read_text()).get("status") != "running",
                    seconds=6,
                )
                assert "permission" in session.read_text().lower(), session.read_text()
            finally:
                subprocess.run(
                    [TNY, "session", "stop", session.parent.name, "--kill"],
                    env=env,
                    cwd=home,
                    capture_output=True,
                    timeout=12,
                )
    finally:
        mock.terminate()
        mock.wait(timeout=5)
    print("PASS unattended ask -B denies without handoff permission parking")


if __name__ == "__main__":
    workspace_sections_and_fuzzy_filter()
    workspace_filter_paste_and_small_viewport()
    global_saved_sessions()
    legacy_session_without_workspace()
    duplicate_ids_keep_selected_bucket()
    completed_session_continuation()
    locked_saved_inspection_retry()
    dashboard_cancels_provider_wizard()
    killed_idle_runner_continuation()
    killed_idle_runner_continuation(stale_client=True)
    provider_free_inspection()

    run_case()
    run_case(no_tools=True)
    run_case(permission=True)
    run_case(steer=True)
    run_case(images=True)
    run_case(transformed=True)
    run_case(trigger="\x18")
    run_case(trigger="/agents\r")

    cancellation_after_handoff()
    idle_left_dashboard()
    terminal_loss_after_handoff()
    empty_dashboard()

    unattended_permission()

    unsupported_in_process()
