#!/usr/bin/env python3
"""Durable ask/image jobs (#124), end to end — docs/jobs.md, docs/adr/0093.

The exact C124 command is

    env -u TNY_TOOLS python3 tests/integration/test_jobs.py <absolute build/tny>

and `tests/integration/run.sh` appends $TNY the same way. Every provider call
is a local HTTP fixture under a throwaway HOME with fake credentials; every
process assertion observes real detached `tny` children with `ps` and
`os.kill(pid, 0)`. Nothing here mocks the job supervisor, the child processes
or the advisory locks — those are the things under test.

With TNY_TEST_EXPECT_WASM=1 only the documented unsupported-execution cases
run: the browser build cannot own a child process.
"""

from __future__ import annotations

import base64
import fcntl
import json
import os
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

MSYS = os.name == "posix" and os.uname().sysname.startswith(("MSYS", "CYGWIN"))

ROOT = Path(__file__).resolve().parents[2]
TNY = str(Path(os.environ.get("TNY", ROOT / "build/tny")).resolve())
WASM = bool(os.environ.get("TNY_TEST_EXPECT_WASM")) or "wasm" in TNY
TOKEN = "fixture-image-token-not-real"
ACCOUNT = "fixture-image-account"
API_KEY = "fixture-chat-key-not-real"
# A second, explicitly selected image account: distinct from the ambient one so
# "the ask child saw nothing" cannot pass by accident. Both are fixtures.
FLAG_TOKEN = "fixture-flag-image-token-not-real"
FLAG_ACCOUNT = "fixture-flag-image-account"
TERMINAL = ("succeeded", "failed", "cancelled", "interrupted")
# Carriers that belong to exactly one side of the chat/image split. The image
# allowance is a ChatGPT account; the chat side here is an OpenAI-compatible
# key. Both values are fixtures — no test reads a real credential.
IMAGE_ONLY_ENV = ("CHATGPT_ACCESS_TOKEN", "CHATGPT_ACCOUNT_ID", "TNY_CODEX_BASE_URL")
CHAT_ONLY_ENV = ("OPENAI_API_KEY", "OPENAI_BASE_URL", "OPENAI_WIRE_API")
# The only names a child is ever asked about. Nothing else about the machine
# running this suite is read, written to a file or printed.
WATCHED_ENV = (
    IMAGE_ONLY_ENV
    + CHAT_ONLY_ENV
    + (
        "DECLARED_TOOL_TOKEN",
        "TNY_JOB_API_KEY",
        "TNY_JOB_BASE_URL",
        "TNY_NESTED",
        "TNY_NESTED_MODE",
        "TNY_TOOLS",
    )
)


def png(width: int, height: int) -> bytes:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    rows = b"".join(b"\0" + b"\x20\x60\xa0" * width for _ in range(height))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">II5B", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows, 1))
        + chunk(b"IEND", b"")
    )


class Handler(BaseHTTPRequestHandler):
    """Chat and image fixture. Behaviour is chosen by the prompt text."""

    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def reply(self, status, content_type, body):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def enter(self, prompt):
        state = self.server.state
        with self.server.lock:
            state["requests"].append(prompt)
            state["active"] += 1
            state["peak"] = max(state["peak"], state["active"])

    def leave(self):
        with self.server.lock:
            self.server.state["active"] -= 1

    def note_headers(self, kind, prompt):
        """What the child actually put on the wire, never the real process
        environment: the resolved authorization of this exact request."""
        with self.server.lock:
            self.server.state["headers"].append(
                {
                    "kind": kind,
                    "path": self.path,
                    "prompt": prompt,
                    "authorization": self.headers.get("Authorization"),
                    "account": self.headers.get("chatgpt-account-id"),
                }
            )

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) or b"{}"
        if self.path.startswith("/backend-api/codex/images/"):
            self.images(json.loads(raw))
            return
        body = json.loads(raw)
        self.server.state["bodies"].append(body)
        if self.path.endswith("/responses"):
            self.codex_chat(body)
            return
        prompt = " ".join(
            str(m.get("content"))
            for m in body.get("messages", [])
            if m.get("role") == "user" and isinstance(m.get("content"), str)
        )
        tool_results = [m for m in body.get("messages", []) if m.get("role") == "tool"]
        self.note_headers("chat", prompt)
        self.enter(prompt)
        try:
            self.chat(prompt, bool(tool_results))
        finally:
            self.leave()

    def codex_chat(self, body):
        """The Codex chat wire (Responses). Its credential is the ChatGPT
        account, which is exactly what an ask job on `--provider codex` must
        still receive."""
        prompt = ""
        for item in body.get("input") or []:
            if isinstance(item, dict) and item.get("role") == "user":
                content = item.get("content")
                if isinstance(content, str):
                    prompt += content
                elif isinstance(content, list):
                    prompt += " ".join(
                        str(part.get("text", ""))
                        for part in content
                        if isinstance(part, dict)
                    )
        self.note_headers("codex-chat", prompt)
        self.enter("codex:" + prompt)
        try:
            if "FAIL" in prompt and self.server.state["fail_ask"]:
                self.reply(
                    500, "application/json", b'{"error":{"message":"fixture refuses"}}'
                )
                return
            tokens = self.server.state.get("chat_tokens", [TOKEN])
            if self.headers.get("Authorization") not in [
                f"Bearer {token}" for token in tokens
            ]:
                self.reply(401, "application/json", b'{"error":{"message":"no token"}}')
                return
            if self.headers.get("chatgpt-account-id") != ACCOUNT:
                self.reply(
                    401, "application/json", b'{"error":{"message":"no account"}}'
                )
                return
            text = "codex-answer:" + prompt[:40]
            events = [
                {"type": "response.created", "response": {"status": "in_progress"}},
                {
                    "type": "response.output_item.added",
                    "output_index": 0,
                    "item": {
                        "type": "message",
                        "id": "msg_1",
                        "role": "assistant",
                        "content": [],
                    },
                },
                {
                    "type": "response.output_text.delta",
                    "item_id": "msg_1",
                    "output_index": 0,
                    "delta": text,
                },
                {
                    "type": "response.output_text.done",
                    "item_id": "msg_1",
                    "output_index": 0,
                    "text": text,
                },
                {
                    "type": "response.completed",
                    "response": {
                        "status": "completed",
                        "usage": {"input_tokens": 3, "output_tokens": 1},
                    },
                },
            ]
            data = b"".join(
                f"event: {e['type']}\ndata: {json.dumps(e)}\n\n".encode()
                for e in events
            )
            self.reply(200, "text/event-stream", data)
        finally:
            self.leave()

    def chat(self, prompt, after_tool=False):
        state = self.server.state
        if "DAG_BARRIER" in prompt and ("ENVDUMP" not in prompt or after_tool):
            state["dag_entered"].set()
            if not state["dag_release"].wait(timeout=60):
                self.reply(500, "application/json", b"{}")
                return
        if "HOLD" in prompt:
            time.sleep(state["hold"])
        if "FAIL" in prompt and state["fail_ask"]:
            self.reply(
                500, "application/json", b'{"error":{"message":"fixture refuses"}}'
            )
            return
        if not after_tool and (
            ("ENVDUMP" in prompt and state["envdump"]) or "MEMBER_CONTROL" in prompt
        ):
            # A real tool call: the item child runs it, so the file it writes
            # is that child's own environment, captured from the inside.
            call = {
                "index": 0,
                "id": "call_envdump",
                "type": "function",
                "function": {
                    "name": "terminal",
                    "arguments": json.dumps({"command": state["envdump"]}),
                },
            }
            if "MEMBER_CONTROL" in prompt:
                call["function"] = {
                    "name": "job_control",
                    "arguments": json.dumps(
                        {"action": "cancel", "id": state["member_target"]}
                    ),
                }
            frames = [
                {"choices": [{"index": 0, "delta": {"tool_calls": [call]}}]},
                {
                    "choices": [
                        {"index": 0, "delta": {}, "finish_reason": "tool_calls"}
                    ],
                    "usage": {"prompt_tokens": 3, "completion_tokens": 1},
                },
            ]
            data = (
                "".join(f"data: {json.dumps(f)}\n\n" for f in frames)
                + "data: [DONE]\n\n"
            ).encode()
            self.reply(200, "text/event-stream", data)
            return
        if "FLOOD" in prompt:
            chunks = ["x" * 65536] * state["flood_chunks"]
        elif "SECRET" in prompt:
            chunks = ["a fixed answer with no prompt text"]
        else:
            chunks = ["answer:" + prompt[:40]]
        frames = [{"choices": [{"index": 0, "delta": {"content": c}}]} for c in chunks]
        frames.append(
            {
                "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                "usage": {"prompt_tokens": 3, "completion_tokens": 1},
            }
        )
        if "NO_USAGE" in prompt:
            frames[-1].pop("usage", None)
        data = (
            "".join(f"data: {json.dumps(f)}\n\n" for f in frames) + "data: [DONE]\n\n"
        ).encode()
        self.reply(200, "text/event-stream", data)

    def images(self, body):
        state = self.server.state
        prompt = str(body.get("prompt", ""))
        self.note_headers("image", prompt)
        self.enter("image:" + prompt)
        try:
            accepted = [
                (f"Bearer {token}", account) for token, account in state["image_auth"]
            ]
            if (
                self.headers.get("Authorization"),
                self.headers.get("chatgpt-account-id"),
            ) not in accepted:
                self.reply(401, "application/json", b"{}")
                return
            if "HOLD" in prompt:
                time.sleep(state["hold"])
            if "FAIL" in prompt and state["fail_image"]:
                self.reply(500, "application/json", b'{"error":{"message":"no"}}')
                return
            payload = base64.b64encode(state["image"]).decode()
            self.reply(
                200,
                "application/json",
                json.dumps({"created": 1, "data": [{"b64_json": payload}]}).encode(),
            )
        finally:
            self.leave()


