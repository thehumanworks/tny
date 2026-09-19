#!/usr/bin/env python3
"""Offline checks for the explicit native-session proposal adapter."""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ADAPTER = ROOT / "python/tny_improve_propose.py"

STUB = """#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
with open(os.environ["CALLS"], "a") as out:
    out.write(json.dumps(args) + "\\n")
mode = os.environ.get("STUB_MODE", "done")
if "ask" in args:
    Path(os.environ["PROMPT"]).write_text(sys.stdin.read())
    print("unreadable" if mode == "launch-bad" else json.dumps({"session_id": "0123456789abcdef"}))
    if mode == "launch-error":
        sys.exit(7)
elif "sessions" in args:
    print(json.dumps({"sessions": [{"id": "0123456789abcdef"}]}))
elif "stop" in args:
    Path(os.environ["CALLS"] + ".stopped").touch()
    sys.exit(0)
elif Path(os.environ["CALLS"] + ".stopped").exists():
    print(json.dumps({"status": "interrupted", "exit_code": 130}))
    sys.exit(130)
elif mode == "timeout":
    sys.exit(124)
else:
    print(json.dumps({"status": "error" if mode == "error" else "done",
        "exit_code": 2 if mode == "error" else 0,
        "result": {"output": "not JSON" if mode == "bad" else json.dumps({
            "instructions": "Keep exact evidence.", "rationale": "Training feedback."})}}))
"""


class WorkspaceCleanup:
    def cleanup_workspaces(self, stderr):
        for line in stderr.splitlines():
            if line.startswith('{"workspace":') or line.startswith('{"session_id":'):
                workspace = json.loads(line).get("workspace")
                if workspace:
                    self.addCleanup(shutil.rmtree, workspace, ignore_errors=True)


