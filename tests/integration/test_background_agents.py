#!/usr/bin/env python3
"""Real PTY/runner fixture: completed-tool checkpoint, exec and same-turn attach."""

import base64
import fcntl
import json
import os
import shlex
import shutil
import signal
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from test_tui import BANNER, TNY, Term, base_env, clean

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
                                            "command": (
                                                f"{shlex.quote(TNY)} image attach image.png; "
                                                if images
                                                else ""
                                            )
                                            + "printf first\\n >> effects; touch started; while [ ! -f release ]; do sleep 0.05; done",
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
    restart_failure=False,
    images=False,
    transformed=False,
    restart_fault=None,
):
    with tempfile.TemporaryDirectory(prefix="tny-agents-") as home:
        ws = Path(home) / "ws"
        ws.mkdir()
        provider = Provider(ws, no_tools, steer, images, transformed)
        binary = TNY
        if restart_failure or restart_fault:
            binary = str(Path(home) / "fixture-tny")
            shutil.copy2(TNY, binary)
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
        if restart_fault:
            library = Path(home) / "restart-fault.so"
            source = (
                Path(__file__).resolve().parents[1] / "fixtures/runner_restart_fault.c"
            )
            subprocess.run(
                [
                    os.environ.get("CC", "cc"),
                    "-shared",
                    "-fPIC",
                    str(source),
                    "-o",
                    str(library),
                ],
                check=True,
                capture_output=True,
            )
            env[
                "DYLD_INSERT_LIBRARIES"
                if os.uname().sysname == "Darwin"
                else "LD_PRELOAD"
            ] = str(library)
            env["TNY_FIXTURE_RESTART_FAULT"] = restart_fault
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
                "        assert initialized, 'fresh host received a hook before initialization'\n"
                "        if event.tool_id == 'first': return replace_tool_result('EFFECTIVE-RESULT')\n"
            )
            env["EVENTS_PATH"] = str(Path(home) / "events.jsonl")
            env["TNY_EXTENSION_HOST"] = str(
                Path(__file__).resolve().parents[2] / "python/tny_extension_host.py"
            )
        term = Term(
            [
                binary,
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
                # Split Left in a focused permission must neither deny nor arm.
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
            old_pid = int((session.parent / "pid").read_text())
            candidates = [session.parent / "sock"]
            for root in (env.get("TMPDIR", "/tmp"), "/tmp"):
                candidates.append(
                    Path(root) / f"tny-{os.getuid()}" / f"{session.parent.name}.sock"
                )
            socket_path = next(path for path in candidates if path.exists())
            socket_identity = socket_path.stat()

            def assert_writer_and_listener_retained():
                current = socket_path.stat()
                assert (current.st_dev, current.st_ino) == (
                    socket_identity.st_dev,
                    socket_identity.st_ino,
                )
                with (session.parent / "lock").open("r+b") as lock:
                    try:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        pass
                    else:
                        raise AssertionError("restart released writer authority")

            assert_writer_and_listener_retained()
            if steer:
                term.send("STEER-KEEP\r")
                term.expect("steer")
            if restart_failure:
                # Make the re-exec fail on every platform: the runner spawns
                # its own executable path, and on Linux /proc/self/exe follows
                # a rename, so hiding the file would still restart there.
                # A copy with no permission bits fails posix_spawn with
                # EACCES on Linux and macOS alike.
                os.chmod(binary, 0)
            term.send("\x1b[D\x1b[D")
            term.expect("Background armed")
            assert "\x1b[2J" not in term.buf, "arming handoff cleared the chat early"
            if images:
                (ws / "image.png").write_bytes(b"later bytes must not be reopened")
            if no_tools:
                provider.finish.set()
            else:
                (ws / "release").touch()
            if restart_failure or restart_fault == "post-go":
                term.expect(
                    "Background restart failed; continuing in foreground", timeout=20
                )
                until(provider.second.is_set, term)
                assert int((session.parent / "pid").read_text()) == old_pid
                assert_writer_and_listener_retained()
                assert len(provider.requests) == 2, provider.requests
                effects = (ws / "effects").read_text()
                assert effects.count("first") == effects.count("second") == 1
                provider.finish.set()
                term.expect("FINISHED-ONCE")
                term.send("\x04")
                assert term.wait() == 0
                assert not provider.errors, provider.errors
                print("PASS background restart failure retains foreground")
                return
            if restart_fault == "post-run":
                until(
                    lambda: (
                        json.loads(session.read_text())
                        .get("continuation", {})
                        .get("_resume", {})
                        .get("resumable")
                        is True
                    ),
                    term,
                )
                # The successor exits after RUN but before consuming the durable
                # checkpoint. No pending effect has been released.
                until(lambda: not writer_live(session), term)
                assert len(provider.requests) == 1
                assert "second" not in (ws / "effects").read_text()
                term.send("\x04")
                assert term.wait() == 0
                saved_checkpoint = session.read_text()
                assert "fixture-secret" not in saved_checkpoint
                assert "127.0.0.1" not in saved_checkpoint
                env.pop("TNY_FIXTURE_RESTART_FAULT")
                # An already-activated checkpoint cannot establish whether an
                # external effect ran. It must be rejected, never replayed.
                consumed = json.loads(saved_checkpoint)
                consumed["continuation"]["_resume"]["resumable"] = False
                session.write_text(json.dumps(consumed))
                refused = Term(
                    [TNY, "--provider", "openai", "resume", session.parent.name],
                    env,
                    str(ws),
                )
                try:
                    refused.expect("checkpoint cannot be replayed", timeout=10)
                    refused.send("\x04")
                    refused.wait()
                finally:
                    refused.close()
                assert "second" not in (ws / "effects").read_text()
                session.write_text(saved_checkpoint)
                # A different endpoint must never receive or execute recovery.
                wrong = {**env, "OPENAI_BASE_URL": "http://127.0.0.1:1/v1"}
                rejected = Term(
                    [TNY, "--provider", "openai", "resume", session.parent.name],
                    wrong,
                    str(ws),
                )
                try:
                    rejected.expect("checkpoint cannot be replayed", timeout=10)
                    rejected.send("\x04")
                    rejected.wait()
                finally:
                    rejected.close()
                assert "second" not in (ws / "effects").read_text()
                before_inspect = snapshot_state(home)
                attached = Term([TNY, "agents"], env, str(ws))
                attached.expect("Background agents")
                attached.send("\r")
                attached.expect("Saved read-only")
                attached.expect("perform fixture")
                assert snapshot_state(home) == before_inspect
                assert not writer_live(session)
                assert len(provider.requests) == 1
                attached.send("CHECKPOINT-PROMPT-NOT-QUEUED\r")
                attached.expect("Your prompt was not submitted or queued")
                assert snapshot_state(home) == before_inspect
                assert len(provider.requests) == 1
                attached.send("/continue\r")
                if permission:
                    attached.expect("approve?")
                    attached.pump(0.1)
                    assert not provider.second.is_set(), (
                        "disk recovery widened permission"
                    )
                    attached.send("y")
                attached.expect("SAME-TURN-RUNNING", timeout=20)
                until(provider.second.is_set, attached)
                provider.finish.set()
                attached.expect("FINISHED-ONCE")
                until(
                    lambda: json.loads(session.read_text()).get("status") == "done",
                    attached,
                )
                final = json.loads(session.read_text())
                assert final["turns"] == 1 and final["result"]["steps"] == 2, final
                assert "continuation" not in final, final
                attached.send("\x04")
                assert attached.wait() == 0
                assert not provider.errors, provider.errors
                print(
                    "PASS unconsumed checkpoint recovery with exact batch/config/steps"
                )
                return
            term.expect("Background agents", timeout=20)
            assert_clean_dashboard(term, BANNER, "REASONING-KEEP", "FINISHED-ONCE")
            saved = json.loads(session.read_text())
            assert saved["background"] is True, saved
            if not no_tools:
                pid = int((session.parent / "pid").read_text())
                assert pid != old_pid, (old_pid, pid)
                assert_writer_and_listener_retained()
                if not permission:
                    until(provider.second.is_set, term)
                else:
                    term.pump(0.25)
                    assert not provider.second.is_set()
                    assert "second" not in (ws / "effects").read_text()
                # Dashboard quit cannot stop the detached worker.
                term.send("q")
                assert term.wait() == 0
                os.kill(pid, 0)
                attached = Term([TNY, "agents"], env, str(ws))
                attached.expect("Background agents")
                before = len(provider.requests)
                attached.send("\r")
                attached.expect("Attached")
                if permission:
                    attached.expect("approve?")
                    attached.pump(0.1)
                    assert not provider.second.is_set(), "reattach widened ask to yolo"
                    attached.send("y")
                attached.expect("SAME-TURN-RUNNING")
                assert len(provider.requests) == 2
                assert before == (1 if permission else 2)
                rival = Term([TNY, "agents"], env, str(ws))
                try:
                    rival.expect("Background agents")
                    rival.send("\r")
                    rival.expect("Saved read-only")
                    rival.expect("perform fixture")
                    before_rival = snapshot_state(home)
                    for attempt in ("/continue\r", "RIVAL-NOT-SENT\r"):
                        rival.send(attempt)
                        rival.expect_next("Still read-only: owner unavailable")
                        assert len(provider.requests) == 2
                        assert snapshot_state(home) == before_rival
                        assert_writer_and_listener_retained()
                    if not (permission or steer or images or transformed):
                        attached.send("/agents\r")
                        attached.expect_next("Background agents")
                        # Retry only after an explicit response. The detached
                        # socket may still be queued in the runner's poll loop.
                        continue_owner(rival, session.parent.name)
                        rival.expect_next("SAME-TURN-RUNNING")
                        assert len(provider.requests) == 2
                        assert_writer_and_listener_retained()
                        rival.send("/quit\r")
                        assert rival.wait() == 0
                        attached.send("\r")
                        attached.expect_next("Attached")
                    else:
                        rival.send("/quit\r")
                        assert rival.wait() == 0
                finally:
                    rival.close()
                # An attached running background turn returns immediately to
                # the list for both commands, without a second restart.
                for detach in ("/agents\r", "\x18"):
                    attached.send(detach)
                    attached.expect_next("Background agents")
                    assert_clean_dashboard(attached, "Attached", "SAME-TURN-RUNNING")
                    assert int((session.parent / "pid").read_text()) == pid
                    attached.send("\r")
                    attached.expect_next("Attached")
                provider.finish.set()
                attached.expect("FINISHED-ONCE")
                until(
                    lambda: json.loads(session.read_text()).get("status") == "done",
                    attached,
                )
                final = json.loads(session.read_text())
                assert final["turns"] == 1, final
                assert final["result"]["steps"] == 2, final
                assert final["result"]["output"].count("FINISHED-ONCE") == 1, final
                assert "fixture-secret" not in session.read_text()
                listed = subprocess.run(
                    [TNY, "agents", "--json"],
                    env=env,
                    cwd=ws,
                    capture_output=True,
                    text=True,
                    timeout=5,
                )
                live_row = json.loads(listed.stdout)["agents"][0]
                assert (
                    live_row["status"] == "done"
                    and not live_row["running"]
                    and live_row["live"]
                ), live_row
                if transformed:
                    records = [
                        json.loads(line)
                        for line in Path(env["EVENTS_PATH"]).read_text().splitlines()
                    ]
                    assert all(r["initialized"] for r in records), records
                    assert (
                        len(
                            [
                                r
                                for r in records
                                if r["type"] == "session_start"
                                and r["reason"] == "background_resume"
                            ]
                        )
                        == 1
                    ), records
                    events = [
                        json.loads(line)["type"]
                        for line in Path(env["EVENTS_PATH"]).read_text().splitlines()
                    ]
                    assert events.count("user_prompt_submit") == 1, events
                    assert events.count("session_start") == 2, events
                attached.send("FOLLOWUP-AFTER-DONE\r")
                attached.expect("LIVE-FOLLOWUP-OK")
                until(
                    lambda: json.loads(session.read_text()).get("turns") == 2, attached
                )
                final = json.loads(session.read_text())
                attached.send("\x04")
                assert attached.wait() == 0

                # The same list keeps completed rows; a stale marker is never
                # presented as running once its writer is actually free.
                def free_writer():
                    with (session.parent / "lock").open() as lock:
                        try:
                            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        except BlockingIOError:
                            return False
                        return True

                until(free_writer)
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
            else:
                assert saved["status"] == "done", saved
                term.send("q")
                assert term.wait() == 0
            assert not provider.errors, provider.errors
        finally:
            provider.close()
            for t in (term, attached):
                if t:
                    t.close()
            if session is None:
                session = next(
                    (Path(home) / ".tny/sessions").glob("*/*/session.json"), None
                )
            if session and (session.parent / "pid").exists():
                subprocess.run(
                    [TNY, "session", "stop", session.parent.name, "--kill"],
                    env=env,
                    cwd=ws,
                    capture_output=True,
                    timeout=12,
                )
    print(
        "PASS background agents",
        "no-tools"
        if no_tools
        else "permission"
        if permission
        else "steer"
        if steer
        else "multi-tool restart/reattach",
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


def saved_fixture(home, ws, backend="fixture", checkpoint=False):
    # Match the physical cwd used by tny, including macOS /var -> /private/var.
    ws = ws.resolve()
    value = 0xCBF29CE484222325
    for byte in str(ws).encode():
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    sid = "1234567890abcdef"
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

    def __init__(self):
        self.requests = []
        self.refreshes = []
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
                event = {
                    "choices": [
                        {
                            "delta": {"content": f"SAVED-TURN-{len(fixture.requests)}"},
                            "finish_reason": "stop",
                        }
                    ]
                }
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
            term.expect("Background agents")
            term.send("\r")
            term.expect("Saved read-only " + initial["id"])
            term.expect("SAVED-TURN-1")
            assert not writer_live(session)
            assert snapshot_state(home) == before
            assert len(provider.requests) == 1
            term.send("SECOND\r")
            term.expect("SAVED-TURN-2")
            until(lambda: json.loads(session.read_text()).get("turns") == 2, term)
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
            term.expect("Background agents")
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
            term.expect("Background agents")
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
            until(lambda: json.loads(session.read_text()).get("turns") == 3, term)
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
            term.expect("Background agents")
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
            until(lambda: json.loads(session.read_text()).get("turns") == 2, term)
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
            term.expect("Background agents")
            term.send("\r")
            term.expect("Saved read-only")
            term.send("BEFORE-RUNNER-DEATH\r")
            term.expect("SAVED-TURN-1")
            until(lambda: json.loads(session.read_text()).get("turns") == 2, term)
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
                    term.expect("Background agents")
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
                    term.expect("Background agents")
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


def cancellation_before_boundary():
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
            term.send("\x05\x15")  # end then clear draft; empty-composer Left arms
            term.send("\x1b[D")
            term.expect("Background armed")
            term.send("\x03")
            until(
                lambda: json.loads(session.read_text()).get("status") == "interrupted",
                term,
                8,
            )
            saved = json.loads(session.read_text())
            assert not saved.get("background"), saved
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
    print("PASS active cancellation beats armed handoff; idle Left edits")


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
                term.expect_next(
                    "background handoff requires a saved native session runner"
                )
                assert "Background agents" not in term.buf
                assert "\x1b[2J" not in term.buf, "refused handoff cleared the chat"
            provider.finish.set()
            term.expect("FINISHED-ONCE")
            term.send("/agents\r")
            term.expect("No background sessions")
            term.send("q")
            assert term.wait() == 0
            assert not provider.errors, provider.errors
            session = next((Path(home) / ".tny/sessions").glob("*/*/session.json"))
            assert not json.loads(session.read_text()).get("background")
        finally:
            provider.close()
            term.close()
    print("PASS in-process Left/CtrlX/agents reject safely and preserve active turn")


def assert_clean_dashboard(term, *old_text):
    term.expect_on_screen("Background agents")
    screen = term.screen()
    assert screen.splitlines()[0].startswith("Background agents"), screen
    for text in old_text:
        assert text not in screen, screen
    assert "\x1b[2J" in term.buf and "\x1b[3J" in term.buf, term.buf
    clears = term.buf.count("\x1b[2J")
    paints = term.buf.count("Background agents")
    # Observe a repaint, not a fixed sleep: idle polling can delay the 500 ms
    # refresh. It must neither clear again nor restore old chat text.
    until(lambda: term.buf.count("Background agents") > paints, term, seconds=5)
    assert term.buf.count("\x1b[2J") == clears, term.buf
    screen = term.screen()
    assert screen.splitlines()[0].startswith("Background agents"), screen
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
                    term.expect("No background sessions")
                    assert_clean_dashboard(term, "OLD-SHELL-TEXT", BANNER)
                    assert not (Path(home) / ".tny/sessions").exists(), (
                        "dashboard prewarmed a runner"
                    )
                    term.send("q")
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
                assert result.stdout == "No background sessions in this workspace.\n"
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
    run_case(restart_failure=True)
    run_case(images=True)
    run_case(transformed=True)
    run_case(restart_fault="post-go")
    run_case(restart_fault="post-run")
    run_case(permission=True, restart_fault="post-run")

    cancellation_before_boundary()
    empty_dashboard()

    unattended_permission()

    unsupported_in_process()