class QuietServer(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        kind = sys.exc_info()[0]
        if kind is not None and issubclass(
            kind, (BrokenPipeError, ConnectionResetError)
        ):
            return
        super().handle_error(request, client_address)


def running(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def pids_argv(*needles: str) -> list[str]:
    """The command lines of live processes matching every needle."""
    out = subprocess.run(
        ["ps", "-eo", "pid=,args="], capture_output=True, text=True, timeout=30
    ).stdout
    return [
        line.strip()
        for line in out.splitlines()
        if all(needle in line for needle in needles) and "ps -eo" not in line
    ]


def pids_matching(*needles: str) -> list[int]:
    """Live pids whose argv contains every needle. Scoped by the caller to one
    test's throwaway HOME so a neighbouring case's child is never mistaken for
    this one's."""
    out = subprocess.run(
        ["ps", "-eo", "pid=,args="], capture_output=True, text=True, timeout=30
    ).stdout
    found = []
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        pid, _, args = line.partition(" ")
        if all(needle in args for needle in needles) and "ps -eo" not in args:
            try:
                found.append(int(pid))
            except ValueError:
                pass
    return found


class JobsFixture(unittest.TestCase):
    """Throwaway HOME, loopback provider, real detached children."""

    hold = 2.0

    def setUp(self):
        if WASM:
            self.skipTest("native job execution is not available in this build")
        self.tmp = tempfile.TemporaryDirectory(prefix="tny-jobs-")
        # Confined mailbox readers require a canonical root; /var is a macOS symlink.
        self.home = Path(self.tmp.name).resolve()
        self.workspace = self.home / "ws"
        self.workspace.mkdir()
        (self.home / "codex").mkdir()
        self.state = {
            "requests": [],
            "bodies": [],
            "headers": [],
            "active": 0,
            "peak": 0,
            "hold": self.hold,
            "fail_ask": True,
            "fail_image": True,
            "flood_chunks": 80,
            "envdump": None,
            "image_auth": [(TOKEN, ACCOUNT)],
            "image": png(8, 8),
        }
        self.server = QuietServer(("127.0.0.1", 0), Handler)
        self.server.state = self.state
        self.server.lock = threading.Lock()
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        url = f"http://127.0.0.1:{self.server.server_port}"
        env = {
            k: v
            for k, v in os.environ.items()
            if not k.endswith("_API_KEY") and not k.endswith("_BASE_URL")
        }
        env.pop("TNY_TOOLS", None)
        env.update(
            {
                "HOME": str(self.home),
                "CODEX_HOME": str(self.home / "codex"),
                "OPENAI_API_KEY": API_KEY,
                "OPENAI_BASE_URL": url + "/v1",
                "OPENAI_WIRE_API": "chat",
                "OPENAI_DEFAULT_MODEL": "mock-model",
                "CHATGPT_ACCESS_TOKEN": TOKEN,
                "CHATGPT_ACCOUNT_ID": ACCOUNT,
                "TNY_CODEX_BASE_URL": url + "/backend-api/codex",
                "TNY_TEST_SUITE": "jobs",
            }
        )
        self.env = env
        self.started: list[subprocess.Popen] = []

    def tearDown(self):
        for job in self.job_dirs():
            record = self.record_at(job)
            if record and record.get("state") not in TERMINAL:
                fence = (
                    ["--expected-attempt", str(record["attempt"])]
                    if record.get("dag")
                    else []
                )
                self.run_tny("jobs", "cancel", job.name, *fence, check=False)
        for process in self.started:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        for pid in pids_matching("jobs _worker", self.home.name):
            try:
                os.kill(pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
        self.server.shutdown()
        self.server.server_close()
        self.tmp.cleanup()

    # ---------------------------------------------------------- helpers

    def run_tny(
        self, *args, stdin=b"", timeout=120, check=True, env=None, provider="openai"
    ):
        run = subprocess.run(
            [TNY, "--provider", provider, *args],
            cwd=self.workspace,
            env=env or self.env,
            input=stdin,
            capture_output=True,
            timeout=timeout,
        )
        if check:
            self.assertIn(run.returncode, (0,), run.stderr.decode()[-2000:])
        return run

    def submit(self, *args, stdin=b"", check=True):
        run = self.run_tny("jobs", "submit", *args, "--json", stdin=stdin, check=check)
        payload = json.loads(run.stdout.decode()) if run.stdout.strip() else {}
        return run, payload

    def status(self, job_id, check_state=None):
        run = self.run_tny("jobs", "status", job_id, "--json", check=False)
        record = json.loads(run.stdout.decode())
        if check_state:
            self.assertEqual(record["state"], check_state, record)
        return record

    def await_state(self, job_id, predicate, timeout=90, what="state"):
        deadline = time.time() + timeout
        record = None
        while time.time() < deadline:
            record = self.status(job_id)
            if predicate(record):
                return record
            time.sleep(0.2)
        self.fail(f"timed out waiting for {what}: {json.dumps(record, indent=1)}")

    def await_terminal(self, job_id, timeout=90):
        return self.await_state(
            job_id, lambda r: r["state"] in TERMINAL, timeout, "a terminal state"
        )

    def jobs_root(self):
        return self.home / ".tny" / "jobs"

    def job_dirs(self):
        root = self.jobs_root()
        if not root.is_dir():
            return []
        return [p for p in sorted(root.iterdir()) if p.is_dir() and len(p.name) == 32]

    def record_at(self, directory: Path):
        path = directory / "job.json"
        if not path.is_file():
            return None
        try:
            return json.loads(path.read_text())
        except json.JSONDecodeError:
            return None

    def startup_diagnostics(self, record):
        """Bounded local engine diagnostics, not provider bodies or credentials."""
        found = []
        for item in record.get("items", []):
            path = Path(item["log_path"])
            if not path.exists() or not path.resolve().is_relative_to(
                self.home.resolve()
            ):
                continue
            for line in path.read_text(errors="replace")[
                : 4 * 1024 * 1024
            ].splitlines():
                try:
                    event = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if event.get("type") == "error":
                    text = event.get("text", "")
                    safe = (
                        text[:200]
                        if text.startswith(
                            ("MAILBOX_", "could not persist team", "the job supervisor")
                        )
                        else "provider/runner error"
                    )
                    found.append((item["index"], event.get("error_code"), safe))
        return found

    def ask_requests(self):
        return [r for r in self.state["requests"] if not r.startswith("image:")]

    def image_requests(self):
        return [r for r in self.state["requests"] if r.startswith("image:")]

    def headers_of(self, kind):
        return [h for h in self.state["headers"] if h["kind"] == kind]

    def child_environment(self, *argv, stdin=b"", timeout=120):
        """Run one ask item that reports its own carriers from inside the
        child, and return them as a dict.

        Only the fixture's own carrier names are ever selected — the filter
        runs in the child, so no unrelated value from the surrounding machine
        is written anywhere or shown in a failure message.
        """
        dump = self.home / "child-env.txt"
        names = "|".join(WATCHED_ENV)
        self.state["envdump"] = f"env | grep -E '^({names})=' > {dump}; true"
        env = dict(self.env, TNY_TOOLS="all")
        run = self.run_tny(*argv, "--json", stdin=stdin, env=env)
        payload = json.loads(run.stdout.decode())
        record = self.await_terminal(payload["id"], timeout=timeout)
        self.assertTrue(dump.is_file(), f"the child never ran the report: {record}")
        captured = {}
        for line in dump.read_text(errors="replace").splitlines():
            name, sep, value = line.partition("=")
            if sep and name in WATCHED_ENV:
                captured[name] = value
        return captured, record

    def lock_held(self, path: Path):
        """Hold a real advisory lock on one of the job's own lock files, so
        two contenders meet inside the production transaction instead of
        racing process startup."""
        handle = open(path, "a+b")
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        return handle

    def spawn_tny(self, *args, env=None):
        return subprocess.Popen(
            [TNY, "--provider", "openai", *args],
            cwd=self.workspace,
            env=env or self.env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def worker_pid(self, job_id, timeout=20):
        deadline = time.time() + timeout
        while time.time() < deadline:
            found = pids_matching("jobs _worker", job_id)
            if found:
                return found[0]
            time.sleep(0.1)
        self.fail(f"no supervisor process for job {job_id}")

    def item_child_pid(self, job_id, timeout=30):
        """The item's own `tny ask`/`tny image` child, found by its workspace."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            record = self.status(job_id)
            if record["items"][0]["state"] == "running":
                found = pids_matching("--events=jsonl", self.home.name)
                if found:
                    return found[0]
            time.sleep(0.1)
        self.fail("no item child process appeared")

    def start_sentinel(self):
        """An unrelated process that actually reacts to the signals a job
        cancellation uses. The test runner may have inherited an ignored
        SIGINT, and a child would inherit that too — which would make a
        survival assertion vacuous — so the disposition is reset before exec."""

        def defaults():
            for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                signal.signal(sig, signal.SIG_DFL)

        sentinel = subprocess.Popen(
            ["sleep", "120"], start_new_session=True, preexec_fn=defaults
        )
        self.started.append(sentinel)
        return sentinel

    def batch_request(self, kind, items, concurrency=2):
        path = self.home / f"batch-{len(self.state['requests'])}.json"
        path.write_text(
            json.dumps({"kind": kind, "concurrency": concurrency, "items": items})
        )
        return str(path)


class JobsSubmitAndStatus(JobsFixture):
    def test_submit_returns_a_durable_id_and_paths_before_the_item_finishes(self):
        started = time.time()
        run, payload = self.submit("ask", stdin=b"HOLD one")
        elapsed = time.time() - started
        self.assertLess(elapsed, self.hold, "submit waited for the item to finish")
        self.assertRegex(payload["id"], r"^[0-9a-f]{32}$")
        self.assertIn(payload["state"], ("queued", "running"))
        metadata = Path(payload["metadata_path"])
        self.assertTrue(metadata.is_file(), payload)
        if not MSYS:  # NTFS ACLs do not map to POSIX mode bits
            self.assertEqual(oct(metadata.stat().st_mode & 0o777), "0o600")
            self.assertEqual(oct(metadata.parent.stat().st_mode & 0o777), "0o700")
        log = Path(payload["items"][0]["log_path"])
        self.assertEqual(log.parent, metadata.parent)
        record = self.await_terminal(payload["id"])
        self.assertEqual(record["state"], "succeeded", record)
        item = record["items"][0]
        self.assertEqual(item["exit_code"], 0)
        self.assertRegex(item["session_id"], r"^[0-9a-f]{16}$")
        self.assertRegex(item["result_sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(record["cleanup"], "complete")
        self.assertEqual(len(self.ask_requests()), 1)
        # The canonical ask event stream is the log, verbatim: one JSON object
        # per line with no job envelope of any kind (#66 / ADR 0090).
        lines = [json.loads(line) for line in log.read_text().splitlines() if line]
        self.assertTrue(lines)
        for event in lines:
            self.assertEqual(event["schema_version"], 1)
            self.assertNotIn("job", event)
            self.assertNotIn("job_id", event)
        self.assertEqual(
            [e for e in lines if e["type"] == "turn_end"][0]["stop_reason"], 0
        )

    def test_the_job_outlives_its_submitters_whole_process_group(self):
        process = subprocess.Popen(
            [TNY, "--provider", "openai", "jobs", "submit", "ask", "--json"],
            cwd=self.workspace,
            env=self.env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
        self.started.append(process)
        out, err = process.communicate(b"HOLD submitter dies", timeout=60)
        payload = json.loads(out.decode())
        # Tear down the submitter's entire process group while the item runs.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        record = self.await_terminal(payload["id"])
        self.assertEqual(record["state"], "succeeded", (record, err.decode()))
        self.assertEqual(record["items"][0]["exit_code"], 0)

    def test_a_mixed_batch_keeps_per_item_outcomes_independent(self):
        request = self.batch_request(
            "ask",
            [{"prompt": "first one"}, {"prompt": "FAIL this one"}, {"prompt": "third"}],
            concurrency=2,
        )
        _run, payload = self.submit("batch", "--request", request)
        record = self.await_terminal(payload["id"])
        self.assertEqual(record["state"], "failed", record)
        states = [item["state"] for item in record["items"]]
        self.assertEqual(states, ["succeeded", "failed", "succeeded"])
        self.assertIsNotNone(record["items"][0]["session_id"])
        self.assertIsNone(record["items"][1]["session_id"])
        self.assertIsNotNone(record["items"][1]["error"])
        self.assertNotIn(API_KEY, json.dumps(record))

    def test_unknown_and_traversing_ids_are_refused(self):
        for bad in ("../../etc", "not-hex", "0" * 31, "0" * 33, "A" * 32):
            run = self.run_tny("jobs", "status", bad, "--json", check=False)
            self.assertEqual(run.returncode, 1, (bad, run.stdout, run.stderr))
            self.assertNotIn(b"traceback", run.stderr.lower())
        run = self.run_tny("jobs", "status", "0" * 32, "--json", check=False)
        self.assertEqual(run.returncode, 1)
        self.assertIn(b"no job", run.stderr)

    def test_unsupported_and_malformed_records_fail_closed(self):
        _run, payload = self.submit("ask", stdin=b"quick one")
        self.await_terminal(payload["id"])
        metadata = Path(payload["metadata_path"])
        record = json.loads(metadata.read_text())

        record["version"] = 2
        metadata.write_text(json.dumps(record))
        run = self.run_tny("jobs", "status", payload["id"], "--json", check=False)
        self.assertEqual(run.returncode, 1, run.stdout)
        self.assertIn(b"version 1", run.stderr)

        metadata.write_text("{not json")
        run = self.run_tny("jobs", "status", payload["id"], "--json", check=False)
        self.assertEqual(run.returncode, 1)

        record["version"] = 1
        record["state"] = "teleported"
        metadata.write_text(json.dumps(record))
        run = self.run_tny("jobs", "status", payload["id"], "--json", check=False)
        self.assertEqual(run.returncode, 1)

    def test_list_and_logs_are_bounded_and_private(self):
        _run, payload = self.submit("ask", stdin=b"list me")
        self.await_terminal(payload["id"])
        run = self.run_tny("jobs", "list", "--json")
        listing = json.loads(run.stdout.decode())
        self.assertEqual([j["id"] for j in listing["jobs"]], [payload["id"]])
        run = self.run_tny("jobs", "logs", payload["id"], "--max-bytes", "64", "--json")
        logs = json.loads(run.stdout.decode())
        self.assertLessEqual(len(logs["text"]), 64)
        self.assertTrue(logs["truncated"])
        self.assertEqual(
            oct(Path(logs["log_path"]).stat().st_mode & 0o777), "0o600", logs
        )

    def test_no_job_command_writes_anything_but_its_own_result_to_stdout(self):
        """The supervisor's acknowledgment and the item child's stream are
        private: the caller's stdout carries one result and nothing else."""
        _run, payload = self.submit("ask", stdin=b"tidy one")
        job_id = payload["id"]
        self.await_terminal(job_id)
        for args in (
            ("jobs", "status", job_id, "--json"),
            ("jobs", "list", "--json"),
            ("jobs", "logs", job_id, "--json"),
            ("jobs", "wait", job_id, "--timeout", "30", "--json"),
        ):
            run = self.run_tny(*args, check=False)
            text = run.stdout.decode()
            self.assertTrue(text.endswith("\n"), args)
            self.assertEqual(len(text.strip().splitlines()), 1, (args, text[:400]))
            json.loads(text)  # one object: no prologue, no trailer, no noise
        human = self.run_tny("jobs", "status", job_id, check=False)
        body = human.stdout.decode()
        self.assertIn(job_id, body)
        for noise in ("data: {", '{"type":', "answer:", API_KEY, TOKEN, ACCOUNT):
            self.assertNotIn(noise, body, body[:400])


class JobsCancellation(JobsFixture):
    hold = 6.0

    def test_cancelling_a_queued_item_prevents_any_provider_request(self):
        request = self.batch_request(
            "ask",
            [{"prompt": "HOLD first"}, {"prompt": "second never runs"}],
            concurrency=1,
        )
        _run, payload = self.submit("batch", "--request", request)
        job_id = payload["id"]
        self.await_state(
            job_id,
            lambda r: r["items"][0]["state"] == "running",
            what="the first item to start",
        )
        cancel = self.run_tny("jobs", "cancel", job_id, "--items", "1", "--json")
        self.assertEqual(cancel.returncode, 0, cancel.stderr)
        record = self.await_terminal(job_id)
        self.assertEqual(record["items"][1]["state"], "cancelled", record)
        self.assertEqual(record["items"][0]["state"], "succeeded", record)
        # The cancelled item never reached the provider — and never started a
        # process at all: `started` is stamped at the launch claim, and the
        # log file is only created when a child is spawned.
        self.assertIsNone(
            record["items"][1]["started"], "a cancelled item was launched"
        )
        self.assertEqual(record["items"][1]["error"], "cancelled before it started")
        self.assertFalse(
            Path(record["items"][1]["log_path"]).exists(),
            "a cancelled item opened a log",
        )
        self.assertEqual(len(self.ask_requests()), 1, self.ask_requests())
        self.assertNotIn("second never runs", " ".join(self.ask_requests()))

    def test_cancelling_a_running_item_stops_its_tree_and_spares_a_sentinel(self):
        sentinel = self.start_sentinel()
        _run, payload = self.submit("ask", stdin=b"HOLD cancel me")
        job_id = payload["id"]
        child = self.item_child_pid(job_id)
        supervisor = self.worker_pid(job_id)
        self.run_tny("jobs", "cancel", job_id, "--json")
        record = self.await_terminal(job_id, timeout=60)
        self.assertEqual(record["state"], "cancelled", record)
        self.assertEqual(record["items"][0]["state"], "cancelled", record)
        self.assertEqual(record["cleanup"], "complete", record)
        deadline = time.time() + 30
        while time.time() < deadline and (running(child) or running(supervisor)):
            time.sleep(0.2)
        self.assertFalse(running(child), "the owned item child survived cancellation")
        self.assertFalse(running(supervisor), "the supervisor survived its last item")
        self.assertIsNone(sentinel.poll(), "an unrelated process was killed")
        sentinel.kill()

    def test_a_killed_supervisor_reads_as_interrupted_with_unknown_cleanup(self):
        _run, payload = self.submit("ask", stdin=b"HOLD lose the worker")
        job_id = payload["id"]
        child = self.item_child_pid(job_id)
        supervisor = self.worker_pid(job_id)
        os.kill(supervisor, signal.SIGKILL)
        record = self.await_state(
            job_id, lambda r: r["state"] in TERMINAL, timeout=40, what="an honest state"
        )
        self.assertEqual(record["state"], "interrupted", record)
        self.assertEqual(record["cleanup"], "unknown", record)
        self.assertEqual(record["exit_code"], 2, record)
        self.assertEqual(record["items"][0]["state"], "interrupted", record)
        self.assertNotEqual(record["state"], "cancelled")
        # Wait is bounded and reports the same terminal state, not "running".
        run = self.run_tny(
            "jobs", "wait", job_id, "--timeout", "5", "--json", check=False
        )
        self.assertEqual(run.returncode, 2, run.stdout)
        # The orphaned child cooperatively stops: it watches for its actual
        # parent, which is a kernel-maintained relationship.
        deadline = time.time() + 40
        while time.time() < deadline and running(child):
            time.sleep(0.2)
        self.assertFalse(running(child), "the orphaned item child kept running")

    def test_a_forged_pid_in_the_record_never_signals_an_unrelated_process(self):
        sentinel = self.start_sentinel()
        _run, payload = self.submit("ask", stdin=b"HOLD forged pid")
        job_id = payload["id"]
        self.await_state(job_id, lambda r: r["items"][0]["state"] == "running")
        metadata = Path(payload["metadata_path"])
        record = json.loads(metadata.read_text())
        # Nothing in this schema confers authority over a pid; prove it.
        record["pid"] = sentinel.pid
        record["items"][0]["pid"] = sentinel.pid
        metadata.write_text(json.dumps(record))
        child = self.item_child_pid(job_id)
        self.run_tny("jobs", "cancel", job_id, "--json")
        final = self.await_terminal(job_id, timeout=60)
        # poll(), not kill(pid, 0): a signalled child of this test would sit
        # in zombie state and still answer signal 0.
        self.assertIsNone(sentinel.poll(), "a persisted pid was signalled")
        # The cancellation still did its real work on the owned child, so a
        # broken ownership check cannot hide behind an unchanged sentinel.
        self.assertEqual(final["state"], "cancelled", final)
        self.assertEqual(final["cleanup"], "complete", final)
        deadline = time.time() + 20
        while time.time() < deadline and running(child):
            time.sleep(0.2)
        self.assertFalse(running(child), "the owned child outlived the cancellation")
        sentinel.kill()


class JobsParentWatch(JobsFixture):
    """A job child watches for its actual supervisor, not for its pipes."""

    hold = 25.0

    def test_an_orphaned_child_stops_without_waiting_for_the_provider(self):
        _run, payload = self.submit("ask", stdin=b"HOLD a very slow provider")
        job_id = payload["id"]
        child = self.item_child_pid(job_id)
        supervisor = self.worker_pid(job_id)
        os.kill(supervisor, signal.SIGKILL)
        started = time.time()
        deadline = started + 12
        while time.time() < deadline and running(child):
            time.sleep(0.2)
        elapsed = time.time() - started
        self.assertFalse(
            running(child),
            "the orphan waited for the provider instead of noticing its parent",
        )
        # Well inside the fixture's 25 s hold: the child cannot have been
        # stopped by the provider answering.
        self.assertLess(elapsed, 12, elapsed)
        record = self.status(job_id)
        self.assertEqual(record["state"], "interrupted", record)
        self.assertEqual(record["cleanup"], "unknown", record)


class JobsDAG(JobsFixture):
    """Public DAG submission, real worker ownership, barrier-controlled spend."""

    def setUp(self):
        super().setUp()
        self.state["dag_entered"] = threading.Event()
        self.state["dag_release"] = threading.Event()
        self.addCleanup(self.state["dag_release"].set)
        for args in (
            ("init", "-q"),
            (
                "-c",
                "user.name=Fixture",
                "-c",
                "user.email=fixture@invalid",
                "commit",
                "-q",
                "--allow-empty",
                "-m",
                "baseline",
            ),
        ):
            subprocess.run(
                ["git", "-C", str(self.workspace), *args],
                check=True,
                capture_output=True,
            )

    def tearDown(self):
        self.state["dag_release"].set()
        super().tearDown()

    def dag_submit(self, items, **extra):
        request = dict(kind="ask", dag=True, concurrency=3, items=items)
        request.update(extra)
        return self.submit("batch", stdin=json.dumps(request).encode(), check=False)

    def failed_dag(self):
        run, payload = self.dag_submit(
            [
                {"prompt": "DAG_A", "label": "review", "role": "lead"},
                {"prompt": "FAIL DAG_B", "depends_on": [0]},
                {"prompt": "DAG_C", "depends_on": [1]},
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        record = self.await_terminal(payload["id"])
        self.assertEqual(
            [i["state"] for i in record["items"]],
            ["succeeded", "failed", "failed"],
            record,
        )
        self.assertEqual(record["items"][2]["error_code"], "dependency_blocked")
        self.assertFalse(any("DAG_C" in r for r in self.ask_requests()))
        return payload["id"], record

    def test_graph_validation_has_no_spend_or_job_records(self):
        cases = [
            ([{"prompt": "x", "depends_on": [0]}], {}),
            (
                [
                    {"prompt": "x", "depends_on": [1]},
                    {"prompt": "y", "depends_on": [0]},
                ],
                {},
            ),
            (
                [
                    {"prompt": "x"},
                    {"prompt": "y", "depends_on": [0, 0]},
                    {"prompt": "z"},
                ],
                {},
            ),
            ([{"prompt": "x", "depends_on": [-1]}], {}),
            ([{"prompt": "x", "depends_on": [1.0]}, {"prompt": "y"}], {}),
            ([{"prompt": "x", "depends_on": [True]}, {"prompt": "y"}], {}),
            ([{"prompt": "x", "role": "admin"}], {}),
            ([{"prompt": "x", "provider": "cursor"}], {}),
            ([{"prompt": "x", "persist_request": False}], {}),
            ([{"prompt": "x", "depends_on": []}], {"dag": False}),
            ([{"prompt": "x"}], {"dag": "true"}),
            ([{"prompt": "x"}], {"parent_session_id": "a" * 16}),
        ]
        for items, extra in cases:
            with self.subTest(items=items, extra=extra):
                run, _ = self.dag_submit(items, **extra)
                self.assertEqual(run.returncode, 1, run.stdout)
                self.assertEqual(self.job_dirs(), [])
                self.assertEqual(self.ask_requests(), [])

    def test_forward_dependencies_and_unverified_lineage(self):
        run, payload = self.dag_submit(
            [
                {"prompt": "DAG_C", "depends_on": [2]},
                {"prompt": "DAG_A", "label": "review", "role": "lead"},
                {"prompt": "DAG_B", "depends_on": [1]},
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        record = self.await_terminal(payload["id"])
        self.assertEqual(
            record["state"], "succeeded", (record, self.startup_diagnostics(record))
        )
        self.assertEqual(record["run_id"], payload["id"])
        self.assertIsNone(record["parent_session_id"])
        self.assertEqual(record["verification"], "unverified")
        self.assertEqual(len(record["workspace_revision"]), 40)
        self.assertEqual(
            [
                next(x for x in ("DAG_A", "DAG_B", "DAG_C") if x in r)
                for r in self.ask_requests()
            ],
            ["DAG_A", "DAG_B", "DAG_C"],
        )
        for index, item in enumerate(record["items"]):
            self.assertEqual(item["task_id"], index)
            self.assertEqual(item["verification"], "unverified")
            self.assertEqual(len(item["definition_sha256"]), 64)
            self.assertEqual(len(item["dependency_sha256"]), 64)
            self.assertTrue(item["session_id"])

    def test_barrier_cancel_and_explicit_retry_carries_a_once(self):
        run, payload = self.dag_submit(
            [
                {"prompt": "DAG_A"},
                {"prompt": "DAG_BARRIER", "depends_on": [0]},
                {"prompt": "DAG_C", "depends_on": [1]},
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        job_id = payload["id"]
        self.assertTrue(self.state["dag_entered"].wait(30))
        before = self.status(job_id)
        self.assertEqual(before["items"][0]["state"], "succeeded")
        self.assertEqual(before["items"][1]["state"], "running")
        self.assertEqual(before["items"][2]["state"], "queued")
        self.assertFalse(any("DAG_C" in r for r in self.ask_requests()))
        self.run_tny(
            "jobs",
            "cancel",
            job_id,
            "--items",
            "1",
            "--expected-attempt",
            str(before["attempt"]),
            "--json",
        )
        first = self.await_terminal(job_id)
        self.assertEqual(first["items"][1]["state"], "cancelled")
        self.assertEqual(first["items"][2]["error_code"], "dependency_blocked")
        self.assertEqual(first["cleanup"], "complete", first)
        self.state["dag_release"].set()
        self.run_tny("jobs", "retry", job_id, "--json")
        final = self.await_terminal(job_id)
        self.assertEqual(final["state"], "succeeded", final)
        self.assertEqual(
            final["items"][0]["session_id"], before["items"][0]["session_id"]
        )
        self.assertEqual(sum("DAG_A" in r for r in self.ask_requests()), 1)
        self.assertEqual(sum("DAG_C" in r for r in self.ask_requests()), 1)
        self.assertEqual(final["items"][0]["attempt"], 1)
        self.assertEqual(final["items"][1]["attempt"], 2)
        self.assertEqual(final["items"][2]["attempt"], 2)
        snapshot = Path(final["metadata_path"]).parent / "attempt-1.json"
        saved = snapshot.read_bytes()
        again = self.run_tny("jobs", "retry", job_id, "--json", check=False)
        self.assertEqual(again.returncode, 1)
        self.assertEqual(snapshot.read_bytes(), saved)

    def test_supervisor_loss_preserves_a_but_unknown_cleanup_refuses_retry(self):
        run, payload = self.dag_submit(
            [
                {"prompt": "DAG_A"},
                {"prompt": "DAG_BARRIER", "depends_on": [0]},
                {"prompt": "DAG_C", "depends_on": [1]},
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertTrue(self.state["dag_entered"].wait(30))
        job_id = payload["id"]
        os.kill(self.worker_pid(job_id), signal.SIGKILL)
        record = self.await_terminal(job_id)
        self.assertEqual(record["items"][0]["state"], "succeeded")
        self.assertEqual(record["items"][1]["state"], "interrupted")
        self.assertEqual(record["cleanup"], "unknown")
        requests = len(self.ask_requests())
        for _ in range(2):
            run = self.run_tny("jobs", "retry", job_id, "--json", check=False)
            self.assertEqual(run.returncode, 1)
        self.assertEqual(len(self.ask_requests()), requests)
        self.assertEqual(sum("DAG_A" in r for r in self.ask_requests()), 1)
        self.assertFalse(any("DAG_C" in r for r in self.ask_requests()))

    def test_changed_inputs_dependencies_artifacts_and_ceilings_refuse_retry(self):
        job_id, record = self.failed_dag()
        path = Path(record["metadata_path"])
        original = path.read_bytes()
        requests = len(self.ask_requests())
        for change in (
            "definition",
            "dependencies",
            "binding",
            "cleanup",
            "permission",
            "tools",
        ):
            with self.subTest(change=change):
                stored = json.loads(original)
                if change == "definition":
                    stored["items"][1]["request"]["prompt"] = "new work"
                elif change == "dependencies":
                    stored["items"][1]["depends_on"] = []
                elif change == "binding":
                    stored["items"][0]["dependency_sha256"] = "0" * 64
                elif change == "cleanup":
                    stored["cleanup_hold"] = True
                    stored["cleanup"] = "unknown"
                elif change == "permission":
                    stored["permission_ceiling"] = "ask"
                else:
                    stored["tool_ceiling"] = "terminal"
                path.write_text(json.dumps(stored))
                run = self.run_tny("jobs", "retry", job_id, "--json", check=False)
                self.assertNotEqual(run.returncode, 0, run.stdout)
                self.assertEqual(len(self.ask_requests()), requests)
        path.write_bytes(original)
        for flags in (("--permission-mode", "auto"), ("--model", "other-model")):
            with self.subTest(caller_flags=flags):
                run = self.run_tny(
                    *flags, "jobs", "retry", job_id, "--json", check=False
                )
                self.assertEqual(run.returncode, 1, run.stdout)
                self.assertEqual(len(self.ask_requests()), requests)
        log = Path(record["items"][0]["log_path"])
        saved_log = log.read_bytes()
        for deleted in (False, True):
            if deleted:
                log.unlink()
            else:
                log.write_bytes(b"corrupt")
            run = self.run_tny("jobs", "retry", job_id, "--json", check=False)
            self.assertEqual(run.returncode, 2, run.stdout)
            self.assertEqual(len(self.ask_requests()), requests)
            log.write_bytes(saved_log)
        (self.workspace / "dirty").write_text("untracked")
        run = self.run_tny("jobs", "retry", job_id, "--json", check=False)
        self.assertEqual(run.returncode, 1, run.stdout)
        (self.workspace / "dirty").unlink()
        subprocess.run(
            [
                "git",
                "-C",
                str(self.workspace),
                "-c",
                "user.name=Fixture",
                "-c",
                "user.email=fixture@invalid",
                "commit",
                "-q",
                "--allow-empty",
                "-m",
                "changed revision",
            ],
            check=True,
            capture_output=True,
        )
        run = self.run_tny("jobs", "retry", job_id, "--json", check=False)
        self.assertEqual(run.returncode, 1, run.stdout)
        self.assertEqual(len(self.ask_requests()), requests)

    def test_corrupt_dependency_before_launch_blocks_consumer(self):
        run, payload = self.dag_submit(
            [
                {"prompt": "DAG_A"},
                {"prompt": "DAG_BARRIER", "depends_on": [0]},
                {"prompt": "DAG_C", "depends_on": [0, 1]},
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertTrue(self.state["dag_entered"].wait(30))
        record = self.status(payload["id"])
        Path(record["items"][0]["log_path"]).write_bytes(b"corrupt dependency")
        self.state["dag_release"].set()
        final = self.await_terminal(payload["id"])
        self.assertEqual(final["items"][2]["error_code"], "dependency_blocked")
        self.assertFalse(any("DAG_C" in r for r in self.ask_requests()))

    def test_concurrent_retries_have_one_owner_and_immutable_history(self):
        job_id, first = self.failed_dag()
        self.state["fail_ask"] = False
        contenders = [
            self.spawn_tny("jobs", "retry", job_id, "--json") for _ in range(2)
        ]
        outputs = [p.communicate(timeout=90) for p in contenders]
        self.assertEqual(sorted(p.returncode for p in contenders), [0, 1], outputs)
        final = self.await_terminal(job_id)
        self.assertEqual(final["state"], "succeeded", final)
        self.assertEqual(final["attempt"], 2)
        self.assertEqual(sum("DAG_A" in r for r in self.ask_requests()), 1)
        self.assertEqual(sum("DAG_C" in r for r in self.ask_requests()), 1)
        snapshot = Path(first["metadata_path"]).parent / "attempt-1.json"
        self.assertEqual(
            json.loads(snapshot.read_text())["items"][2]["error_code"],
            "dependency_blocked",
        )
        self.assertFalse((snapshot.parent / "attempt-2.json").exists())


class JobsEnrollment(JobsDAG):
    """Scheduler enrollment, not helper-only admission/worktree tests."""

    def enrollment(self, cap=2, claims=100):
        return dict(
            label="fixture",
            provider_scope="public_account",
            cap=cap,
            queue_cap=16,
            claim_limit=claims,
        )

    def admission_state(self):
        path = (
            self.home
            / ".tny"
            / "admission"
            / "fixture"
            / "public_account"
            / "state.json"
        )
        return json.loads(path.read_text())

    def test_independent_batches_share_two_real_launch_slots(self):
        jobs = []
        for batch in range(2):
            run, payload = self.dag_submit(
                [{"prompt": f"DAG_BARRIER batch{batch} task{i}"} for i in range(2)],
                admission=self.enrollment(),
                dag=False,
            )
            self.assertEqual(run.returncode, 0, run.stderr)
            jobs.append(payload["id"])
        self.assertTrue(self.state["dag_entered"].wait(30))
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            records = [self.status(job) for job in jobs]
            if len(self.ask_requests()) == 2 and any(
                item.get("admission_reason") in ("queued_capacity", "queued_fifo")
                for record in records
                for item in record["items"]
            ):
                break
            time.sleep(0.1)
        self.assertEqual(len(self.ask_requests()), 2, records)
        self.assertEqual(self.state["peak"], 2)
        self.assertTrue(
            any(
                item.get("admission_reason") in ("queued_capacity", "queued_fifo")
                for record in records
                for item in record["items"]
            )
        )
        self.state["dag_release"].set()
        for job in jobs:
            record = self.await_terminal(job)
            self.assertEqual(record["state"], "succeeded", record)
            self.assertEqual(record["admission"], self.enrollment())
            self.assertTrue(
                all(i["admission_reason"] == "released" for i in record["items"])
            )
        self.assertEqual(len(self.ask_requests()), 4)
        self.assertLessEqual(self.state["peak"], 2)

    def test_exhaustion_does_not_count_claims_as_token_budget(self):
        run, payload = self.dag_submit(
            [{"prompt": "one"}, {"prompt": "two"}],
            admission=self.enrollment(cap=1, claims=1),
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        record = self.await_terminal(payload["id"])
        self.assertEqual(record["state"], "failed", record)
        self.assertEqual(len(self.ask_requests()), 1)
        self.assertEqual(record["admission"]["claim_limit"], 1)
        failed = next(i for i in record["items"] if i["state"] == "failed")
        self.assertIn("exhausted", failed["error"])
        self.assertFalse(failed["usage_known"])
        self.assertIsNone(failed["usage_input_tokens"])
        self.assertGreater(record["usage"]["unknown_items"], 0)

    def test_cancel_waiting_ticket_and_owner_loss_hold_granted_capacity(self):
        run, first = self.dag_submit(
            [{"prompt": "DAG_BARRIER owned"}], admission=self.enrollment(cap=1)
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertTrue(self.state["dag_entered"].wait(30))
        run, second = self.dag_submit(
            [{"prompt": "never starts"}], admission=self.enrollment(cap=1)
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        self.await_state(
            second["id"],
            lambda r: r["items"][0].get("admission_reason") == "queued_capacity",
        )
        self.run_tny(
            "jobs", "cancel", second["id"], "--expected-attempt", "1", "--json"
        )
        record = self.await_terminal(second["id"])
        self.assertEqual(record["items"][0]["admission_reason"], "canceled")
        os.kill(self.worker_pid(first["id"]), signal.SIGKILL)
        record = self.await_terminal(first["id"])
        self.assertEqual(record["cleanup"], "unknown")
        ledger = self.admission_state()
        self.assertEqual(
            next(e for e in ledger["entries"] if e["run"] == first["id"])["state"], 2
        )
        self.assertEqual(len(self.ask_requests()), 1)
        retry = self.run_tny("jobs", "retry", first["id"], "--json", check=False)
        self.assertEqual(retry.returncode, 1)

    def test_isolated_workers_edit_same_relative_file_and_keep_provenance(self):
        self.state["envdump"] = (
            "printf '%s' \"$TNY_TEAM_TASK\" > same.txt; git add same.txt"
        )
        run, payload = self.dag_submit(
            [
                {"prompt": "ENVDUMP edit first", "workspace": {"policy": "isolated"}},
                {"prompt": "ENVDUMP edit second", "workspace": {"policy": "isolated"}},
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        record = self.await_terminal(payload["id"])
        self.assertEqual(record["state"], "succeeded", record)
        self.assertFalse((self.workspace / "same.txt").exists())
        paths = [Path(i["workspace_cwd"]) for i in record["items"]]
        self.assertNotEqual(paths[0], paths[1])
        for index, item in enumerate(record["items"]):
            self.assertEqual((paths[index] / "same.txt").read_text(), str(index))
            self.assertEqual(item["workspace_inspection"], "recorded")
            self.assertTrue(item["workspace_dirty"])
            self.assertIn("same.txt", item["workspace_patch"])
            self.assertTrue(item["workspace_branch"])
            self.assertTrue(item["workspace_base"])
            self.assertTrue(item["session_id"])
            self.assertEqual(item["verification"], "unverified")
        self.assertNotEqual(
            record["items"][0]["session_id"], record["items"][1]["session_id"]
        )

    def test_capability_private_environment_and_max_steps(self):
        dump = self.home / "member.json"
        script = (
            "import os,json; json.dump({k:os.getenv(k) for k in "
            "['TNY_TEAM_RUN','TNY_TEAM_TASK','TNY_TEAM_ATTEMPT','TNY_TEAM_CAPABILITY',"
            "'TNY_TEAM_READ_ONLY','TNY_ADMISSION_ENROLLED']},open("
            + repr(str(dump))
            + ",'w'))"
        )
        import shlex

        self.state["envdump"] = "python3 -c " + shlex.quote(script)
        request = dict(
            kind="ask",
            dag=True,
            peer_messages=True,
            admission=self.enrollment(),
            items=[
                {
                    "prompt": "ENVDUMP DAG_BARRIER private member",
                    "workspace": {"policy": "shared_writable"},
                }
            ],
        )
        run = self.run_tny(
            "--max-steps",
            "4",
            "jobs",
            "submit",
            "batch",
            "--json",
            stdin=json.dumps(request).encode(),
        )
        payload = json.loads(run.stdout)
        self.assertTrue(self.state["dag_entered"].wait(30))
        member = json.loads(dump.read_text())
        self.assertEqual(member["TNY_TEAM_RUN"], payload["id"])
        self.assertEqual(member["TNY_TEAM_TASK"], "0")
        self.assertEqual(member["TNY_TEAM_ATTEMPT"], "1")
        self.assertIsNone(member["TNY_TEAM_READ_ONLY"])
        self.assertEqual(member["TNY_ADMISSION_ENROLLED"], "1")
        bearer = member["TNY_TEAM_CAPABILITY"]
        self.assertEqual(len(bearer), 64)
        import hashlib

        record = self.status(payload["id"])
        raw = Path(record["metadata_path"]).read_text()
        verifier = json.loads(raw)["items"][0]["mailbox_capability_sha256"]
        self.assertEqual(hashlib.sha256(bearer.encode()).hexdigest(), verifier)
        self.assertNotIn(bearer, raw)
        self.assertNotIn(bearer, json.dumps(record))
        argv = "\n".join(pids_argv(self.home.name))
        self.assertNotIn(bearer, argv)
        self.assertIn("--max-steps 4", argv)
        self.assertTrue(record["peer_messages"])
        self.assertEqual(record["max_steps"], 4)
        self.state["dag_release"].set()
        final = self.await_terminal(payload["id"])
        self.assertEqual(final["state"], "succeeded", final)
        self.assertNotIn(bearer, Path(final["items"][0]["log_path"]).read_text())

    def test_default_read_only_marker_is_assigned_to_owned_child(self):
        run, payload = self.dag_submit([{"prompt": "DAG_BARRIER read-only marker"}])
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertTrue(self.state["dag_entered"].wait(30))
        child = self.item_child_pid(payload["id"])
        observed = subprocess.run(
            ["ps", "eww", "-p", str(child), "-o", "command="],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        # Do not print the observed environment: it includes a private fixture bearer.
        self.assertTrue("TNY_TEAM_READ_ONLY=1" in observed, "read-only marker absent")
        self.state["dag_release"].set()
        self.assertEqual(self.await_terminal(payload["id"])["state"], "succeeded")

    def test_nested_enrolled_submission_refused_before_job_or_provider(self):
        env = dict(self.env, TNY_ADMISSION_ENROLLED="1")
        run = self.run_tny(
            "jobs",
            "submit",
            "ask",
            "--prompt",
            "must not start",
            "--json",
            env=env,
            check=False,
        )
        self.assertEqual(run.returncode, 1)
        self.assertEqual(self.job_dirs(), [])
        self.assertEqual(self.ask_requests(), [])
        self.assertFalse((self.home / ".tny" / "admission").exists())

    def test_actual_enrolled_child_cannot_submit_nested_jobs(self):
        import shlex

        dump = self.home / "nested.json"
        script = (
            "import subprocess,json; r=subprocess.run("
            + repr(
                [
                    TNY,
                    "--provider",
                    "openai",
                    "jobs",
                    "submit",
                    "ask",
                    "--prompt",
                    "nested never",
                    "--json",
                ]
            )
            + ",capture_output=True); json.dump({'exit':r.returncode},open("
            + repr(str(dump))
            + ",'w'))"
        )
        self.state["envdump"] = "python3 -c " + shlex.quote(script)
        run, payload = self.dag_submit(
            [
                {
                    "prompt": "ENVDUMP nested check",
                    "workspace": {"policy": "shared_writable"},
                }
            ],
            admission=self.enrollment(),
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        record = self.await_terminal(payload["id"])
        self.assertEqual(record["state"], "succeeded", record)
        self.assertEqual(json.loads(dump.read_text())["exit"], 1)
        self.assertEqual(len(self.job_dirs()), 1)
        self.assertFalse(any("nested never" in r for r in self.ask_requests()))

    def test_unknown_usage_and_retry_totals_exclude_carried_successes(self):
        run, payload = self.dag_submit([{"prompt": "NO_USAGE unknown"}])
        self.assertEqual(run.returncode, 0, run.stderr)
        record = self.await_terminal(payload["id"])
        self.assertEqual(record["state"], "succeeded", record)
        self.assertFalse(record["items"][0]["usage_known"])
        self.assertEqual(record["usage"]["unknown_items"], 1)
        job_id, first = self.failed_dag()
        initial = first["items"][0]["usage_input_tokens"]
        self.assertEqual(initial, 3)
        self.state["fail_ask"] = False
        self.run_tny("jobs", "retry", job_id, "--json")
        final = self.await_terminal(job_id)
        self.assertEqual(
            final["state"], "succeeded", (final, self.startup_diagnostics(final))
        )
        self.assertEqual(final["usage"]["known_input_tokens"], 9)
        self.assertEqual(final["usage"]["known_output_tokens"], 3)
        self.assertEqual(final["items"][0]["attempt"], 1)
        self.assertEqual(sum("DAG_A" in r for r in self.ask_requests()), 1)

    def test_failed_isolated_edit_is_not_retried_in_retained_tree(self):
        self.state["envdump"] = "printf edited > same.txt; git add same.txt"
        run, payload = self.dag_submit(
            [
                {"prompt": "ENVDUMP edit", "workspace": {"policy": "isolated"}},
                {"prompt": "FAIL after edit", "depends_on": [0]},
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        record = self.await_terminal(payload["id"])
        self.assertEqual(record["items"][0]["state"], "succeeded", record)
        before = len(self.ask_requests())
        retry = self.run_tny("jobs", "retry", payload["id"], "--json", check=False)
        self.assertEqual(retry.returncode, 2)
        self.assertEqual(len(self.ask_requests()), before)
        self.assertEqual(
            (Path(record["items"][0]["workspace_cwd"]) / "same.txt").read_text(),
            "edited",
        )

    def test_read_only_worker_denies_actual_edit_tool(self):
        self.state["envdump"] = "printf forbidden > forbidden.txt"
        run, payload = self.dag_submit([{"prompt": "ENVDUMP attempt edit"}])
        self.assertEqual(run.returncode, 0, run.stderr)
        record = self.await_terminal(payload["id"])
        self.assertFalse((self.workspace / "forbidden.txt").exists(), record)
        log = Path(record["items"][0]["log_path"]).read_text()
        self.assertIn('"tool_ok":false', log)

    def test_retry_preserves_positive_step_ceiling_and_rotates_capability(self):
        self.state["dag_release"].set()
        request = dict(
            kind="ask",
            dag=True,
            admission=self.enrollment(),
            items=[{"prompt": "FAIL DAG_BARRIER retry cap"}],
        )
        run = self.run_tny(
            "--max-steps",
            "4",
            "jobs",
            "submit",
            "batch",
            "--json",
            stdin=json.dumps(request).encode(),
        )
        job_id = json.loads(run.stdout)["id"]
        first = self.await_terminal(job_id)
        self.assertEqual(first["state"], "failed", first)
        old_hash = json.loads(Path(first["metadata_path"]).read_text())["items"][0][
            "mailbox_capability_sha256"
        ]
        self.state["fail_ask"] = False
        self.state["dag_release"].clear()
        self.state["dag_entered"].clear()
        self.run_tny("--max-steps", "20", "jobs", "retry", job_id, "--json")
        self.assertTrue(self.state["dag_entered"].wait(30))
        record = self.status(job_id)
        self.assertEqual(record["max_steps"], 4)
        self.assertIn("--max-steps 4", "\n".join(pids_argv(self.home.name)))
        current = json.loads(Path(record["metadata_path"]).read_text())
        self.assertNotEqual(current["items"][0]["mailbox_capability_sha256"], old_hash)
        self.state["dag_release"].set()
        final = self.await_terminal(job_id)
        self.assertEqual(final["state"], "succeeded", final)
        self.assertEqual(final["items"][0]["admission_claims"], 2)

    def test_workspace_preparation_does_not_hold_job_state_lock(self):
        import shutil

        wrapper_dir = self.home / "bin"
        wrapper_dir.mkdir()
        marker, release = self.home / "git-entered", self.home / "git-release"
        git = shutil.which("git")
        wrapper = wrapper_dir / "git"
        wrapper.write_text(
            f"#!{sys.executable}\nimport os,sys,time\n"
            f"from pathlib import Path\n"
            f"if 'worktree' in sys.argv and 'add' in sys.argv:\n"
            f" Path({str(marker)!r}).touch()\n"
            f" deadline=time.monotonic()+25\n"
            f" while not Path({str(release)!r}).exists() and time.monotonic()<deadline: time.sleep(.05)\n"
            f"os.execv({git!r},[{git!r},*sys.argv[1:]])\n"
        )
        wrapper.chmod(0o700)
        self.addCleanup(lambda: release.touch() if release.parent.exists() else None)
        env = dict(self.env, PATH=str(wrapper_dir) + os.pathsep + self.env["PATH"])
        request = dict(
            kind="ask",
            dag=True,
            admission=self.enrollment(),
            items=[{"prompt": "never launched", "workspace": {"policy": "isolated"}}],
        )
        run = self.run_tny(
            "jobs",
            "submit",
            "batch",
            "--json",
            env=env,
            stdin=json.dumps(request).encode(),
        )
        job_id = json.loads(run.stdout)["id"]
        deadline = time.monotonic() + 15
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertTrue(marker.exists())
        self.assertEqual(
            self.status(job_id)["items"][0]["workspace_preparation"], "intent"
        )
        started = time.monotonic()
        self.run_tny(
            "jobs", "cancel", job_id, "--expected-attempt", "1", "--json", timeout=3
        )
        self.assertLess(time.monotonic() - started, 3)
        release.touch()
        record = self.await_terminal(job_id)
        self.assertEqual(record["state"], "cancelled", record)
        self.assertEqual(self.ask_requests(), [])
        self.assertEqual(self.admission_state()["claims"], 0)
        self.assertTrue(Path(record["items"][0]["workspace_cwd"]).exists())

    def test_invalid_or_secret_admission_configuration_has_no_side_effects(self):
        for changes in (
            {"cap": 17},
            {"queue_cap": 129},
            {"claim_limit": 0},
            {"provider_scope": API_KEY},
            {"api_key": API_KEY},
            {"provider_scope": "../foreign"},
            {"root": str(self.home / "foreign")},
        ):
            with self.subTest(fields=list(changes)):
                config = self.enrollment()
                config.update(changes)
                run, _ = self.dag_submit(
                    [{"prompt": "must not launch"}], admission=config
                )
                self.assertEqual(run.returncode, 1)
                self.assertEqual(self.job_dirs(), [])
                self.assertFalse((self.home / ".tny" / "admission").exists())
                self.assertEqual(self.ask_requests(), [])

    def test_unsupported_host_and_private_request_fields_refused(self):
        request = dict(kind="ask", dag=True, items=[{"prompt": "never"}])
        run = self.run_tny(
            "jobs",
            "submit",
            "batch",
            "--json",
            stdin=json.dumps(request).encode(),
            provider="cursor",
            check=False,
        )
        self.assertEqual(run.returncode, 1)
        self.assertEqual(self.job_dirs(), [])
        for fields in (
            {"capability": "f" * 64},
            {"mailbox_capability_sha256": "f" * 64},
        ):
            request.update(fields)
            run, _ = self.submit(
                "batch", stdin=json.dumps(request).encode(), check=False
            )
            self.assertEqual(run.returncode, 1)
        self.assertEqual(self.job_dirs(), [])
        self.assertEqual(self.ask_requests(), [])


class JobsReview(JobsDAG):
    def test_stale_cancel_is_fenced_inside_transaction_after_retry(self):
        """Pause cancel at its actual state-lock acquisition, not a timed race."""
        import select

        self.state["dag_release"].set()
        run, payload = self.dag_submit(
            [
                {"prompt": "DAG_A"},
                {"prompt": "FAIL DAG_BARRIER", "depends_on": [0]},
                {"prompt": "DAG_C", "depends_on": [1]},
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        job_id = payload["id"]
        first = self.await_terminal(job_id)
        self.assertEqual(first["state"], "failed")
        source, library = self.home / "cancel-fence.c", self.home / "cancel-fence.so"
        source.write_text(r"""
#include <sys/file.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>
static int paused;
static int fenced_flock(int fd, int op) {
#ifdef __APPLE__
    int (*real_flock)(int,int) = flock;
#else
    int (*real_flock)(int,int) = (int (*)(int,int))dlsym(RTLD_NEXT,"flock");
#endif
    struct stat a,b;
    const char *path = getenv("FENCE_LOCK");
    if (!paused && (op & LOCK_EX) && path && !fstat(fd,&a) && !stat(path,&b) && a.st_ino==b.st_ino && a.st_dev==b.st_dev) {
        paused=1;
        char byte='R';
        if (write(atoi(getenv("FENCE_READY")),&byte,1)!=1 || read(atoi(getenv("FENCE_GO")),&byte,1)!=1) _exit(91);
    }
    return real_flock(fd,op);
}
#ifdef __APPLE__
__attribute__((used,section("__DATA,__interpose"))) static struct { const void *replacement; const void *original; } hook = {(const void *)fenced_flock,(const void *)flock};
#else
int flock(int fd,int op) { return fenced_flock(fd,op); }
#endif
""")
        subprocess.run(
            [
                "cc",
                "-fPIC",
                "-dynamiclib" if sys.platform == "darwin" else "-shared",
                str(source),
                "-o",
                str(library),
            ],
            check=True,
            capture_output=True,
        )
        ready_r, ready_w = os.pipe()
        go_r, go_w = os.pipe()
        loader = "DYLD_INSERT_LIBRARIES" if sys.platform == "darwin" else "LD_PRELOAD"
        env = dict(
            self.env,
            **{
                loader: str(library),
                "FENCE_LOCK": str(Path(first["metadata_path"]).parent / "state.lock"),
                "FENCE_READY": str(ready_w),
                "FENCE_GO": str(go_r),
            },
        )
        cancel = subprocess.Popen(
            [
                TNY,
                "--provider",
                "openai",
                "jobs",
                "cancel",
                job_id,
                "--expected-attempt",
                "1",
                "--json",
            ],
            cwd=self.workspace,
            env=env,
            pass_fds=(ready_w, go_r),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
        self.started.append(cancel)
        os.close(ready_w)
        os.close(go_r)
        try:
            self.assertTrue(
                select.select([ready_r], [], [], 20)[0],
                "cancel did not reach state transaction",
            )
            ready_byte = os.read(ready_r, 1)
            if ready_byte != b"R":
                stdout, stderr = cancel.communicate(timeout=5)
                self.fail(
                    f"cancel barrier exited {cancel.returncode}: {stderr.decode()} {stdout.decode()}"
                )
            self.state["dag_release"].clear()
            self.state["dag_entered"].clear()
            self.state["fail_ask"] = False
            self.run_tny("jobs", "retry", job_id, "--json")
            self.assertTrue(self.state["dag_entered"].wait(30))
            os.write(go_w, b"G")
            stdout, stderr = cancel.communicate(timeout=15)
            self.assertEqual(cancel.returncode, 1, stdout)
            self.assertIn(b"stale_attempt", stderr)
            current = self.status(job_id)
            self.assertEqual(current["attempt"], 2)
            self.assertFalse(current["cancel_requested"])
            self.assertTrue(all(not i["cancel_requested"] for i in current["items"]))
            missing = self.run_tny("jobs", "cancel", job_id, "--json", check=False)
            self.assertEqual(missing.returncode, 1)
            self.assertIn(b"expected_attempt", missing.stderr)
            self.state["dag_release"].set()
            self.assertEqual(self.await_terminal(job_id)["state"], "succeeded")
        finally:
            os.close(ready_r)
            os.close(go_w)

    def test_execution_scope_refuses_endpoint_key_policy_and_extra_dirs(self):
        job_id, first = self.failed_dag()
        before = len(self.ask_requests())
        settings = self.home / ".tny" / "settings.json"
        saved = settings.read_bytes() if settings.exists() else None
        extra = self.home / "extra"
        extra.mkdir()
        cases = [
            (
                dict(
                    self.env,
                    OPENAI_BASE_URL=self.env["OPENAI_BASE_URL"]
                    + "/different?credential=not-real",
                ),
                (),
            ),
            (dict(self.env, OPENAI_API_KEY="different-fixture-key"), ()),
            (self.env, ("--add-dir", str(extra))),
        ]
        for env, flags in cases:
            run = self.run_tny(
                *flags, "jobs", "retry", job_id, "--json", env=env, check=False
            )
            self.assertEqual(run.returncode, 1, run.stdout)
            self.assertIn(b"execution_scope_changed", run.stderr)
            self.assertEqual(len(self.ask_requests()), before)
        config = json.loads(saved) if saved else {}
        config["permission"] = {"rules": [{"tool": "terminal", "allow": False}]}
        settings.write_text(json.dumps(config))
        run = self.run_tny("jobs", "retry", job_id, "--json", check=False)
        self.assertEqual(run.returncode, 1)
        self.assertIn(b"execution_scope_changed", run.stderr)
        if saved is None:
            settings.unlink()
        else:
            settings.write_bytes(saved)
        self.assertEqual(len(self.ask_requests()), before)
        raw = Path(first["metadata_path"]).read_text()
        self.assertEqual(len(json.loads(raw)["execution_scope_sha256"]), 64)
        self.assertNotIn(API_KEY, raw)
        self.assertNotIn(self.env["OPENAI_BASE_URL"], raw)

    def test_chatgpt_account_fenced_but_same_account_token_refresh_allowed(self):
        request = dict(
            kind="ask", dag=True, items=[{"prompt": "keeper"}, {"prompt": "FAIL codex"}]
        )
        run = self.run_tny(
            "--model",
            "mock-model",
            "jobs",
            "submit",
            "batch",
            "--json",
            stdin=json.dumps(request).encode(),
            provider="codex",
        )
        job_id = json.loads(run.stdout)["id"]
        first = self.await_terminal(job_id)
        self.assertEqual(first["items"][0]["state"], "succeeded", first)
        before = len(self.ask_requests())
        run = self.run_tny(
            "--model",
            "mock-model",
            "jobs",
            "retry",
            job_id,
            "--json",
            env=dict(self.env, CHATGPT_ACCOUNT_ID="other-account"),
            provider="codex",
            check=False,
        )
        self.assertEqual(run.returncode, 1)
        self.assertIn(b"execution_scope_changed", run.stderr)
        self.assertEqual(len(self.ask_requests()), before)
        self.state["fail_ask"] = False
        self.state["chat_tokens"] = ["refreshed-fixture-token"]
        run = self.run_tny(
            "--model",
            "mock-model",
            "jobs",
            "retry",
            job_id,
            "--json",
            env=dict(self.env, CHATGPT_ACCESS_TOKEN="refreshed-fixture-token"),
            provider="codex",
        )
        final = self.await_terminal(job_id)
        self.assertEqual(final["state"], "succeeded", final)
        self.assertEqual(sum("keeper" in r for r in self.ask_requests()), 1)
        raw = Path(final["metadata_path"]).read_text()
        self.assertNotIn("refreshed-fixture-token", raw)
        self.assertNotIn(TOKEN, raw)

    def test_actual_member_cannot_control_unrelated_run_via_cli_or_tool(self):
        import shlex

        run, target = self.submit("ask", stdin=b"DAG_BARRIER unrelated sentinel")
        self.assertEqual(run.returncode, 0)
        self.assertTrue(self.state["dag_entered"].wait(30))
        target_id = target["id"]
        dump = self.home / "member-controls.json"
        script = (
            "import subprocess,json; results=[]; "
            + "\nfor action in ['cancel','retry','rm']:\n r=subprocess.run("
            + repr([TNY, "--provider", "openai", "jobs"])
            + "+[action,"
            + repr(target_id)
            + ",'--json'],capture_output=True); results.append([r.returncode,r.stderr.decode()])\n"
            + "json.dump(results,open("
            + repr(str(dump))
            + ",'w'))"
        )
        self.state["envdump"] = "python3 -c " + shlex.quote(script)
        self.state["member_target"] = target_id
        run, member = self.dag_submit(
            [
                {
                    "prompt": "ENVDUMP member CLI controls",
                    "workspace": {"policy": "shared_writable"},
                },
                {
                    "prompt": "MEMBER_CONTROL typed cancel",
                    "workspace": {"policy": "shared_writable"},
                },
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        result = self.await_terminal(member["id"])
        self.assertEqual(result["state"], "succeeded", result)
        for code, error in json.loads(dump.read_text()):
            self.assertEqual(code, 1)
            self.assertIn("member_control_unsupported", error)
        self.assertIn("member_control_unsupported", json.dumps(self.state["bodies"]))
        sentinel = self.status(target_id)
        self.assertEqual(sentinel["state"], "running")
        self.assertFalse(sentinel["cancel_requested"])
        # An ordinary local operator retains legacy batch authority.
        self.run_tny("jobs", "cancel", target_id, "--json")
        self.assertEqual(self.await_terminal(target_id)["state"], "cancelled")


class JobsSoftBudget(JobsDAG):
    def test_observed_exhaustion_cancels_pending_admission_and_retry_stays_stopped(
        self,
    ):
        run, payload = self.dag_submit(
            [{"prompt": "first observed"}, {"prompt": "must not request"}],
            concurrency=1,
            budget={"soft_tokens": 4},
            admission=dict(
                label="budget",
                provider_scope="public",
                cap=1,
                queue_cap=8,
                claim_limit=10,
            ),
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        record = self.await_terminal(payload["id"])
        self.assertEqual(record["budget"], {"soft_tokens": 4, "unknown_usage": "stop"})
        self.assertEqual(record["budget_observed_tokens"], 4)
        self.assertEqual(record["budget_state"], "tokens_exhausted")
        self.assertEqual(record["items"][1]["state"], "cancelled")
        self.assertEqual(record["items"][1]["admission_reason"], "canceled")
        self.assertEqual(len(self.ask_requests()), 1)
        ledger = json.loads(
            (
                self.home / ".tny" / "admission" / "budget" / "public" / "state.json"
            ).read_text()
        )
        self.assertEqual(ledger["claims"], 1)
        before = Path(record["metadata_path"]).read_bytes()
        for _ in range(2):
            retry = self.run_tny("jobs", "retry", payload["id"], "--json", check=False)
            self.assertEqual(retry.returncode, 1)
            self.assertIn(b"budget_stop", retry.stderr)
        self.assertEqual(before, Path(record["metadata_path"]).read_bytes())
        self.assertEqual(len(self.ask_requests()), 1)

    def test_unknown_usage_default_stops_and_explicit_continue_is_inspectable(self):
        for policy in ("stop", "continue"):
            before = len(self.ask_requests())
            budget = {"soft_tokens": 100}
            if policy == "continue":
                budget["unknown_usage"] = policy
            run, payload = self.dag_submit(
                [{"prompt": "NO_USAGE first"}, {"prompt": "second"}],
                concurrency=1,
                budget=budget,
            )
            self.assertEqual(run.returncode, 0, run.stderr)
            record = self.await_terminal(payload["id"])
            self.assertTrue(record["budget_usage_unknown"])
            if policy == "stop":
                self.assertEqual(record["budget_state"], "usage_unknown")
                self.assertEqual(record["items"][1]["state"], "cancelled")
                self.assertEqual(len(self.ask_requests()) - before, 1)
            else:
                self.assertEqual(record["state"], "succeeded")
                self.assertEqual(record["budget_state"], "usage_unknown_continue")
                self.assertEqual(record["budget_observed_tokens"], 4)
                self.assertEqual(len(self.ask_requests()) - before, 2)

    def test_already_admitted_work_can_overshoot_soft_budget(self):
        run, payload = self.dag_submit(
            [
                {"prompt": "DAG_BARRIER first"},
                {"prompt": "DAG_BARRIER second"},
                {"prompt": "third not admitted"},
            ],
            concurrency=2,
            budget={"soft_tokens": 4},
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        self.await_state(payload["id"], lambda r: len(self.ask_requests()) == 2)
        self.assertEqual(len(self.ask_requests()), 2)
        self.state["dag_release"].set()
        record = self.await_terminal(payload["id"])
        self.assertEqual(record["budget_observed_tokens"], 8)
        self.assertEqual(record["budget_state"], "tokens_exhausted")
        self.assertEqual(record["items"][2]["state"], "cancelled")
        self.assertEqual(len(self.ask_requests()), 2)

    def test_retry_budget_accounting_does_not_count_carried_a_twice(self):
        run, payload = self.dag_submit(
            [
                {"prompt": "DAG_A"},
                {"prompt": "FAIL DAG_B", "depends_on": [0]},
                {"prompt": "DAG_C", "depends_on": [1]},
            ],
            concurrency=1,
            budget={"soft_tokens": 8, "unknown_usage": "continue"},
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        job_id = payload["id"]
        first = self.await_terminal(job_id)
        self.assertEqual(first["budget_observed_tokens"], 4)
        self.state["fail_ask"] = False
        self.run_tny("jobs", "retry", job_id, "--json")
        final = self.await_terminal(job_id)
        self.assertEqual(final["budget_observed_tokens"], 8)
        self.assertEqual(final["items"][0]["attempt"], 1)
        self.assertEqual(final["items"][1]["state"], "succeeded")
        self.assertEqual(final["items"][2]["state"], "cancelled")
        self.assertEqual(sum("DAG_A" in r for r in self.ask_requests()), 1)
        self.assertFalse(any("DAG_C" in r for r in self.ask_requests()))
        self.assertTrue(final["budget_usage_unknown"])

    def test_invalid_token_policy_is_refused_before_side_effects(self):
        for budget in (
            {"soft_tokens": 0},
            {"soft_tokens": 3.5},
            {"soft_tokens": 10, "unknown_usage": "zero"},
            {"soft_tokens": 10, "api_key": API_KEY},
        ):
            run, _ = self.dag_submit([{"prompt": "never"}], budget=budget)
            self.assertEqual(run.returncode, 1)
            self.assertEqual(self.job_dirs(), [])
            self.assertEqual(self.ask_requests(), [])


class JobsProviderSelection(JobsDAG):
    def setUp(self):
        super().setUp()
        self.secondary = QuietServer(("127.0.0.1", 0), Handler)
        self.secondary.state = dict(
            self.state,
            requests=[],
            bodies=[],
            headers=[],
            active=0,
            peak=0,
            dag_entered=threading.Event(),
            dag_release=threading.Event(),
        )
        self.secondary.lock = threading.Lock()
        threading.Thread(target=self.secondary.serve_forever, daemon=True).start()
        self.settings_file = self.home / ".tny" / "settings.json"
        self.settings_file.parent.mkdir(exist_ok=True)
        self.config = {
            "alpha": {
                "base_url": self.env["OPENAI_BASE_URL"],
                "api_key_env": "ALPHA_KEY",
                "model": "alpha-model",
                "wire_api": "chat",
            },
            "beta": {
                "base_url": f"http://127.0.0.1:{self.secondary.server_port}/v1",
                "api_key_env": "BETA_KEY",
                "model": "beta-model",
                "wire_api": "chat",
            },
            "effort": {"alpha": "low", "beta": "high"},
        }
        self.settings_file.write_text(json.dumps(self.config))
        self.env.update(ALPHA_KEY="fixture-alpha-key", BETA_KEY="fixture-beta-key")

    def tearDown(self):
        self.secondary.state["dag_release"].set()
        super().tearDown()
        self.secondary.shutdown()
        self.secondary.server_close()

    def all_requests(self):
        return len(self.ask_requests()) + len(self.secondary.state["requests"])

    def test_per_item_endpoints_credentials_defaults_and_parent_unchanged(self):
        request = dict(
            kind="ask",
            dag=True,
            concurrency=3,
            items=[
                {"prompt": "alpha worker", "provider": "alpha"},
                {
                    "prompt": "beta worker",
                    "provider": "beta",
                    "model": "beta-override",
                    "effort": "medium",
                },
                {"prompt": "parent worker"},
            ],
        )
        env = dict(self.env, ROOT_OVERRIDE="fixture-parent-override")
        before = dict(env)
        run = self.run_tny(
            "--api-key-env",
            "ROOT_OVERRIDE",
            "--model",
            "parent-model",
            "--effort",
            "high",
            "jobs",
            "submit",
            "batch",
            "--json",
            env=env,
            stdin=json.dumps(request).encode(),
        )
        record = self.await_terminal(json.loads(run.stdout)["id"])
        self.assertEqual(record["state"], "succeeded", record)
        self.assertEqual(env, before)
        self.assertEqual(
            (record["provider"], record["model"], record["effort"]),
            ("openai", "parent-model", "high"),
        )
        self.assertEqual(
            [(i["provider"], i["model"], i["effort"]) for i in record["items"]],
            [
                ("alpha", "alpha-model", "low"),
                ("beta", "beta-override", "medium"),
                ("openai", "parent-model", "high"),
            ],
        )
        primary = {
            prompt: next(
                h["authorization"]
                for h in self.state["headers"]
                if h["prompt"].startswith(prompt)
            )
            for prompt in ("alpha worker", "parent worker")
        }
        self.assertEqual(primary["alpha worker"], "Bearer fixture-alpha-key")
        self.assertEqual(primary["parent worker"], "Bearer fixture-parent-override")
        self.assertEqual(
            self.secondary.state["headers"][0]["authorization"],
            "Bearer fixture-beta-key",
        )
        self.assertEqual(self.secondary.state["bodies"][0]["model"], "beta-override")
        self.assertEqual(
            self.secondary.state["bodies"][0]["reasoning_effort"], "medium"
        )
        body = next(b for b in self.state["bodies"] if b["model"] == "alpha-model")
        self.assertEqual(body["reasoning_effort"], "low")
        raw = Path(record["metadata_path"]).read_text()
        self.assertEqual(json.loads(raw)["items"][1]["request"]["provider"], "beta")
        for item in record["items"]:
            self.assertEqual(len(item["execution_scope_sha256"]), 64)
        for secret in (
            API_KEY,
            env["ROOT_OVERRIDE"],
            self.env["ALPHA_KEY"],
            self.env["BETA_KEY"],
        ):
            self.assertNotIn(secret, raw)
            self.assertNotIn(secret, json.dumps(record))
            for item in record["items"]:
                self.assertNotIn(secret, Path(item["log_path"]).read_text())

    def write_settings(self, permission):
        config = dict(self.config, permission=permission)
        self.settings_file.write_text(json.dumps(config))

    def test_permission_detail_discloses_public_effective_selectors_only(self):
        run = JobsPermissions.ask_with_tool(
            self,
            "job_submit",
            {
                "kind": "ask",
                "dag": True,
                "items": [
                    {"prompt": "alpha", "provider": "alpha"},
                    {
                        "prompt": "beta",
                        "provider": "beta",
                        "model": "override",
                        "effort": "medium",
                    },
                ],
            },
            {"job_status": "allow"},
        )
        detail = run.stderr.decode()
        self.assertIn("item=0 provider=alpha model=alpha-model effort=low", detail)
        self.assertIn("item=1 provider=beta model=override effort=medium", detail)
        for secret in (API_KEY, self.env["ALPHA_KEY"], self.env["BETA_KEY"]):
            self.assertNotIn(secret, detail)
        self.assertEqual(self.job_dirs(), [])
        self.assertEqual(self.secondary.state["requests"], [])

    def test_explicit_profiles_do_not_retain_parent_wire_or_endpoint_overrides(self):
        request = dict(
            kind="ask",
            dag=True,
            items=[
                {"prompt": "alpha independent", "provider": "alpha"},
                {"prompt": "beta independent", "provider": "beta"},
            ],
        )
        run = self.run_tny(
            "--wire-api",
            "responses",
            "--base-url",
            "http://127.0.0.1:1/unused",
            "--model",
            "parent-only",
            "--effort",
            "high",
            "jobs",
            "submit",
            "batch",
            "--json",
            stdin=json.dumps(request).encode(),
        )
        record = self.await_terminal(json.loads(run.stdout)["id"])
        self.assertEqual(record["state"], "succeeded", record)
        self.assertEqual(record["model"], "parent-only")
        self.assertEqual(self.state["bodies"][0]["model"], "alpha-model")
        self.assertEqual(self.state["bodies"][0]["reasoning_effort"], "low")
        self.assertTrue(
            all(
                h["path"].endswith("/chat/completions")
                for h in self.state["headers"] + self.secondary.state["headers"]
            )
        )

    def test_homogeneous_explicit_admission_and_account_override_refusal(self):
        request = dict(
            kind="ask",
            dag=True,
            admission=dict(
                label="same", provider_scope="public", cap=2, queue_cap=8, claim_limit=8
            ),
            items=[
                {"prompt": "first", "provider": "openai"},
                {"prompt": "second", "provider": "openai"},
            ],
        )
        env = dict(self.env, ROOT_OVERRIDE="fixture-different-parent-account")
        refused = self.run_tny(
            "--api-key-env",
            "ROOT_OVERRIDE",
            "jobs",
            "submit",
            "batch",
            "--json",
            env=env,
            stdin=json.dumps(request).encode(),
            check=False,
        )
        self.assertEqual(refused.returncode, 1)
        self.assertEqual(self.job_dirs(), [])
        self.assertEqual(self.all_requests(), 0)
        run, payload = self.submit("batch", stdin=json.dumps(request).encode())
        record = self.await_terminal(payload["id"])
        self.assertEqual(record["state"], "succeeded", record)
        self.assertEqual(self.all_requests(), 2)

    def test_selected_child_environment_never_contains_root_or_sibling_keys(self):
        import shlex

        script = (
            "import os,json; names=['TNY_JOB_API_KEY','OPENAI_API_KEY','ALPHA_KEY','BETA_KEY',"
            "'CHATGPT_ACCESS_TOKEN','ROOT_ALIAS','LANG','TNY_TOOLS']; "
            "json.dump({k:os.getenv(k) for k in names},open("
            + repr(str(self.home / "env-"))
            + "+os.environ['TNY_TEAM_TASK']+'.json','w'))"
        )
        command = "python3 -c " + shlex.quote(script)
        self.state["envdump"] = command
        self.secondary.state["envdump"] = command
        self.config["jobs"] = {"ask_env": ["ROOT_ALIAS"]}
        self.settings_file.write_text(json.dumps(self.config))
        env = dict(
            self.env,
            ROOT_ALIAS=API_KEY,
            LANG=self.env["BETA_KEY"],
            TNY_TOOLS="terminal",
        )
        request = dict(
            kind="ask",
            dag=True,
            items=[
                {
                    "prompt": "ENVDUMP DAG_BARRIER first",
                    "provider": "alpha",
                    "workspace": {"policy": "shared_writable"},
                },
                {
                    "prompt": "ENVDUMP DAG_BARRIER second",
                    "provider": "beta",
                    "workspace": {"policy": "shared_writable"},
                },
            ],
        )
        run = self.run_tny(
            "jobs",
            "submit",
            "batch",
            "--json",
            env=env,
            stdin=json.dumps(request).encode(),
        )
        job_id = json.loads(run.stdout)["id"]
        self.assertTrue(self.state["dag_entered"].wait(30))
        self.assertTrue(self.secondary.state["dag_entered"].wait(30))
        for index, key in enumerate((self.env["ALPHA_KEY"], self.env["BETA_KEY"])):
            values = json.loads((self.home / f"env-{index}.json").read_text())
            self.assertEqual(values.pop("TNY_JOB_API_KEY"), key)
            self.assertEqual(values.pop("TNY_TOOLS"), "terminal")
            self.assertTrue(
                all(value is None for value in values.values()),
                "foreign credential carrier reached child",
            )
        argv = "\n".join(pids_argv(self.home.name))
        for secret in (API_KEY, self.env["ALPHA_KEY"], self.env["BETA_KEY"]):
            self.assertNotIn(secret, argv)
        self.state["dag_release"].set()
        self.secondary.state["dag_release"].set()
        self.assertEqual(self.await_terminal(job_id)["state"], "succeeded")

    def test_unknown_host_mixed_admission_and_nonportable_ceilings_refuse_without_spend(
        self,
    ):
        cases = [
            (
                [
                    {"prompt": "valid", "provider": "alpha"},
                    {"prompt": "invalid", "provider": "unknown_profile"},
                ],
                {},
            ),
            ([{"prompt": "host", "provider": "cursor"}], {}),
            ([{"prompt": "host", "provider": "acp"}], {}),
            (
                [
                    {
                        "prompt": "isolated",
                        "provider": "alpha",
                        "workspace": {"policy": "isolated"},
                    }
                ],
                {},
            ),
            (
                [{"prompt": "mixed", "provider": "beta"}],
                {
                    "admission": dict(
                        label="mixed",
                        provider_scope="public",
                        cap=2,
                        queue_cap=8,
                        claim_limit=8,
                    )
                },
            ),
        ]
        for items, extra in cases:
            run, _ = self.dag_submit(items, **extra)
            self.assertEqual(run.returncode, 1)
            self.assertEqual(self.job_dirs(), [])
            self.assertEqual(self.all_requests(), 0)
            self.assertFalse((self.home / ".tny" / "admission").exists())
            self.assertFalse((self.workspace / ".git" / "worktrees").exists())
        env_only = dict(
            self.env,
            ENV_ONLY_BASE_URL=self.env["OPENAI_BASE_URL"],
            ENV_ONLY_API_KEY="fixture-env-only-key",
            ENV_ONLY_DEFAULT_MODEL="env-only-model",
        )
        request = dict(
            kind="ask", dag=True, items=[{"prompt": "env-only", "provider": "env_only"}]
        )
        refused = self.run_tny(
            "jobs",
            "submit",
            "batch",
            "--json",
            env=env_only,
            stdin=json.dumps(request).encode(),
            check=False,
        )
        self.assertEqual(refused.returncode, 1)
        self.assertIn(b"settings-backed", refused.stderr)
        self.assertEqual(self.job_dirs(), [])
        self.assertEqual(self.all_requests(), 0)
        self.config["beta"]["auth_header_name"] = "X-Custom-Key"
        self.settings_file.write_text(json.dumps(self.config))
        run, _ = self.dag_submit([{"prompt": "custom routing", "provider": "beta"}])
        self.assertEqual(run.returncode, 1)
        self.assertEqual(self.job_dirs(), [])
        self.assertEqual(self.all_requests(), 0)
        (self.workspace / ".tny.json").write_text(json.dumps({"sandbox": "os"}))
        run, _ = self.dag_submit(
            [{"prompt": "must not widen sandbox", "provider": "alpha"}]
        )
        self.assertEqual(run.returncode, 1)
        self.assertEqual(self.job_dirs(), [])

    def test_retry_does_not_lend_a_carried_siblings_credential_alias(self):
        import shlex

        dump = self.home / "retry-env.json"
        self.state["envdump"] = "python3 -c " + shlex.quote(
            "import os,json; json.dump({'LANG':os.getenv('LANG'),'own':os.getenv('TNY_JOB_API_KEY')},open("
            + repr(str(dump))
            + ",'w'))"
        )
        env = dict(self.env, LANG=self.env["BETA_KEY"])
        request = dict(
            kind="ask",
            dag=True,
            items=[
                {"prompt": "keeper", "provider": "beta"},
                {
                    "prompt": "FAIL ENVDUMP retry",
                    "provider": "alpha",
                    "workspace": {"policy": "shared_writable"},
                },
            ],
        )
        run = self.run_tny(
            "jobs",
            "submit",
            "batch",
            "--json",
            env=env,
            stdin=json.dumps(request).encode(),
        )
        job_id = json.loads(run.stdout)["id"]
        first = self.await_terminal(job_id)
        self.assertEqual([i["state"] for i in first["items"]], ["succeeded", "failed"])
        self.state["fail_ask"] = False
        self.run_tny("jobs", "retry", job_id, "--json", env=env)
        final = self.await_terminal(job_id)
        self.assertEqual(final["state"], "succeeded", final)
        captured = json.loads(dump.read_text())
        self.assertIsNone(captured["LANG"])
        self.assertEqual(captured["own"], self.env["ALPHA_KEY"])
        self.assertEqual(len(self.secondary.state["requests"]), 1)

    def test_permission_tool_and_step_ceilings_are_parent_owned(self):
        request = dict(
            kind="ask",
            dag=True,
            items=[{"prompt": "DAG_BARRIER ceiling", "provider": "beta"}],
        )
        run = self.run_tny(
            "--permission-mode",
            "auto",
            "--max-steps",
            "3",
            "jobs",
            "submit",
            "batch",
            "--json",
            env=dict(self.env, TNY_TOOLS="terminal"),
            stdin=json.dumps(request).encode(),
        )
        job_id = json.loads(run.stdout)["id"]
        self.assertTrue(self.secondary.state["dag_entered"].wait(30))
        record = self.status(job_id)
        self.assertEqual(
            (record["permission_ceiling"], record["tool_ceiling"], record["max_steps"]),
            ("auto", "terminal", 3),
        )
        argv = "\n".join(pids_argv(self.home.name, "--events=jsonl"))
        self.assertIn("--permission-mode auto", argv)
        self.assertIn("--max-steps 3", argv)
        self.assertIn("--provider beta", argv)
        self.secondary.state["dag_release"].set()
        self.assertEqual(self.await_terminal(job_id)["state"], "succeeded")

    def test_changed_carried_secondary_scope_blocks_retry_before_any_request(self):
        run, payload = self.dag_submit(
            [
                {"prompt": "keeper", "provider": "beta"},
                {"prompt": "FAIL retry", "provider": "alpha"},
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        job_id = payload["id"]
        first = self.await_terminal(job_id)
        self.assertEqual([i["state"] for i in first["items"]], ["succeeded", "failed"])
        before = self.all_requests()
        original = self.settings_file.read_bytes()
        for field, value in (
            ("base_url", self.env["OPENAI_BASE_URL"] + "/changed"),
            ("api_key_env", "OTHER_BETA_KEY"),
        ):
            config = json.loads(original)
            config["beta"][field] = value
            self.settings_file.write_text(json.dumps(config))
            run = self.run_tny(
                "jobs",
                "retry",
                job_id,
                "--json",
                env=dict(self.env, OTHER_BETA_KEY="changed-secondary-key"),
                check=False,
            )
            self.assertEqual(run.returncode, 1)
            self.assertIn(b"item_execution_scope_changed", run.stderr)
            self.assertEqual(self.all_requests(), before)
        self.settings_file.write_bytes(original)
        self.state["fail_ask"] = False
        self.run_tny("jobs", "retry", job_id, "--json")
        final = self.await_terminal(job_id)
        self.assertEqual(final["state"], "succeeded", final)
        self.assertEqual(len(self.secondary.state["requests"]), 1)
        self.assertEqual(final["items"][0]["carried_from_attempt"], 1)

    def test_explicit_codex_is_independent_and_changed_account_is_fenced(self):
        run, payload = self.dag_submit(
            [
                {
                    "prompt": "codex keeper",
                    "provider": "codex",
                    "model": "codex-worker",
                },
                {"prompt": "FAIL alpha", "provider": "alpha"},
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        job_id = payload["id"]
        record = self.await_terminal(job_id)
        self.assertEqual(record["items"][0]["state"], "succeeded", record)
        codex = next(h for h in self.state["headers"] if h["kind"] == "codex-chat")
        self.assertEqual(
            (codex["authorization"], codex["account"]), (f"Bearer {TOKEN}", ACCOUNT)
        )
        self.assertEqual(record["items"][0]["provider"], "codex")
        self.assertEqual(record["items"][0]["model"], "codex-worker")
        before = self.all_requests()
        run = self.run_tny(
            "jobs",
            "retry",
            job_id,
            "--json",
            env=dict(self.env, CHATGPT_ACCOUNT_ID="other-account"),
            check=False,
        )
        self.assertEqual(run.returncode, 1)
        self.assertIn(b"item_execution_scope_changed", run.stderr)
        self.assertEqual(self.all_requests(), before)


class JobsConcurrency(JobsFixture):
    hold = 1.5

    def test_the_concurrency_bound_holds_against_an_independent_counter(self):
        items = [{"prompt": f"HOLD item {i}"} for i in range(6)]
        request = self.batch_request("ask", items, concurrency=2)
        _run, payload = self.submit("batch", "--request", request)
        record = self.await_terminal(payload["id"], timeout=180)
        self.assertEqual(record["state"], "succeeded", record)
        self.assertEqual(len(self.ask_requests()), 6)
        self.assertLessEqual(
            self.state["peak"], 2, "more items ran than the bound allows"
        )
        self.assertGreater(self.state["peak"], 1, "the batch never actually overlapped")

    def test_a_batch_rejects_mixed_kinds_and_out_of_range_bounds(self):
        mixed = self.batch_request(
            "ask", [{"prompt": "one"}, {"kind": "image", "prompt": "two"}]
        )
        run, _payload = self.submit("batch", "--request", mixed, check=False)
        self.assertEqual(run.returncode, 1, run.stdout)
        self.assertEqual(self.job_dirs(), [])
        too_many = self.batch_request("ask", [{"prompt": "x"}] * 65)
        run, _payload = self.submit("batch", "--request", too_many, check=False)
        self.assertEqual(run.returncode, 1)
        wide = self.batch_request("ask", [{"prompt": "x"}], concurrency=17)
        run, _payload = self.submit("batch", "--request", wide, check=False)
        self.assertEqual(run.returncode, 1)
        self.assertEqual(self.ask_requests(), [])


class JobsRetry(JobsFixture):
    hold = 0.2

    def mixed_job(self):
        request = self.batch_request(
            "ask", [{"prompt": "keeper"}, {"prompt": "FAIL once"}], concurrency=2
        )
        _run, payload = self.submit("batch", "--request", request)
        record = self.await_terminal(payload["id"])
        self.assertEqual([i["state"] for i in record["items"]], ["succeeded", "failed"])
        return payload["id"], record

    def test_selective_retry_never_re_executes_a_successful_item(self):
        job_id, first = self.mixed_job()
        keeper = first["items"][0]
        before = len([r for r in self.ask_requests() if "keeper" in r])
        self.state["fail_ask"] = False
        run = self.run_tny("jobs", "retry", job_id, "--failed", "--json")
        self.assertEqual(run.returncode, 0, run.stderr)
        record = self.await_terminal(job_id)
        self.assertEqual(record["attempt"], 2, record)
        self.assertEqual(
            [i["state"] for i in record["items"]], ["succeeded", "succeeded"]
        )
        after = len([r for r in self.ask_requests() if "keeper" in r])
        self.assertEqual(after, before, "a successful item was re-executed")
        carried = record["items"][0]
        self.assertEqual(carried["session_id"], keeper["session_id"])
        self.assertEqual(carried["result_sha256"], keeper["result_sha256"])
        self.assertEqual(carried["carried_from_attempt"], 1)
        # The finished attempt is kept immutably beside the projection.
        snapshot = Path(record["metadata_path"]).parent / "attempt-1.json"
        self.assertTrue(snapshot.is_file())
        self.assertEqual(json.loads(snapshot.read_text())["attempt"], 1)

    def test_a_retry_clears_the_previous_attempts_cancellation(self):
        """An old attempt's controls can never apply to the new one."""
        self.state["hold"] = 3.0
        request = self.batch_request(
            "ask", [{"prompt": "HOLD first"}, {"prompt": "HOLD second"}], concurrency=2
        )
        _run, payload = self.submit("batch", "--request", request)
        job_id = payload["id"]
        self.await_state(job_id, lambda r: r["items"][0]["state"] == "running")
        self.run_tny("jobs", "cancel", job_id, "--json")
        record = self.await_terminal(job_id, timeout=60)
        self.assertEqual(record["state"], "cancelled", record)
        self.assertTrue(record["cancel_requested"], record)

        self.state["hold"] = 0.2
        run = self.run_tny("jobs", "retry", job_id, "--failed", "--json")
        self.assertEqual(run.returncode, 0, run.stderr)
        record = self.await_terminal(job_id)
        self.assertEqual(record["attempt"], 2, record)
        self.assertFalse(record["cancel_requested"], record)
        self.assertEqual(
            [item["state"] for item in record["items"]],
            ["succeeded", "succeeded"],
            record,
        )
        for item in record["items"]:
            self.assertFalse(item["cancel_requested"], item)

    def test_explicitly_selecting_a_successful_item_is_refused(self):
        job_id, _first = self.mixed_job()
        run = self.run_tny(
            "jobs", "retry", job_id, "--items", "0", "--json", check=False
        )
        self.assertEqual(run.returncode, 1, run.stdout)
        self.assertIn(b"already succeeded", run.stderr)

    def test_a_changed_carried_success_blocks_the_retry_before_spending(self):
        job_id, first = self.mixed_job()
        before = len(self.ask_requests())
        log = Path(first["items"][0]["log_path"])
        log.write_text(log.read_text() + "\n")
        self.state["fail_ask"] = False
        run = self.run_tny("jobs", "retry", job_id, "--failed", "--json", check=False)
        self.assertEqual(run.returncode, 2, run.stdout)
        self.assertIn("JOB_STALE_SUCCESS", run.stdout.decode())
        self.assertEqual(len(self.ask_requests()), before, "a stale retry spent anyway")
        self.assertEqual(self.status(job_id)["attempt"], 1)

    def test_a_missing_session_also_blocks_the_retry(self):
        job_id, first = self.mixed_job()
        before = len(self.ask_requests())
        session = self.home / ".tny" / "sessions" / first["items"][0]["session_id"]
        if not session.is_dir():
            candidates = list(
                (self.home / ".tny").rglob(first["items"][0]["session_id"])
            )
            self.assertTrue(candidates, "the child session was not stored")
            session = candidates[0]
        for child in session.rglob("*"):
            if child.is_file():
                child.unlink()
        run = self.run_tny("jobs", "retry", job_id, "--failed", "--json", check=False)
        self.assertEqual(run.returncode, 2, run.stdout)
        self.assertIn("JOB_STALE_SUCCESS", run.stdout.decode())
        self.assertEqual(len(self.ask_requests()), before)

    def test_retrying_a_privacy_opted_out_item_is_refused_with_guidance(self):
        run, payload = self.submit(
            "ask", "--no-store-request", "--prompt", "SECRET sentinel prompt"
        )
        job_id = payload["id"]
        record = self.await_terminal(job_id)
        self.assertEqual(record["state"], "succeeded", record)
        directory = Path(record["metadata_path"]).parent
        stored = json.loads((directory / "job.json").read_text())
        self.assertIsNone(stored["items"][0]["request"])
        blob = "\n".join(
            path.read_text(errors="replace")
            for path in directory.rglob("*")
            if path.is_file()
        )
        self.assertNotIn(
            "SECRET sentinel prompt", blob, "an opted-out prompt was stored"
        )
        self.assertNotIn(API_KEY, blob, "a credential reached the job directory")
        self.assertNotIn(TOKEN, blob)
        # Nothing to replay: the retry must say so instead of inventing one.
        self.run_tny("jobs", "cancel", job_id, check=False)
        run = self.run_tny(
            "jobs", "retry", job_id, "--items", "0", "--json", check=False
        )
        self.assertEqual(run.returncode, 1, run.stdout)


class JobsRetryContention(JobsFixture):
    """Two real retries of one finished job (contract A14).

    The barrier is the job's own `state.lock`: both contenders clear their
    unlocked preconditions and then block inside the production transaction,
    which is exactly the window the review found. Nothing is mocked.
    """

    hold = 0.2

    def failed_mixed_job(self):
        request = self.batch_request(
            "ask", [{"prompt": "keeper"}, {"prompt": "FAIL HOLD one"}], concurrency=2
        )
        _run, payload = self.submit("batch", "--request", request)
        record = self.await_terminal(payload["id"])
        self.assertEqual(
            [i["state"] for i in record["items"]], ["succeeded", "failed"], record
        )
        return payload["id"], record

    def test_simultaneous_retries_start_exactly_one_new_attempt(self):
        job_id, first = self.failed_mixed_job()
        directory = Path(first["metadata_path"]).parent
        keeper = first["items"][0]
        keeper_before = len([r for r in self.ask_requests() if "keeper" in r])
        retried_before = len([r for r in self.ask_requests() if "FAIL HOLD one" in r])
        self.state["fail_ask"] = False
        self.state["hold"] = 6.0

        barrier = self.lock_held(directory / "state.lock")
        contenders = [
            self.spawn_tny("jobs", "retry", job_id, "--failed", "--json")
            for _ in range(2)
        ]
        time.sleep(0.6)  # both are past their unlocked checks by now
        fcntl.flock(barrier.fileno(), fcntl.LOCK_UN)
        barrier.close()
        outputs = [contender.communicate(timeout=120) for contender in contenders]
        codes = [contender.returncode for contender in contenders]
        self.assertEqual(
            codes.count(0), 1, f"not exactly one winner: {codes} {outputs}"
        )
        self.assertEqual(
            sorted(codes),
            [0, 1],
            f"a losing retry was accepted before ownership refusal: {codes} {outputs}",
        )
        self.assertEqual(
            outputs[codes.index(1)][0],
            b"",
            "a refused retry returned an accepted attempt",
        )

        # A losing contender must never write immutable history for an attempt
        # that is still in flight.
        self.assertFalse(
            (directory / "attempt-2.json").exists(),
            "a contender snapshotted a running attempt as finished history",
        )
        snapshot = json.loads((directory / "attempt-1.json").read_text())
        self.assertEqual(snapshot["attempt"], 1, snapshot)
        self.assertEqual(snapshot["state"], "failed", snapshot)

        record = self.await_terminal(job_id, timeout=120)
        self.assertEqual(record["attempt"], 2, record)
        self.assertEqual(
            [i["state"] for i in record["items"]], ["succeeded", "succeeded"], record
        )
        self.assertEqual(
            len([r for r in self.ask_requests() if "FAIL HOLD one" in r]),
            retried_before + 1,
            "the second contender bought a second provider execution",
        )
        self.assertEqual(
            len([r for r in self.ask_requests() if "keeper" in r]),
            keeper_before,
            "a carried success was re-executed",
        )
        carried = record["items"][0]
        self.assertEqual(carried["session_id"], keeper["session_id"])
        self.assertEqual(carried["result_sha256"], keeper["result_sha256"])
        self.assertEqual(carried["log_sha256"], keeper["log_sha256"])
        self.assertEqual(carried["carried_from_attempt"], 1)

    def test_a_retry_that_cannot_take_ownership_leaves_the_record_untouched(self):
        """Failure before acceptance is byte-identical metadata (A14)."""
        job_id, first = self.failed_mixed_job()
        directory = Path(first["metadata_path"]).parent
        before_bytes = (directory / "job.json").read_bytes()
        before_files = sorted(p.name for p in directory.iterdir())
        before_requests = len(self.ask_requests())
        self.state["fail_ask"] = False

        owner = self.lock_held(directory / "owner.lock")
        try:
            run = self.run_tny(
                "jobs", "retry", job_id, "--failed", "--json", check=False
            )
        finally:
            fcntl.flock(owner.fileno(), fcntl.LOCK_UN)
            owner.close()
        self.assertNotEqual(run.returncode, 0, run.stdout)
        self.assertEqual(
            (directory / "job.json").read_bytes(),
            before_bytes,
            "a refused retry rewrote the job record",
        )
        self.assertEqual(sorted(p.name for p in directory.iterdir()), before_files)
        self.assertEqual(
            len(self.ask_requests()), before_requests, "a refused retry spent anyway"
        )

    def test_a_record_that_changed_under_the_retry_is_not_overwritten(self):
        """Ownership is not the only guard: the transaction refuses to commit
        against a record that is no longer the one it prepared from.

        The retry takes ownership, reads the record, and then blocks on the
        real state lock this test holds. While it waits, the record moves on —
        exactly what a concurrent writer does. Releasing the lock must produce
        a refusal, not a silent overwrite of the newer document.
        """
        job_id, first = self.failed_mixed_job()
        directory = Path(first["metadata_path"]).parent
        metadata = directory / "job.json"
        before_requests = len(self.ask_requests())
        self.state["fail_ask"] = False

        barrier = self.lock_held(directory / "state.lock")
        contender = self.spawn_tny("jobs", "retry", job_id, "--failed", "--json")
        time.sleep(0.6)  # it owns the job and is waiting for the state lock
        moved_on = json.loads(metadata.read_text())
        moved_on["revision"] = moved_on["revision"] + 1
        moved_on["items"][1]["error"] = "a concurrent writer got here first"
        metadata.write_text(json.dumps(moved_on))
        fcntl.flock(barrier.fileno(), fcntl.LOCK_UN)
        barrier.close()
        _out, err = contender.communicate(timeout=120)
        self.assertNotEqual(contender.returncode, 0, _out)
        self.assertIn(b"changed", err)

        after = json.loads(metadata.read_text())
        self.assertEqual(after["revision"], moved_on["revision"], after)
        self.assertEqual(after["attempt"], 1, after)
        self.assertEqual(
            after["items"][1]["error"], "a concurrent writer got here first", after
        )
        self.assertFalse((directory / "attempt-1.json").exists())
        self.assertEqual(len(self.ask_requests()), before_requests)

    def test_no_contender_leaves_an_ownerless_queued_attempt(self):
        """Whatever happens, a retried job never rests as an unowned promise."""
        job_id, first = self.failed_mixed_job()
        directory = Path(first["metadata_path"]).parent
        self.state["fail_ask"] = False
        barrier = self.lock_held(directory / "state.lock")
        contenders = [
            self.spawn_tny("jobs", "retry", job_id, "--failed", "--json")
            for _ in range(3)
        ]
        time.sleep(0.6)
        fcntl.flock(barrier.fileno(), fcntl.LOCK_UN)
        barrier.close()
        for contender in contenders:
            contender.communicate(timeout=120)
        record = self.await_terminal(job_id, timeout=120)
        self.assertIn(record["state"], TERMINAL, record)
        for item in record["items"]:
            self.assertIn(item["state"], TERMINAL, record)
            if item["state"] != "cancelled":
                self.assertIsNotNone(item["started"], record)
        self.assertEqual(record["attempt"], 2, record)


class JobsCredentialIsolation(JobsFixture):
    """Chat and image credentials are separate allowances (contract A14).

    The chat side here is an OpenAI-compatible key; the image side is a
    ChatGPT account. Both are fixture strings. The ask child's environment is
    captured from inside that child by a real tool call, never by reading this
    process's environment.
    """

    hold = 0.2

    def test_an_ask_item_child_never_receives_the_image_allowance(self):
        captured, record = self.child_environment(
            "jobs", "submit", "ask", stdin=b"ENVDUMP one"
        )
        self.assertEqual(record["state"], "succeeded", record)
        present = sorted(captured)
        for name in IMAGE_ONLY_ENV:
            self.assertNotIn(name, present, f"{name} reached an ask item child")
        blob = "\n".join(f"{k}={v}" for k, v in captured.items())
        self.assertNotIn(TOKEN, blob, "the image token reached an ask item child")
        self.assertNotIn(ACCOUNT, blob, "the image account reached an ask item child")
        # The chat credential it does need is still there, through the private
        # carrier the argv names — never on the command line itself.
        self.assertEqual(captured.get("TNY_JOB_API_KEY"), API_KEY, present)
        self.assertNotIn(API_KEY, " ".join(pids_argv("jobs _worker", self.home.name)))

    def test_an_explicitly_selected_image_account_never_reaches_an_ask_child(self):
        """Two different accounts: the image allowance is chosen on the
        command line, the conversation uses an unrelated chat key. The ask
        item child must see neither image account, ambient or explicit."""
        self.state["image_auth"] = [(FLAG_TOKEN, FLAG_ACCOUNT)]
        captured, record = self.child_environment(
            "--chatgpt-token",
            FLAG_TOKEN,
            "--chatgpt-account-id",
            FLAG_ACCOUNT,
            "jobs",
            "submit",
            "ask",
            stdin=b"ENVDUMP two",
        )
        self.assertEqual(record["state"], "succeeded", record)
        for name in IMAGE_ONLY_ENV:
            self.assertNotIn(name, sorted(captured), f"{name} reached an ask child")
        blob = "\n".join(f"{k}={v}" for k, v in captured.items())
        for secret in (FLAG_TOKEN, FLAG_ACCOUNT, TOKEN, ACCOUNT):
            self.assertNotIn(secret, blob, "an image account reached an ask child")

        # …and that same explicit allowance is exactly what an image item uses,
        # so the isolation is a split and not a deletion.
        target = self.workspace / "flagged.png"
        run = self.run_tny(
            "--chatgpt-token",
            FLAG_TOKEN,
            "--chatgpt-account-id",
            FLAG_ACCOUNT,
            "jobs",
            "submit",
            "image",
            "--output-file",
            str(target),
            "--prompt",
            "a flagged square",
            "--json",
        )
        payload = json.loads(run.stdout.decode())
        image = self.await_terminal(payload["id"])
        self.assertEqual(image["state"], "succeeded", image)
        headers = self.headers_of("image")
        self.assertTrue(headers, self.state["headers"])
        for request in headers:
            self.assertEqual(request["authorization"], f"Bearer {FLAG_TOKEN}")
            self.assertEqual(request["account"], FLAG_ACCOUNT)

    def test_an_ask_item_on_the_codex_provider_keeps_its_own_chat_account(self):
        """The fix must not be 'delete the token': a Codex ask job is a
        legitimate ChatGPT chat request with its account header."""
        run = self.run_tny(
            "jobs", "submit", "ask", "--json", stdin=b"hello codex", provider="codex"
        )
        payload = json.loads(run.stdout.decode())
        record = self.await_terminal(payload["id"])
        self.assertEqual(record["state"], "succeeded", record)
        chat = self.headers_of("codex-chat")
        self.assertTrue(chat, self.state["headers"])
        for request in chat:
            self.assertEqual(request["authorization"], f"Bearer {TOKEN}")
            self.assertEqual(request["account"], ACCOUNT)
        # …and the chat side never borrowed the OpenAI-compatible chat key.
        self.assertFalse([h for h in chat if h["authorization"] == f"Bearer {API_KEY}"])

    def test_an_image_item_uses_the_image_account_and_no_chat_key(self):
        output = self.workspace / "isolated.png"
        run, payload = self.submit(
            "image", "--prompt", "a plain square", "--output-file", str(output)
        )
        record = self.await_terminal(payload["id"])
        self.assertEqual(record["state"], "succeeded", record)
        images = self.headers_of("image")
        self.assertTrue(images, self.state["headers"])
        for request in images:
            self.assertEqual(request["authorization"], f"Bearer {TOKEN}")
            self.assertEqual(request["account"], ACCOUNT)
            self.assertNotIn(API_KEY, request["authorization"] or "")

    def test_named_and_explicit_chat_keys_are_resolved_into_owned_carriers(self):
        settings = self.home / ".tny" / "settings.json"
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text(
            json.dumps(
                {
                    "fixture_named": {
                        "base_url": self.env["OPENAI_BASE_URL"],
                        "api_key_env": "ODD_CARRIER",
                        "wire_api": "chat",
                    }
                }
            )
        )
        for explicit in (False, True):
            with self.subTest(explicit=explicit):
                env = dict(self.env, ODD_CARRIER=API_KEY, EXPLICIT_SECRET=API_KEY)
                flags = ("--api-key-env", "EXPLICIT_SECRET") if explicit else ()
                run = self.run_tny(
                    *flags,
                    "jobs",
                    "submit",
                    "ask",
                    "--json",
                    stdin=b"hello named provider",
                    env=env,
                    provider="fixture_named",
                )
                final = self.await_terminal(json.loads(run.stdout)["id"])
                self.assertEqual(final["state"], "succeeded", final)
        self.assertTrue(self.headers_of("chat"))
        self.assertTrue(
            all(
                h["authorization"] == f"Bearer {API_KEY}"
                for h in self.headers_of("chat")
            )
        )

    def test_declared_ask_tool_environment_preserved_and_image_alias_refused(self):
        settings = self.home / ".tny" / "settings.json"
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text(json.dumps({"jobs": {"ask_env": ["DECLARED_TOOL_TOKEN"]}}))
        self.env["DECLARED_TOOL_TOKEN"] = "fixture-tool-token-not-real"
        captured, final = self.child_environment(
            "jobs", "submit", "ask", stdin=b"ENVDUMP declared"
        )
        self.assertEqual(final["state"], "succeeded", final)
        self.assertEqual(
            captured.get("DECLARED_TOOL_TOKEN"), "fixture-tool-token-not-real"
        )
        count = len(self.ask_requests())
        self.env["DECLARED_TOOL_TOKEN"] = TOKEN
        run, _ = self.submit(
            "ask", "--prompt", "must not borrow image token", check=False
        )
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual(len(self.ask_requests()), count)

    def test_declared_ask_carrier_with_operational_name_does_not_reach_image(self):
        settings = self.home / ".tny" / "settings.json"
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text(json.dumps({"jobs": {"ask_env": ["LANG"]}}))
        library, marker = (
            self.workspace / "image-env.so",
            self.workspace / "image-env-result",
        )
        subprocess.run(
            [
                "cc",
                "-D_DEFAULT_SOURCE",
                "-DTNY_FIXTURE_IMAGE_ENV",
                "-fPIC",
                "-dynamiclib" if sys.platform == "darwin" else "-shared",
                str(ROOT / "tests/fixtures/jobs_launch_barrier.c"),
                "-o",
                str(library),
                "-ldl",
            ],
            check=True,
            capture_output=True,
        )
        loader = "DYLD_INSERT_LIBRARIES" if sys.platform == "darwin" else "LD_PRELOAD"
        env = dict(
            self.env,
            LANG="fixture-tool-token-not-real",
            **{loader: str(library), "TNY_FIXTURE_PIPE_READY": str(marker)},
        )
        run = self.run_tny(
            "jobs",
            "submit",
            "image",
            "--output-file",
            str(self.workspace / "env.png"),
            "--prompt",
            "only the image allowance",
            "--json",
            env=env,
        )
        final = self.await_terminal(json.loads(run.stdout)["id"])
        self.assertEqual(final["state"], "succeeded", final)
        self.assertEqual(marker.read_text(), "absent")

    def test_no_credential_reaches_argv_or_the_job_directory(self):
        output = self.workspace / "quiet.png"
        _run, payload = self.submit(
            "image", "--prompt", "HOLD a plain square", "--output-file", str(output)
        )
        job_id = payload["id"]
        self.await_state(job_id, lambda r: r["items"][0]["state"] == "running")
        argv = " ".join(pids_argv(self.home.name))
        self.assertNotIn(TOKEN, argv, "a credential reached a command line")
        self.assertNotIn(ACCOUNT, argv)
        self.assertNotIn(API_KEY, argv)
        record = self.await_terminal(job_id)
        directory = Path(record["metadata_path"]).parent
        blob = "\n".join(
            path.read_text(errors="replace")
            for path in directory.rglob("*")
            if path.is_file() and path.suffix != ".png"
        )
        for secret in (TOKEN, ACCOUNT, API_KEY):
            self.assertNotIn(secret, blob, "a credential reached the job directory")


class JobsWaiting(JobsFixture):
    hold = 8.0

    def test_a_wait_timeout_exits_124_and_never_cancels(self):
        _run, payload = self.submit("ask", stdin=b"HOLD waiting")
        job_id = payload["id"]
        started = time.time()
        run = self.run_tny(
            "jobs", "wait", job_id, "--timeout", "1", "--json", check=False
        )
        self.assertEqual(run.returncode, 124, run.stdout)
        self.assertLess(time.time() - started, 6)
        record = self.status(job_id)
        self.assertIn(record["state"], ("queued", "running"), record)
        self.assertFalse(record["cancel_requested"], record)
        record = self.await_terminal(job_id)
        self.assertEqual(record["state"], "succeeded", record)

    def test_wait_returns_the_terminal_state_and_its_exit_code(self):
        self.state["hold"] = 0.2
        _run, payload = self.submit("ask", stdin=b"FAIL for wait")
        run = self.run_tny(
            "jobs", "wait", payload["id"], "--timeout", "60", "--json", check=False
        )
        self.assertEqual(run.returncode, 2, run.stdout)
        self.assertEqual(json.loads(run.stdout.decode())["state"], "failed")


class JobsOutputs(JobsFixture):
    hold = 2.0

    def test_an_image_job_commits_a_verified_output_and_records_its_hash(self):
        target = self.workspace / "robot.png"
        _run, payload = self.submit(
            "image", "--output-file", str(target), "--prompt", "an orange robot"
        )
        record = self.await_terminal(payload["id"])
        self.assertEqual(record["state"], "succeeded", record)
        item = record["items"][0]
        # The record keys on the canonical path (macOS resolves /var -> /private/var).
        self.assertEqual(item["output_path"], os.path.realpath(target))
        self.assertTrue(target.is_file())
        self.assertEqual(item["output_bytes"], target.stat().st_size)
        import hashlib

        self.assertEqual(
            item["output_sha256"], hashlib.sha256(target.read_bytes()).hexdigest()
        )
        self.assertEqual(target.read_bytes(), self.state["image"])
        self.assertEqual(len(self.image_requests()), 1)
        # The integrated image service returns real immutable provenance.
        manifest = Path(item["manifest_path"])
        self.assertTrue(manifest.is_file(), item)
        provenance = json.loads(manifest.read_text())
        self.assertEqual(provenance["status"], "succeeded")
        self.assertTrue(provenance["committed"])
        self.assertEqual(provenance["operation_id"], item["operation_id"])
        self.assertEqual(provenance["artifacts"][0]["path"], item["output_path"])
        self.assertEqual(provenance["artifacts"][0]["sha256"], item["output_sha256"])
        self.assertEqual(provenance["artifacts"][0]["bytes"], item["output_bytes"])

    def test_a_recorded_generation_manifest_is_rechecked_before_a_retry(self):
        self.check_carried_manifest_retry(fixture=False)

    def test_carried_manifest_retry_reader_fixture(self):
        self.check_carried_manifest_retry(fixture=True)

    def check_carried_manifest_retry(self, *, fixture):
        """#127 lineage: a carried image success is only carried while its
        manifest still describes exactly those bytes.

        Real generation and reader-fixture entry points both require the
        integrated service's manifest; missing integration is a failure.
        """
        target = self.workspace / "lineage.png"
        request = self.batch_request(
            "image",
            [
                {"prompt": "a lineage square", "output_file": str(target)},
                {
                    "prompt": "FAIL retry only this item",
                    "output_file": str(self.workspace / "failed.png"),
                },
            ],
            concurrency=2,
        )
        _run, payload = self.submit("batch", "--request", request)
        record = self.await_terminal(payload["id"])
        self.assertEqual([i["state"] for i in record["items"]], ["succeeded", "failed"])
        item = record["items"][0]
        if fixture and not item["manifest_path"]:
            # Reader fixture only, not a replacement generation implementation.
            manifest = self.workspace / "reader-fixture.json"
            manifest.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "kind": "image_manifest",
                        "committed": True,
                        "operation_id": "fixture-op",
                        "artifacts": [
                            {
                                "path": item["output_path"],
                                "sha256": item["output_sha256"],
                            }
                        ],
                    }
                )
            )
            item["manifest_path"] = str(manifest)
            metadata = Path(record["metadata_path"])
            stored = json.loads(metadata.read_text())
            stored["items"][0]["manifest_path"] = str(manifest)
            metadata.write_text(json.dumps(stored))
        self.assertIsNotNone(
            item["manifest_path"], "real image manifest integration is required"
        )
        manifest = Path(item["manifest_path"])
        self.assertTrue(manifest.is_file(), item)
        before = len(self.image_requests())
        original = manifest.read_text()
        corrupt = original.replace(item["output_sha256"], "0" * 64)
        self.assertNotEqual(
            original, corrupt, "manifest hash corruption was not applied"
        )
        manifest.write_text(corrupt)
        metadata = Path(record["metadata_path"])
        old = metadata.read_bytes()
        self.state["fail_image"] = False
        run = self.run_tny(
            "jobs", "retry", payload["id"], "--items", "1", "--json", check=False
        )
        self.assertEqual(run.returncode, 2, run.stdout)
        self.assertEqual(json.loads(run.stdout)["code"], "JOB_STALE_SUCCESS")
        self.assertEqual(metadata.read_bytes(), old)
        self.assertEqual(
            len(self.image_requests()), before, "a stale lineage spent anyway"
        )

    def test_two_submitters_for_one_output_leave_only_one_paying(self):
        target = self.workspace / "contested.png"
        self.state["hold"] = 4.0
        first = subprocess.Popen(
            [
                TNY,
                "--provider",
                "openai",
                "jobs",
                "submit",
                "image",
                "--output-file",
                str(target),
                "--prompt",
                "HOLD contested",
                "--json",
            ],
            cwd=self.workspace,
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
        self.started.append(first)
        out, _err = first.communicate(timeout=60)
        self.assertEqual(first.returncode, 0, out)
        winner = json.loads(out.decode())["id"]
        second = self.run_tny(
            "jobs",
            "submit",
            "image",
            "--output-file",
            str(target),
            "--prompt",
            "HOLD contested too",
            "--json",
            check=False,
        )
        self.assertEqual(second.returncode, 1, second.stdout)
        self.assertIn(b"output", second.stderr.lower())
        record = self.await_terminal(winner, timeout=90)
        self.assertEqual(record["state"], "succeeded", record)
        self.assertEqual(len(self.image_requests()), 1, self.image_requests())

    def test_aliased_and_existing_destinations_are_refused_before_any_request(self):
        real = self.workspace / "original.png"
        real.write_bytes(b"original bytes")
        link = self.workspace / "link.png"
        link.symlink_to(real)
        hard = self.workspace / "hard.png"
        os.link(real, hard)
        cases = [
            (str(link), "--overwrite"),
            (str(hard), "--overwrite"),
            (str(real), None),  # existing file without an explicit grant
        ]
        for destination, extra in cases:
            args = ["image", "--output-file", destination, "--prompt", "alias"]
            if extra:
                args.append(extra)
            run, _payload = self.submit(*args, check=False)
            self.assertEqual(run.returncode, 1, (destination, run.stdout, run.stderr))
        self.assertEqual(
            self.image_requests(), [], "an aliased output reached the provider"
        )
        self.assertEqual(real.read_bytes(), b"original bytes")

    def test_two_items_in_one_batch_cannot_share_an_output(self):
        target = str(self.workspace / "shared.png")
        request = self.batch_request(
            "image",
            [
                {"prompt": "one", "output_file": target},
                {"prompt": "two", "output_file": target},
            ],
        )
        run, _payload = self.submit("batch", "--request", request, check=False)
        self.assertEqual(run.returncode, 1, run.stdout)
        self.assertEqual(self.image_requests(), [])
        self.assertEqual(self.job_dirs(), [])


class JobsOutputLimit(JobsFixture):
    hold = 0.1

    def test_a_runaway_item_fails_explicitly_instead_of_truncating_into_success(self):
        self.state["flood_chunks"] = (
            80  # ~5 MiB of text deltas, over the 4 MiB log bound
        )
        _run, payload = self.submit("ask", stdin=b"FLOOD please")
        record = self.await_terminal(payload["id"], timeout=180)
        item = record["items"][0]
        self.assertEqual(item["state"], "failed", record)
        self.assertEqual(item["error_code"], "JOB_OUTPUT_LIMIT", record)
        self.assertIsNone(item["session_id"], "a truncated item claimed a session")
        self.assertEqual(record["state"], "failed")


class JobsPermissions(JobsFixture):
    hold = 0.2

    def write_settings(self, permission):
        directory = self.home / ".tny"
        directory.mkdir(exist_ok=True)
        (directory / "settings.json").write_text(json.dumps({"permission": permission}))

    def ask_with_tool(self, tool, arguments, permission, mode="ask"):
        """One real turn whose fixture model calls exactly one tool."""
        self.write_settings(permission)
        handler_state = self.state
        handler_state["tool_call"] = (tool, arguments)
        original = Handler.chat

        def chat(handler, prompt, after_tool=False):
            body_tool, body_args = handler.server.state["tool_call"]
            if handler.server.state.get("answered"):
                frames = [
                    {"choices": [{"index": 0, "delta": {"content": "done"}}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]},
                ]
            else:
                handler.server.state["answered"] = True
                frames = [
                    {
                        "choices": [
                            {
                                "index": 0,
                                "delta": {
                                    "tool_calls": [
                                        {
                                            "index": 0,
                                            "id": "call_1",
                                            "type": "function",
                                            "function": {
                                                "name": body_tool,
                                                "arguments": json.dumps(body_args),
                                            },
                                        }
                                    ]
                                },
                            }
                        ]
                    },
                    {
                        "choices": [
                            {"index": 0, "delta": {}, "finish_reason": "tool_calls"}
                        ]
                    },
                ]
            data = (
                "".join(f"data: {json.dumps(f)}\n\n" for f in frames)
                + "data: [DONE]\n\n"
            ).encode()
            handler.reply(200, "text/event-stream", data)

        Handler.chat = chat
        try:
            env = dict(self.env, TNY_TOOLS="all")
            return self.run_tny(
                "--permission-mode",
                mode,
                "ask",
                "--ephemeral",
                "--json",
                "run the tool",
                env=env,
                check=False,
                timeout=120,
            )
        finally:
            Handler.chat = original
            self.state.pop("answered", None)

    def tool_results(self):
        return [
            message["content"]
            for body in self.state["bodies"]
            for message in body.get("messages", [])
            if message.get("role") == "tool"
        ]

    def tool_statuses(self, run):
        return json.loads(run.stdout.decode()).get("tool_calls", [])

    def test_a_status_grant_never_authorizes_a_submit(self):
        run = self.ask_with_tool(
            "job_submit",
            {"kind": "ask", "items": [{"prompt": "should never run"}]},
            {"job_status": "allow", "bash": "allow"},
        )
        # Unresolved in ask mode: the exact identity and detail are what the
        # engine offered for approval, and nothing ran.
        self.assertIn("job_submit submit kind=ask", run.stderr.decode(), run.stderr)
        self.assertEqual(
            self.tool_statuses(run), [{"name": "job_submit", "status": "error"}]
        )
        self.assertEqual(run.returncode, 2)
        self.assertEqual(self.job_dirs(), [], "a denied submit created a job")
        self.assertEqual(
            [r for r in self.ask_requests() if "should never run" in r], []
        )

    def test_the_intercepted_command_uses_the_same_job_identity(self):
        command = 'tny jobs submit ask --prompt "intercepted"'
        # bash is allowed in both runs: only the job identity's rule changes.
        denied = self.ask_with_tool(
            "terminal", {"command": command}, {"bash": "allow", "job_submit": "deny"}
        )
        self.assertEqual(denied.returncode, 2, denied.stdout)
        self.assertEqual(
            self.tool_statuses(denied), [{"name": "terminal", "status": "error"}]
        )
        self.assertEqual(self.job_dirs(), [], "a denied interception created a job")
        self.assertEqual([r for r in self.ask_requests() if "intercepted" in r], [])

        self.state["bodies"].clear()
        allowed = self.ask_with_tool(
            "terminal", {"command": command}, {"bash": "allow", "job_submit": "allow"}
        )
        self.assertEqual(allowed.returncode, 0, allowed.stderr)
        self.assertEqual(len(self.job_dirs()), 1, allowed.stderr.decode()[-2000:])
        # The intercepted command ran in-process on the shared service: the
        # result is the service's own output, not a shell transcript.
        results = "\n".join(self.tool_results())
        self.assertIn("job ", results)
        self.assertIn("metadata:", results)
        self.await_terminal(self.job_dirs()[0].name)

    def test_an_allowed_typed_submit_creates_exactly_one_job(self):
        run = self.ask_with_tool(
            "job_submit",
            {"kind": "ask", "items": [{"prompt": "allowed work"}]},
            {"job_submit": "allow", "job_status": "allow", "bash": "allow"},
        )
        self.assertEqual(len(self.job_dirs()), 1, run.stderr.decode()[-2000:])
        results = "\n".join(self.tool_results())
        self.assertIn('"kind":"job"', results, results[:400])
        job_id = self.job_dirs()[0].name
        record = self.await_terminal(job_id)
        self.assertEqual(record["state"], "succeeded", record)

    def test_an_unknown_job_action_is_refused_before_any_work(self):
        run = self.ask_with_tool(
            "job_status",
            {"action": "submit", "id": "0" * 32},
            {"job_status": "allow"},
        )
        results = "\n".join(self.tool_results())
        self.assertIn("does not support that action", results, results[:400])
        self.assertEqual(self.tool_statuses(run)[0]["status"], "error")
        self.assertEqual(self.job_dirs(), [])


class JobsSchemaSurface(JobsFixture):
    hold = 0.1

    def advertised_tools(self, **env):
        self.state["bodies"].clear()
        self.run_tny(
            "ask",
            "--ephemeral",
            "--json",
            "hello",
            env=dict(self.env, **env),
            check=False,
        )
        self.assertTrue(self.state["bodies"], "the fixture saw no request")
        return [t["function"]["name"] for t in self.state["bodies"][0].get("tools", [])]

    def test_the_job_tools_are_advertised_in_the_all_profile(self):
        names = self.advertised_tools(TNY_TOOLS="all")
        for tool in ("job_submit", "job_control", "job_status"):
            self.assertIn(tool, names, names)

    def test_restricted_profiles_do_not_advertise_job_tools(self):
        names = self.advertised_tools(TNY_TOOLS="terminal")
        for tool in ("job_submit", "job_control", "job_status"):
            self.assertNotIn(tool, names, names)


class JobsWorkerHandshake(JobsFixture):
    hold = 0.2

    def test_a_forged_supervisor_invocation_cannot_take_over_a_job(self):
        """`jobs _worker ID` without the inherited owner lock does nothing."""
        _run, payload = self.submit("ask", "--prompt", "handshake")
        job_id = payload["id"]
        self.await_terminal(job_id)
        metadata = Path(payload["metadata_path"])
        before = metadata.read_bytes()
        forged = subprocess.run(
            [TNY, "jobs", "_worker", job_id],
            cwd=self.workspace,
            env=self.env,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=60,
        )
        self.assertNotEqual(forged.returncode, 0, forged.stdout)
        self.assertEqual(
            metadata.read_bytes(), before, "a forged worker rewrote the record"
        )

    def test_repeated_invalid_admission_preserves_protected_state(self):
        _run, payload = self.submit("ask", "--prompt", "admission owner fixture")
        self.await_terminal(payload["id"])
        metadata = Path(payload["metadata_path"])
        before = metadata.read_bytes()
        requests = len(self.ask_requests())
        owner = (metadata.parent / "owner.lock").open("r+b")
        fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            for _ in range(12):
                refused = self.run_tny("jobs", "_worker", payload["id"], check=False)
                self.assertNotEqual(refused.returncode, 0)
                self.assertEqual(metadata.read_bytes(), before)
            self.assertEqual(len(self.ask_requests()), requests)
        finally:
            owner.close()

    def test_a_supervisor_without_a_complete_payload_never_spends(self):
        """An incomplete payload is a truthful failed setup, not a request."""
        _run, payload = self.submit("ask", "--prompt", "complete payload")
        job_id = payload["id"]
        self.await_terminal(job_id)
        before = len(self.ask_requests())
        forged = subprocess.run(
            [TNY, "jobs", "_worker", job_id],
            cwd=self.workspace,
            env=self.env,
            input=b'{"version":1,"job":"' + job_id.encode() + b'"',  # truncated JSON
            capture_output=True,
            timeout=60,
        )
        self.assertNotEqual(forged.returncode, 0)
        self.assertEqual(
            len(self.ask_requests()), before, "an invalid payload spent anyway"
        )


class JobsReviewedRaces(JobsFixture):
    """Real owner descriptions, competing processes, and immutable history."""

    hold = 0.2

    def finished_failure(self):
        _, submitted = self.submit("ask", stdin=b"FAIL reviewed attempt")
        record = self.await_terminal(submitted["id"])
        self.assertEqual(record["state"], "failed", record)
        directory = Path(record["metadata_path"]).parent
        # Terminal projection precedes owner close. Wait for the real close.
        self.wait_owner(directory, held=False)
        self.prior_requests = len(self.ask_requests())
        return submitted["id"], directory

    def wait_owner(self, directory, *, held):
        with (directory / "owner.lock").open("r+b") as probe:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                try:
                    fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    if held:
                        return
                else:
                    fcntl.flock(probe, fcntl.LOCK_UN)
                    if not held:
                        return
                time.sleep(0.01)
        self.fail("owner did not reach the required barrier state")

    def private_worker(self, job_id, owner, *, env=None, stdout=subprocess.PIPE):
        # Map the actual test-owned description in a fresh helper process.
        # No preexec_fn runs in this threaded test runner.
        script = (
            "import os,sys,signal; signal.signal(signal.SIGPIPE,signal.SIG_DFL); fd=int(sys.argv[3]); "
            "os.dup2(fd,3,inheritable=True); "
            "os.execv(sys.argv[1],[sys.argv[1],'jobs','_worker',sys.argv[2]])"
        )
        process = subprocess.Popen(
            [sys.executable, "-c", script, TNY, job_id, str(owner.fileno())],
            cwd=self.workspace,
            env=env or self.env,
            pass_fds=(owner.fileno(),),
            stdin=subprocess.PIPE,
            stdout=stdout,
            stderr=subprocess.PIPE,
        )
        self.started.append(process)
        return process

    def queued_owned(self):
        job_id, directory = self.finished_failure()
        owner = self.lock_held(directory / "owner.lock")
        self.addCleanup(owner.close)
        path = directory / "job.json"
        record = json.loads(path.read_text())
        record.update(attempt=2, state="queued", cleanup="pending")
        item = record["items"][0]
        item.update(
            attempt=2,
            state="queued",
            cancel_requested=False,
            log_path=str(directory / "attempt-2-item-0.log"),
        )
        path.write_text(json.dumps(record))
        payload = {
            "version": 1,
            "job": job_id,
            "attempt": 2,
            "kind": "ask",
            "self": TNY,
            "cwd": str(self.workspace),
            "provider": "openai",
            "perm_mode": "yolo",
            "tools": "all",
            "concurrency": 1,
            "chat": {
                "api_key": API_KEY,
                "base_url": self.env["OPENAI_BASE_URL"],
                "wire_api": "chat",
                "model": "mock-model",
            },
            "image": {"token": TOKEN, "account": ACCOUNT},
            "items": [{"index": 0, "prompt": "reviewed private handshake"}],
        }
        return job_id, directory, owner, payload

    def test_accepted_launch_failure_keeps_owner_through_finalization(self):
        self.check_accepted_launch_failure(retry=False)

    def test_accepted_retry_failure_keeps_owner_through_finalization(self):
        self.check_accepted_launch_failure(retry=True)

    def check_accepted_launch_failure(self, *, retry):
        job_id = self.finished_failure()[0] if retry else None
        requests_before = len(self.ask_requests())
        library = self.workspace / "launch-barrier.so"
        command = [
            "cc",
            "-D_DEFAULT_SOURCE",
            "-fPIC",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-dynamiclib" if sys.platform == "darwin" else "-shared",
            str(ROOT / "tests/fixtures/jobs_launch_barrier.c"),
            "-o",
            str(library),
        ]
        subprocess.run(command, check=True, capture_output=True)
        ready, resume = self.workspace / "pipe-ready", self.workspace / "pipe-resume"
        loader = "DYLD_INSERT_LIBRARIES" if sys.platform == "darwin" else "LD_PRELOAD"
        env = dict(
            self.env,
            **{
                loader: str(library),
                "TNY_FIXTURE_PIPE_READY": str(ready),
                "TNY_FIXTURE_PIPE_RESUME": str(resume),
            },
        )
        args = (
            ("retry", job_id, "--failed")
            if retry
            else ("submit", "ask", "--prompt", "never launched")
        )
        submitter = self.spawn_tny("jobs", *args, "--json", env=env)
        self.started.append(submitter)
        deadline = time.monotonic() + 10
        while not ready.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(
            ready.exists(), "the launch fault did not reach its pipe barrier"
        )
        directories = self.job_dirs()
        self.assertEqual(len(directories), 1)
        directory = directories[0]
        before = (directory / "job.json").read_bytes()
        self.assertEqual(json.loads(before)["state"], "queued")
        barrier = self.lock_held(directory / "state.lock")
        try:
            resume.touch()
            # The accepted submitter now fails pipe creation and waits to
            # finalize under the state lock. Its owner must remain held.
            time.sleep(0.2)
            self.assertIsNone(submitter.poll())
            with (directory / "owner.lock").open("r+b") as probe:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
            loser = self.run_tny(
                "jobs", "retry", directory.name, "--failed", "--json", check=False
            )
            self.assertNotEqual(loser.returncode, 0)
            self.assertEqual((directory / "job.json").read_bytes(), before)
        finally:
            barrier.close()
        submitter.communicate(timeout=20)
        self.assertNotEqual(submitter.returncode, 0)
        final = self.status(directory.name)
        self.assertEqual(final["state"], "failed", final)
        self.assertEqual(final["attempt"], 2 if retry else 1)
        self.assertEqual(len(self.ask_requests()), requests_before)

    def test_stopped_payload_reader_is_bounded_before_ack(self):
        self.check_stopped_payload_reader(interrupt=False)

    def test_stopped_payload_reader_can_be_interrupted(self):
        self.check_stopped_payload_reader(interrupt=True)

    def check_stopped_payload_reader(self, *, interrupt):
        library = self.workspace / "stopped-reader.so"
        subprocess.run(
            [
                "cc",
                "-D_DEFAULT_SOURCE",
                "-DTNY_FIXTURE_STOP_READER",
                "-fPIC",
                "-dynamiclib" if sys.platform == "darwin" else "-shared",
                str(ROOT / "tests/fixtures/jobs_launch_barrier.c"),
                "-o",
                str(library),
                "-ldl",
            ],
            check=True,
            capture_output=True,
        )
        marker = self.workspace / "stopped-reader-pid"
        request = self.batch_request(
            "ask",
            [
                {
                    "prompt": "private-handshake-marker" + "x" * 59000,
                    "persist_request": False,
                }
                for _ in range(16)
            ],
            concurrency=1,
        )
        self.assertGreater(Path(request).stat().st_size, 512 * 1024)
        loader = "DYLD_INSERT_LIBRARIES" if sys.platform == "darwin" else "LD_PRELOAD"
        env = dict(
            self.env, **{loader: str(library), "TNY_FIXTURE_PIPE_READY": str(marker)}
        )
        start = time.monotonic()
        submit = self.spawn_tny(
            "jobs", "submit", "batch", "--request", request, "--json", env=env
        )
        self.started.append(submit)
        deadline = time.monotonic() + 10
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(marker.exists(), "worker did not reach stopped-reader barrier")
        worker = int(marker.read_text())
        try:
            if interrupt:
                submit.send_signal(signal.SIGINT)
            stdout, stderr = submit.communicate(timeout=23 if not interrupt else 4)
            elapsed = time.monotonic() - start
            self.assertLess(elapsed, 23 if not interrupt else 6)
            self.assertNotEqual(submit.returncode, 0)
            result = json.loads(stdout)
            self.assertEqual(result["state"], "uncertain", result)
            self.assertEqual(len(self.ask_requests()), 0)
            for value in (API_KEY, TOKEN, "private-handshake-marker"):
                self.assertNotIn(value.encode(), stdout + stderr)
            os.kill(worker, signal.SIGCONT)
            final = self.await_terminal(result["id"], timeout=20)
            self.assertEqual(final["state"], "failed", final)
            self.assertEqual(len(self.ask_requests()), 0)
        finally:
            if running(worker):
                os.kill(worker, signal.SIGCONT)
                os.kill(worker, signal.SIGKILL)

    def test_legacy_image_success_without_manifest_cannot_be_carried(self):
        import copy
        import hashlib

        job_id, directory = self.finished_failure()
        metadata = directory / "job.json"
        record = json.loads(metadata.read_text())
        failed = copy.deepcopy(record["items"][0])
        failed.update(
            index=1,
            request={
                "prompt": "retry failed item",
                "operation": "generate",
                "output_file": str(self.workspace / "failed.png"),
            },
        )
        output = self.workspace / "legacy.png"
        output.write_bytes(png(2, 3))
        keeper = record["items"][0]
        keeper.update(
            state="succeeded",
            output_path=str(output),
            output_bytes=output.stat().st_size,
            output_sha256=hashlib.sha256(output.read_bytes()).hexdigest(),
            manifest_path=None,
        )
        record.update(job_kind="image", items=[keeper, failed])
        metadata.write_text(json.dumps(record))
        before = metadata.read_bytes()
        requests = len(self.state["requests"])
        run = self.run_tny(
            "jobs", "retry", job_id, "--items", "1", "--json", check=False
        )
        self.assertEqual(run.returncode, 2)
        self.assertEqual(json.loads(run.stdout)["code"], "JOB_STALE_SUCCESS")
        self.assertEqual(metadata.read_bytes(), before)
        self.assertEqual(len(self.state["requests"]), requests)

    def test_supplied_descriptor_must_itself_own_the_flock(self):
        job_id, directory, owner, payload = self.queued_owned()
        before = (directory / "job.json").read_bytes()
        with (directory / "owner.lock").open("r+b") as other_description:
            worker = self.private_worker(job_id, other_description)
            worker.communicate(json.dumps(payload).encode(), timeout=20)
        self.assertNotEqual(worker.returncode, 0)
        self.assertEqual((directory / "job.json").read_bytes(), before)
        self.assertFalse((directory / "attempt-2-item-0.log").exists())
        self.assertEqual(len(self.ask_requests()), self.prior_requests)
        owner.close()

    def test_stale_payload_cannot_finalize_the_accepted_new_attempt(self):
        job_id, directory, owner, payload = self.queued_owned()
        before = (directory / "job.json").read_bytes()
        payload["attempt"] = 1
        worker = self.private_worker(job_id, owner)
        worker.communicate(json.dumps(payload).encode(), timeout=20)
        self.assertNotEqual(worker.returncode, 0)
        self.assertEqual((directory / "job.json").read_bytes(), before)
        owner.close()

    def test_closed_ack_reader_does_not_kill_an_accepted_supervisor(self):
        job_id, _directory, owner, payload = self.queued_owned()
        worker = self.private_worker(job_id, owner)
        worker.stdout.close()  # actual EPIPE during the supervisor's ack
        worker.stdout = None
        worker.communicate(json.dumps(payload).encode(), timeout=30)
        owner.close()
        self.assertEqual(worker.returncode, 0)
        self.assertEqual(self.status(job_id)["state"], "succeeded")
        self.assertEqual(len(self.ask_requests()), self.prior_requests + 1)

    def test_live_submitter_death_between_payload_and_ack(self):
        job_id, directory, owner, payload = self.queued_owned()
        payload_path = self.workspace / "fake-private-payload.json"
        # Fake credentials only. This test input is not a job artifact.
        payload_path.write_text(json.dumps(payload))
        child_pid_path = self.workspace / "child-pid"
        ready = self.workspace / "payload-delivered"
        script = """
import os, signal, subprocess, sys, time
binary, job, fd, source, pidfile, ready = sys.argv[1:]
fd = int(fd)
code = "import os,sys,signal; signal.signal(signal.SIGPIPE,signal.SIG_DFL); os.dup2(int(sys.argv[3]),3,inheritable=True); os.execv(sys.argv[1],[sys.argv[1],'jobs','_worker',sys.argv[2]])"
p = subprocess.Popen([sys.executable, '-c', code, binary, job, str(fd)],
                     pass_fds=(fd,), stdin=subprocess.PIPE, stdout=subprocess.PIPE)
open(pidfile, 'w').write(str(p.pid))
p.stdin.write(b' ')
p.stdin.flush()
time.sleep(0.1)
os.kill(p.pid, signal.SIGSTOP)
p.stdin.write(open(source, 'rb').read())
p.stdin.close()
open(ready, 'w').write('ready')
while True: time.sleep(1)
"""
        parent = subprocess.Popen(
            [
                sys.executable,
                "-c",
                script,
                TNY,
                job_id,
                str(owner.fileno()),
                str(payload_path),
                str(child_pid_path),
                str(ready),
            ],
            cwd=self.workspace,
            env=self.env,
            pass_fds=(owner.fileno(),),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.started.append(parent)
        deadline = time.monotonic() + 10
        while not ready.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(ready.exists())
        child = int(child_pid_path.read_text())
        try:
            parent.kill()
            parent.wait(timeout=10)
            owner.close()
            os.kill(child, signal.SIGCONT)
            final = self.await_terminal(job_id, timeout=30)
            self.assertEqual(final["state"], "succeeded", final)
            self.assertEqual(len(self.ask_requests()), self.prior_requests + 1)
        finally:
            if running(child):
                os.kill(child, signal.SIGKILL)
        self.assertTrue((directory / "attempt-2-item-0.log").exists())

    def test_retry_preserves_historical_log_bytes_and_paths(self):
        job_id, directory = self.finished_failure()
        old = json.loads((directory / "job.json").read_text())
        log = Path(old["items"][0]["log_path"])
        old_bytes = log.read_bytes()
        self.state["fail_ask"] = False
        self.run_tny("jobs", "retry", job_id, "--failed", "--json")
        final = self.await_terminal(job_id)
        snapshot = json.loads((directory / "attempt-1.json").read_text())
        self.assertEqual(snapshot["items"][0]["log_path"], str(log))
        self.assertEqual(log.read_bytes(), old_bytes)
        self.assertNotEqual(final["items"][0]["log_path"], str(log))
        self.assertEqual(final["state"], "succeeded", final)

    def test_snapshot_publication_survives_crash_and_commit_failure(self):
        for crash in (True, False):
            with self.subTest(crash=crash):
                job_id, directory = self.finished_failure()
                before = (directory / "job.json").read_bytes()
                library = self.workspace / "snapshot-fault.so"
                subprocess.run(
                    [
                        "cc",
                        "-D_DEFAULT_SOURCE",
                        "-DTNY_FIXTURE_SNAPSHOT_COMMIT",
                        "-fPIC",
                        "-dynamiclib" if sys.platform == "darwin" else "-shared",
                        str(ROOT / "tests/fixtures/jobs_launch_barrier.c"),
                        "-o",
                        str(library),
                        "-ldl",
                    ],
                    check=True,
                    capture_output=True,
                )
                ready = self.workspace / f"snapshot-ready-{crash}"
                resume = self.workspace / f"snapshot-resume-{crash}"
                loader = (
                    "DYLD_INSERT_LIBRARIES"
                    if sys.platform == "darwin"
                    else "LD_PRELOAD"
                )
                env = dict(
                    self.env,
                    **{
                        loader: str(library),
                        "TNY_FIXTURE_PIPE_READY": str(ready),
                        "TNY_FIXTURE_PIPE_RESUME": str(resume),
                    },
                )
                retry = self.spawn_tny(
                    "jobs", "retry", job_id, "--failed", "--json", env=env
                )
                self.started.append(retry)
                deadline = time.monotonic() + 10
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(ready.exists(), "snapshot/state boundary not reached")
                snapshot = directory / "attempt-1.json"
                saved = snapshot.read_bytes()
                self.assertEqual((directory / "job.json").read_bytes(), before)
                if crash:
                    retry.kill()
                else:
                    resume.touch()
                retry.communicate(timeout=20)
                self.assertNotEqual(retry.returncode, 0)
                self.assertEqual((directory / "job.json").read_bytes(), before)
                self.state["fail_ask"] = False
                self.run_tny("jobs", "retry", job_id, "--failed", "--json")
                final = self.await_terminal(job_id)
                self.assertEqual(final["state"], "succeeded", final)
                self.assertEqual(final["attempt"], 2)
                self.assertEqual(snapshot.read_bytes(), saved)
                self.state["fail_ask"] = True

    def test_snapshot_failure_refuses_retry_without_mutation_or_spend(self):
        job_id, directory = self.finished_failure()
        before = (directory / "job.json").read_bytes()
        snapshot = directory / "attempt-1.json"
        snapshot.write_bytes(b"existing inconsistent immutable history\n")
        run = self.run_tny("jobs", "retry", job_id, "--failed", "--json", check=False)
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual((directory / "job.json").read_bytes(), before)
        self.assertEqual(
            snapshot.read_bytes(), b"existing inconsistent immutable history\n"
        )
        self.assertEqual(len(self.ask_requests()), self.prior_requests)

    def test_rm_and_retry_serialize_both_winner_orders(self):
        for winner, loser in (("rm", "retry"), ("retry", "rm")):
            with self.subTest(winner=winner):
                job_id, directory = self.finished_failure()
                barrier = self.lock_held(directory / "state.lock")
                self.addCleanup(barrier.close)
                options = ("--failed",) if winner == "retry" else ()
                first = self.spawn_tny("jobs", winner, job_id, *options, "--json")
                self.started.append(first)
                try:
                    self.wait_owner(directory, held=True)
                    before = (directory / "job.json").read_bytes()
                    options = ("--failed",) if loser == "retry" else ()
                    second = self.run_tny(
                        "jobs", loser, job_id, *options, "--json", check=False
                    )
                    self.assertNotEqual(second.returncode, 0)
                    self.assertEqual((directory / "job.json").read_bytes(), before)
                finally:
                    barrier.close()
                first.communicate(timeout=30)
                self.assertEqual(first.returncode, 0)
                if winner == "rm":
                    self.assertFalse(directory.exists())
                else:
                    self.assertEqual(self.await_terminal(job_id)["attempt"], 2)

    def test_private_no_replace_validates_destination_before_spend(self):
        target = self.workspace / "existing-private.png"
        original = png(2, 3)
        target.write_bytes(original)
        for args in ((), ("--output-file", str(target))):
            with self.subTest(args=args):
                run = self.run_tny(
                    "image",
                    "--job-no-replace",
                    "generate",
                    *args,
                    "--json",
                    stdin=b"no request without an absent destination",
                    check=False,
                )
                self.assertNotEqual(run.returncode, 0)
        self.assertFalse(self.image_requests())
        self.assertEqual(target.read_bytes(), original)
        self.assertFalse(list(self.workspace.glob("*.job-*")))

    def test_output_created_during_generation_is_not_replaced(self):
        self.state["hold"] = 2
        target = self.workspace / "late-output.png"
        _, payload = self.submit(
            "image",
            "--output-file",
            str(target),
            "--prompt",
            "HOLD late competing writer",
        )
        deadline = time.monotonic() + 10
        while not self.image_requests() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(self.image_requests())
        original = png(3, 4)
        target.write_bytes(original)
        final = self.await_terminal(payload["id"])
        self.assertEqual(final["state"], "failed", final)
        self.assertEqual(target.read_bytes(), original)
        self.assertFalse(list(self.workspace.glob("*.job-*")))

    def test_queued_image_rechecks_reserved_output_before_spending(self):
        self.state["hold"] = 2
        first = self.workspace / "first.png"
        second = self.workspace / "second.png"
        request = self.batch_request(
            "image",
            [
                {"prompt": "HOLD first image", "output_file": str(first)},
                {"prompt": "never spend second", "output_file": str(second)},
            ],
            concurrency=1,
        )
        _, payload = self.submit("batch", "--request", request)
        self.await_state(payload["id"], lambda r: r["items"][0]["state"] == "running")
        original = png(4, 3)
        second.write_bytes(original)
        final = self.await_terminal(payload["id"])
        self.assertEqual([i["state"] for i in final["items"]], ["succeeded", "failed"])
        self.assertEqual(len(self.image_requests()), 1)
        self.assertEqual(second.read_bytes(), original)

    def test_unobservable_child_exit_is_interrupted_with_unknown_cleanup(self):
        job_id, _directory, owner, payload = self.queued_owned()
        sentinel = subprocess.Popen(
            [sys.executable, "-c", "import sys; sys.stdin.buffer.read()"],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        self.addCleanup(sentinel.communicate, timeout=10)
        fixture = self.workspace / "auto-reaped-child"
        child_marker = self.workspace / "auto-reaped-child.pid"
        fixture.write_text(
            f"#!{sys.executable}\nimport os,sys\n"
            f"with open({str(child_marker)!r}, 'w') as marker: marker.write(str(os.getpid()))\n"
            "sys.stdin.read()\n"
        )
        fixture.chmod(0o700)
        payload["self"] = str(fixture)
        library = self.workspace / "auto-reap.so"
        subprocess.run(
            [
                "cc",
                "-D_DEFAULT_SOURCE",
                "-DTNY_FIXTURE_REAP_LOST",
                "-fPIC",
                "-dynamiclib" if sys.platform == "darwin" else "-shared",
                str(ROOT / "tests/fixtures/jobs_launch_barrier.c"),
                "-o",
                str(library),
            ],
            check=True,
            capture_output=True,
        )
        loader = "DYLD_INSERT_LIBRARIES" if sys.platform == "darwin" else "LD_PRELOAD"
        marker = self.workspace / "auto-reap-active.json"
        worker = self.private_worker(
            job_id,
            owner,
            env=dict(
                self.env,
                **{
                    loader: str(library),
                    "TNY_FIXTURE_AUTOREAP_TARGET": str(fixture),
                    "TNY_FIXTURE_AUTOREAP_MARKER": str(marker),
                },
            ),
        )
        worker.communicate(json.dumps(payload).encode(), timeout=20)
        owner.close()
        self.assertTrue(
            marker.exists(), "the post-initialization spawn fault was not active"
        )
        injected = json.loads(marker.read_text())
        self.assertEqual(injected["parent"], worker.pid)
        self.assertGreater(injected["child"], 1)
        self.assertEqual(injected["child"], int(child_marker.read_text()))
        self.assertTrue(injected["default_before"], injected)
        self.assertTrue(injected["active"], injected)
        final = self.status(job_id)
        self.assertEqual(final["state"], "interrupted", final)
        self.assertEqual(final["cleanup"], "unknown", final)
        self.assertTrue(final["cleanup_hold"], final)
        self.assertIsNone(final["items"][0]["exit_code"])
        self.assertFalse(
            running(injected["child"]), "the auto-reaped child still exists"
        )
        self.assertIsNone(sentinel.poll(), "the unrelated sentinel was stopped")

    def test_cancellation_terminates_descendants_in_other_process_groups(self):
        self.check_owned_tree_cleanup(auto_reap=False)

    @unittest.skipUnless(sys.platform == "linux", "Linux pidfd ancestry regression")
    def test_initial_ancestry_mismatch_never_signals_unrelated_generation(self):
        fixture = self.workspace / "ancestry-capture"
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-D_DEFAULT_SOURCE",
                "-DTNY_FIXTURE_ANCESTRY",
                "-ffunction-sections",
                "-fdata-sections",
                "-I",
                str(ROOT / "src"),
                str(ROOT / "tests/fixtures/jobs_launch_barrier.c"),
                str(ROOT / "src/util/process.c"),
                str(ROOT / "src/util/util.c"),
                str(ROOT / "src/util/tny_poll.c"),
                "-Wl,--gc-sections,--wrap=fopen,--wrap=syscall,--wrap=close",
                "-o",
                str(fixture),
            ],
            check=True,
            capture_output=True,
        )
        result = subprocess.run([str(fixture)], capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(b"signals=0 sentinel_alive=1", result.stdout)
        self.assertIn(b"opens=1 closes=1", result.stdout)
        self.assertIn(b"cleanup=-1 reaped=1 root_absent=1", result.stdout)

    def test_cancellation_terminates_descendants_of_auto_reaping_parent(self):
        self.check_owned_tree_cleanup(auto_reap=True)

    def check_owned_tree_cleanup(self, *, auto_reap):
        sentinel = self.start_sentinel()
        self.addCleanup(sentinel.kill)
        job_id, _directory, owner, payload = self.queued_owned()
        marker = self.workspace / "descendant-pid"
        fixture = self.workspace / "fake-owned-tree"
        fixture.write_text(
            f"#!{sys.executable}\nimport subprocess,sys,time\n"
            "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)'],start_new_session=True)\n"
            f"open({str(marker)!r},'w').write(str(p.pid))\n"
            "sys.stdin.read()\n"
            "time.sleep(60)\n"
        )
        fixture.chmod(0o700)
        payload["self"] = str(fixture)
        if auto_reap:
            library = self.workspace / "auto-parent.so"
            subprocess.run(
                [
                    "cc",
                    "-D_DEFAULT_SOURCE",
                    "-DTNY_FIXTURE_REAP_LOST",
                    "-fPIC",
                    "-dynamiclib" if sys.platform == "darwin" else "-shared",
                    str(ROOT / "tests/fixtures/jobs_launch_barrier.c"),
                    "-o",
                    str(library),
                ],
                check=True,
                capture_output=True,
            )
            loader = (
                "DYLD_INSERT_LIBRARIES" if sys.platform == "darwin" else "LD_PRELOAD"
            )
            payload["ask_env"] = {
                loader: str(library),
                "TNY_FIXTURE_AUTOREAP_AT_EXEC": "1",
                "TNY_FIXTURE_AUTOREAP_MARKER": str(self.workspace / "auto-reap-proof"),
            }
        worker = self.private_worker(job_id, owner)
        worker.stdin.write(json.dumps(payload).encode())
        worker.stdin.close()
        worker.stdin = None
        deadline = time.monotonic() + 10
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(marker.exists())
        descendant = int(marker.read_text())
        if auto_reap:
            self.assertEqual((self.workspace / "auto-reap-proof").read_text(), "active")
        try:
            self.run_tny("jobs", "cancel", job_id, "--json")
            worker.communicate(timeout=30)
            owner.close()
            final = self.status(job_id)
            self.assertEqual(final["state"], "cancelled", final)
            self.assertEqual(final["cleanup"], "complete", final)
            self.assertFalse(
                running(descendant), "owned descendant still exists (including zombie)"
            )
            self.assertIsNone(sentinel.poll(), "unrelated sentinel was terminated")
        finally:
            if running(descendant):
                os.kill(descendant, signal.SIGKILL)

    def test_image_child_drops_named_custom_and_explicit_chat_carriers(self):
        job_id, directory, owner, payload = self.queued_owned()
        marker = self.workspace / "credential-assertions.json"
        item = self.workspace / "fake-image-child"
        names = (
            "CURSOR_API_KEY",
            "NAMED_API_KEY",
            "NAMED_BASE_URL",
            "CUSTOM_SECRET",
            "EXPLICIT_SECRET",
        )
        item.write_text(
            f"#!{sys.executable}\nimport json,os,sys\n"
            f"names={names!r}\n"
            f"result={{name: name not in os.environ for name in names}}\n"
            "result['no_private_url_alias']='LANG' not in os.environ\n"
            f"result['own_token']=os.getenv('CHATGPT_ACCESS_TOKEN')=={TOKEN!r}\n"
            f"result['own_account']=os.getenv('CHATGPT_ACCOUNT_ID')=={ACCOUNT!r}\n"
            f"open({str(marker)!r},'w').write(json.dumps(result))\n"
            "sys.stdin.read()\n"
        )
        item.chmod(0o700)
        payload.update(kind="image", self=str(item))
        payload["items"][0]["output_file"] = os.path.realpath(
            self.workspace / "fake.png"
        )
        env = dict(self.env, **{name: "fixture-foreign-not-real" for name in names})
        # An explicitly chosen URL carrier may even have an operational name.
        # Record only its absence, never the URL value.
        env["LANG"] = payload["chat"]["base_url"]
        worker = self.private_worker(job_id, owner, env=env)
        worker.communicate(json.dumps(payload).encode(), timeout=20)
        owner.close()
        # Only presence/equality booleans are recorded, never environment values.
        self.assertTrue(marker.exists())
        self.assertTrue(all(json.loads(marker.read_text()).values()))
        self.assertEqual(len(self.ask_requests()), self.prior_requests)
        self.assertFalse((directory / "attempt-1.json").exists())


class JobsUnsupportedRuntime(unittest.TestCase):
    """The documented wasm answer: no process ownership, no silent no-op."""

    def setUp(self):
        if not WASM:
            self.skipTest("native build: the wasm rejection cases run in the wasm job")
        self.tmp = tempfile.TemporaryDirectory(prefix="tny-jobs-wasm-")
        self.home = Path(self.tmp.name)
        self.env = dict(os.environ, HOME=str(self.home))

    def tearDown(self):
        self.tmp.cleanup()

    def run_tny(self, *args, stdin=b""):
        return subprocess.run(
            [TNY, *args],
            cwd=self.home,
            env=self.env,
            input=stdin,
            capture_output=True,
            timeout=120,
        )

    def test_execution_operations_refuse_before_any_side_effect(self):
        run = self.run_tny("jobs", "submit", "ask", "--prompt", "nope", "--json")
        self.assertEqual(run.returncode, 1, run.stdout)
        self.assertIn(b"native", run.stderr.lower())
        self.assertFalse(
            (self.home / ".tny" / "jobs").exists()
            and any((self.home / ".tny" / "jobs").iterdir())
        )
        run = self.run_tny("jobs", "retry", "0" * 32, "--failed", "--json")
        self.assertEqual(run.returncode, 1, run.stdout)

    def test_reading_existing_records_still_works(self):
        run = self.run_tny("jobs", "list", "--json")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(json.loads(run.stdout.decode())["jobs"], [])


def argv_without_runner_binary():
    """run.sh appends $TNY; unittest must not read it as a test name."""
    kept = [sys.argv[0]]
    for argument in sys.argv[1:]:
        if (
            not argument.startswith("-")
            and os.path.isfile(argument)
            and os.access(argument, os.X_OK)
        ):
            continue
        kept.append(argument)
    return kept


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary())