class ProposalTests(WorkspaceCleanup, unittest.TestCase):
    def invoke(self, mode="done", budget=None):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        stub = root / "tny fixture"
        stub.write_text(STUB)
        stub.chmod(0o755)
        calls = root / "calls.jsonl"
        prompt = root / "prompt.txt"
        result = subprocess.run(
            [
                sys.executable,
                str(ADAPTER),
                "--tny",
                str(stub),
                "--provider",
                "synthetic",
                "--model",
                "fixture",
                "--timeout",
                "1",
            ],
            input=json.dumps({"instructions": "Parent", "feedback": ["failure"]}),
            capture_output=True,
            text=True,
            env=dict(
                os.environ,
                CALLS=str(calls),
                PROMPT=str(prompt),
                STUB_MODE=mode,
                **(
                    {"TNY_IMPROVE_TIMEOUT_S": str(budget)} if budget is not None else {}
                ),
            ),
            timeout=10,
        )
        self.cleanup_workspaces(result.stderr)
        recorded = calls.read_text().splitlines() if calls.exists() else []
        return result, [json.loads(x) for x in recorded], prompt

    def test_fresh_bounded_session_and_checked_result(self):
        result, calls, prompt = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            json.loads(result.stdout)["instructions"], "Keep exact evidence."
        )
        launch, wait = calls
        for flag in ("-B", "--no-extensions", "--max-steps", "--output-schema"):
            self.assertIn(flag, launch)
        self.assertEqual(launch[launch.index("--permission-mode") + 1], "ask")
        self.assertIn("--wait", wait)
        self.assertEqual(launch[1], wait[1])
        self.assertIn("Parent", prompt.read_text())
        self.assertIn("0123456789abcdef", result.stderr)

    def test_insufficient_outer_budget_refuses_launch(self):
        for budget in (1, 30, "nan", "infinity", "bad"):
            with self.subTest(budget=budget):
                result, calls, _ = self.invoke(budget=budget)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(calls, [])
        result, calls, _ = self.invoke(budget=100)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(calls), 2)

    def test_wait_timeout_is_not_success_and_requests_cancellation(self):
        result, calls, _ = self.invoke("timeout")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("stop", calls[-2])
        self.assertIn("cancellation settled: True", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_failed_session_cannot_propose(self):
        result, calls, _ = self.invoke("error")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("stop", calls[-2])
        self.assertIn("cancellation settled: True", result.stderr)

    def test_lost_launch_result_recovers_session_before_cancellation(self):
        for mode in ("launch-error", "launch-bad"):
            with self.subTest(mode=mode):
                result, calls, _ = self.invoke(mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("sessions", calls[1])
                self.assertIn("stop", calls[2])
                self.assertIn("recovered_session_id", result.stderr)
                self.assertIn("cancellation settled: True", result.stderr)

    def test_malformed_final_output_cannot_propose(self):
        result, calls, _ = self.invoke("bad")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(calls), 2)  # already settled; no running work to stop

    def test_provider_and_model_are_explicit(self):
        result = subprocess.run(
            [sys.executable, str(ADAPTER)], capture_output=True, text=True, timeout=5
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--provider", result.stderr)


class NativeProposalTests(WorkspaceCleanup, unittest.TestCase):
    def test_controller_interrupt_cancels_real_detached_session(self):
        for requested_signal in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=requested_signal):
                self.check_controller_interrupt(requested_signal)

    def check_controller_interrupt(self, requested_signal):
        binary = Path(os.environ.get("TNY", ROOT / "build/tny")).resolve()
        if not binary.is_file() or "/wasm/" in str(binary):
            self.skipTest("native tny build required")
        received, release = threading.Event(), threading.Event()

        class Provider(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_POST(self):
                self.rfile.read(int(self.headers["Content-Length"]))
                received.set()
                release.wait(20)
                self.close_connection = True

        server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as home:
                root = Path(home)
                (root / "baseline.md").write_text("Parent")
                spec = {
                    "version": 1,
                    "baseline": "baseline.md",
                    "rounds": 1,
                    "timeout_s": 180,
                    "cost_unit": "fixture",
                    "proposer": [
                        sys.executable,
                        str(ADAPTER),
                        "--tny",
                        str(binary),
                        "--provider",
                        "openai",
                        "--model",
                        "fixture",
                        "--timeout",
                        "60",
                    ],
                    "evaluator": [
                        sys.executable,
                        "-c",
                        'print(\'{"passed":true,"cost":1,"feedback":"fixture"}\')',
                    ],
                    "cases": {
                        s: [{"id": s, "input": s}]
                        for s in ("train", "validation", "test")
                    },
                }
                spec_path = root / "spec.json"
                spec_path.write_text(json.dumps(spec))
                env = dict(
                    os.environ,
                    HOME=home,
                    CODEX_HOME=home,
                    OPENAI_API_KEY="synthetic",
                    OPENAI_BASE_URL=f"http://127.0.0.1:{server.server_port}/v1",
                    OPENAI_WIRE_API="chat",
                    TNY_ISOLATE="1",
                    TNY_WORKFLOW_TASK_DIR="",
                )
                child = subprocess.Popen(
                    [
                        sys.executable,
                        str(ROOT / "python/tny_improve.py"),
                        "run",
                        "--spec",
                        str(spec_path),
                        "--out",
                        str(root / "run"),
                    ],
                    env=env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    start_new_session=True,
                )
                try:
                    self.assertTrue(
                        received.wait(15), "native runner did not reach mock provider"
                    )
                    child.send_signal(requested_signal)
                    out, err = child.communicate(timeout=18)
                    self.assertNotEqual(child.returncode, 0, (out, err))
                    self.assertEqual(json.loads(out)["status"], "failed")
                    receipt = (root / "run/round-01.proposal.stderr").read_text()
                    self.cleanup_workspaces(receipt)
                    self.assertIn("cancellation settled: True", receipt)
                    owner = {}
                    for line in receipt.splitlines():
                        if line.startswith(
                            (
                                '{"session_id":',
                                '{"workspace":',
                                '{"recovered_session_id":',
                            )
                        ):
                            owner.update(json.loads(line))
                    owner.setdefault("session_id", owner.get("recovered_session_id"))
                    checked = subprocess.run(
                        [
                            str(binary),
                            "--cwd",
                            owner["workspace"],
                            "session",
                            owner["session_id"],
                            "--wait",
                            "--timeout",
                            "2",
                            "--json",
                        ],
                        env=env,
                        capture_output=True,
                        text=True,
                        timeout=5,
                    )
                    self.assertIn(
                        json.loads(checked.stdout)["status"], ("interrupted", "error")
                    )
                    self.assertIn(checked.returncode, (2, 130))
                finally:
                    if child.returncode is None:
                        child.send_signal(signal.SIGINT)
                        child.communicate(timeout=25)
                    child.stdout.close()
                    child.stderr.close()
        finally:
            release.set()
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_real_detached_runner_with_local_provider(self):
        binary = Path(os.environ.get("TNY", ROOT / "build/tny")).resolve()
        if not binary.is_file() or "/wasm/" in str(binary):
            self.skipTest("native tny build required")
        requests = []

        class Provider(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_POST(self):
                requests.append(
                    json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                )
                proposal = json.dumps(
                    {"instructions": "Verify exact output.", "rationale": "Fixture."}
                )
                chunks = [
                    {
                        "choices": [
                            {
                                "index": 0,
                                "delta": {"role": "assistant", "content": proposal},
                            }
                        ]
                    },
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]},
                ]
                body = (
                    "".join("data: " + json.dumps(x) + "\n\n" for x in chunks)
                    + "data: [DONE]\n\n"
                )
                encoded = body.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

        server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as home:
                env = dict(
                    os.environ,
                    HOME=home,
                    CODEX_HOME=home,
                    OPENAI_API_KEY="synthetic",
                    OPENAI_BASE_URL=f"http://127.0.0.1:{server.server_port}/v1",
                    OPENAI_WIRE_API="chat",
                    TNY_ISOLATE="1",
                    TNY_WORKFLOW_TASK_DIR="",
                )
                result = subprocess.run(
                    [
                        sys.executable,
                        str(ADAPTER),
                        "--tny",
                        str(binary),
                        "--provider",
                        "openai",
                        "--model",
                        "fixture",
                        "--timeout",
                        "15",
                    ],
                    input=json.dumps({"instructions": "Parent", "feedback": []}),
                    text=True,
                    capture_output=True,
                    env=env,
                    timeout=40,
                )
                self.cleanup_workspaces(result.stderr)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(
                    json.loads(result.stdout)["instructions"], "Verify exact output."
                )
                self.assertEqual(len(requests), 1)
                self.assertEqual(requests[0]["model"], "fixture")
                self.assertIn("Parent", json.dumps(requests[0]))
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
