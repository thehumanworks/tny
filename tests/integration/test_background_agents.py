#!/usr/bin/env python3
"""Real PTY/runner fixture: completed-tool checkpoint, exec and same-turn attach."""

import base64
import fcntl
import json
import os
import shlex
import shutil
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from test_tui import BANNER, TNY, Term, base_env

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
                attached = Term(
                    [TNY, "--provider", "openai", "resume", session.parent.name],
                    env,
                    str(ws),
                )
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
            saved = json.loads(session.read_text())
            assert saved["background"] is True, saved
            if not no_tools:
                pid = int((session.parent / "pid").read_text())
                assert pid != old_pid, (old_pid, pid)
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
                    rival.expect("cannot reattach")
                    rival.send("q")
                    assert rival.wait() == 0
                finally:
                    rival.close()
                # An attached running background turn returns immediately to
                # the list for both commands, without a second restart.
                for detach in ("/agents\r", "\x18"):
                    attached.send(detach)
                    attached.expect_next("Background agents")
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
            provider.finish.set()
            term.expect("FINISHED-ONCE")
            term.send("\x04")
            assert term.wait() == 0
            assert not provider.errors, provider.errors
            session = next((Path(home) / ".tny/sessions").glob("*/*/session.json"))
            assert not json.loads(session.read_text()).get("background")
        finally:
            provider.close()
            term.close()
    print("PASS in-process Left/CtrlX/agents reject safely and preserve active turn")


def empty_dashboard():
    with tempfile.TemporaryDirectory(prefix="tny-agents-empty-") as home:
        env = base_env(
            home, {"OPENAI_BASE_URL": "http://127.0.0.1:1", "OPENAI_API_KEY": "fixture"}
        )
        term = Term([TNY, "agents"], env, home)
        try:
            term.expect("No background sessions")
            assert not (Path(home) / ".tny/sessions").exists(), (
                "dashboard prewarmed a runner"
            )
            term.send("q")
            assert term.wait() == 0
        finally:
            term.close()
    print("PASS empty dashboard without provider work")


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


def completed_host_dashboard():
    agent = Path(__file__).resolve().parent / "fake_acp_agent.py"
    with tempfile.TemporaryDirectory(prefix="tny-host-agents-") as home:
        state = Path(home) / "acp-state.json"
        env = base_env(
            home,
            {
                "FAKE_ACP_STATE": str(state),
                "OPENAI_BASE_URL": "http://127.0.0.1:1/v1",
                "OPENAI_API_KEY": "fixture",
            },
        )
        launched = subprocess.run(
            [
                TNY,
                "--provider",
                "acp",
                "--agent",
                str(agent),
                "ask",
                "-B",
                "original host",
            ],
            env=env,
            cwd=home,
            capture_output=True,
            text=True,
            timeout=10,
        )
        assert launched.returncode == 0, launched.stderr
        session = next((Path(home) / ".tny/sessions").glob("*/*/session.json"))
        term = None
        try:
            until(lambda: json.loads(session.read_text()).get("status") == "done")
            until(lambda: not writer_live(session))
            term = Term([TNY, "--agent", str(agent), "agents"], env, home)
            term.expect("Background agents")
            term.send("\r")
            term.expect("Attached")
            term.send("HOST-FOLLOWUP\r")
            term.expect("[asked: HOST-FOLLOWUP]", timeout=15)
            until(
                lambda: (
                    json.loads(state.read_text()).get("last_prompt") == "HOST-FOLLOWUP"
                ),
                term,
            )
            assert "connect failed" not in term.buf
            term.send("\x04")
            assert term.wait() == 0
        finally:
            if term:
                term.close()
            subprocess.run(
                [TNY, "session", "stop", session.parent.name, "--kill"],
                env=env,
                cwd=home,
                capture_output=True,
                timeout=12,
            )
    print("PASS completed host dashboard followup preserves ACP backend")


if __name__ == "__main__":
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
    completed_host_dashboard()

    unsupported_in_process()
