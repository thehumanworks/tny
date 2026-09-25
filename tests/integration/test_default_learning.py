#!/usr/bin/env python3
"""Default native learning against a deterministic local provider, no live calls.

The fixture policy retries blindly until empirical guidance is actually present
in the outgoing request. Exact filesystem results, not assistant prose, score it.
"""

from __future__ import annotations

import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from code_mode_fixture import code_chat_frames, code_response_events

ROOT = Path(__file__).resolve().parents[2]
TNY = Path(os.environ.get("TNY", ROOT / "build/tny")).resolve()
HEADER = "# Automatic workflow learning"


class Provider(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        fixture = self.server.fixture
        fixture.requests.append(body)
        chat = "messages" in body
        items = body.get("messages", body.get("input", []))
        step = sum(
            x.get("role") == "tool" or x.get("type") == "function_call_output"
            for x in items
        )
        context = body.get("instructions", items[0].get("content", "") if items else "")
        if step == 0:
            fixture.guided = HEADER in context
            fixture.guidance = (
                (HEADER + context.split(HEADER, 1)[1]).split(
                    "Conversation image input:", 1
                )[0]
                if fixture.guided
                else ""
            )
            fixture.plan = (
                ["bad", "read", "good"]
                if fixture.guided
                else ["bad", "bad", "read", "good"]
            )
            if fixture.variant == "wrong-target":
                fixture.plan = ["bad", "other-read", "good"]
            elif fixture.variant == "wrong-intent":
                fixture.plan = ["bad", "read", "other-edit"]
            elif fixture.variant == "failed-retry":
                fixture.plan = ["bad", "read", "bad"]
            elif fixture.variant == "unrelated-terminal":
                fixture.plan = ["bad", "echo", "good"]
            elif fixture.variant == "tilde-read":
                fixture.plan = ["bad", "tilde-read", "good"]
            elif fixture.variant in ("unknown-tool", "invalid-arguments"):
                fixture.plan = ["bad", "read", fixture.variant, "good"]
        call = fixture.tool(fixture.plan[step]) if step < len(fixture.plan) else None
        if call:
            fixture.calls.append(call)
        if chat:
            delta = {"content": "fixture complete"}
            if call:
                delta = {
                    "tool_calls": [
                        {
                            "index": 0,
                            "id": f"c{step}",
                            "type": "function",
                            "function": {
                                "name": call["name"],
                                "arguments": json.dumps(call["arguments"]),
                            },
                        }
                    ]
                }
            frames = [
                {"choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {},
                            "finish_reason": "tool_calls" if call else "stop",
                        }
                    ]
                },
            ]
            data = (
                "".join(
                    "data: " + json.dumps(x) + "\n\n" for x in code_chat_frames(frames)
                )
                + "data: [DONE]\n\n"
            )
        else:
            if call:
                item = {
                    "type": "function_call",
                    "id": f"fc{step}",
                    "call_id": f"c{step}",
                    "name": call["name"],
                    "arguments": json.dumps(call["arguments"]),
                }
                frames = [
                    {
                        "type": "response.output_item.added",
                        "output_index": 0,
                        "item": dict(item, arguments=""),
                    },
                    {
                        "type": "response.function_call_arguments.delta",
                        "item_id": item["id"],
                        "output_index": 0,
                        "delta": item["arguments"],
                    },
                    {
                        "type": "response.output_item.done",
                        "output_index": 0,
                        "item": item,
                    },
                ]
            else:
                item = {
                    "type": "message",
                    "id": "m",
                    "role": "assistant",
                    "status": "completed",
                    "content": [
                        {
                            "type": "output_text",
                            "text": "fixture complete",
                            "annotations": [],
                        }
                    ],
                }
                frames = [
                    {
                        "type": "response.output_text.delta",
                        "item_id": "m",
                        "output_index": 0,
                        "content_index": 0,
                        "delta": "fixture complete",
                    }
                ]
            frames.append(
                {
                    "type": "response.completed",
                    "response": {"id": "r", "status": "completed", "output": [item]},
                }
            )
            data = "".join(
                "data: " + json.dumps(x) + "\n\n" for x in code_response_events(frames)
            )
        encoded = data.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)


class Fixture:
    def __init__(self, root: Path, binary=TNY, profile="all", wire="chat"):
        self.root, self.binary, self.profile, self.wire = (
            root,
            Path(binary),
            profile,
            wire,
        )
        self.workspace = root / "workspace"
        self.workspace.mkdir(parents=True, exist_ok=True)
        self.home = root / "home"
        self.home.mkdir(exist_ok=True)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        self.server.fixture = self
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.env = dict(
            os.environ,
            HOME=str(self.home),
            CODEX_HOME=str(self.home / "codex"),
            OPENAI_API_KEY="synthetic-fixture",
            OPENAI_BASE_URL=f"http://127.0.0.1:{self.server.server_port}/v1",
            OPENAI_WIRE_API=wire,
            TNY_TOOLS=profile,
            TNY_ISOLATE="1",
            TNY_EXTENSIONS="0",
            TNY_WORKFLOW_TASK_DIR="",
        )
        self.env.pop("TNY_SELF_IMPROVE", None)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)

    def tool(self, action):
        if action == "unknown-tool":
            return {"name": "unknown_fixture_tool", "arguments": {}}
        if action == "invalid-arguments":
            return {"name": "read_file", "arguments": {"path": 17}}
        if action == "tilde-read":
            return {
                "name": "terminal",
                "arguments": {"command": "cat " + shlex.quote("~/" + self.target.name)},
            }
        if action in ("read", "other-read", "echo"):
            path = self.other if action == "other-read" else self.target
            if action == "echo":
                return {
                    "name": "terminal",
                    "arguments": {"command": "printf unrelated"},
                }
            if self.profile == "all":
                return {"name": "read_file", "arguments": {"path": str(path)}}
            return {
                "name": "terminal",
                "arguments": {"command": "cat " + shlex.quote(str(path))},
            }
        old = "MISSING search text\n" if action == "bad" else self.before
        new = "unrelated intent\n" if action == "other-edit" else self.after
        if self.profile == "all":
            return {
                "name": "edit_file",
                "arguments": {
                    "path": str(self.target),
                    "old_string": old,
                    "new_string": new,
                },
            }
        payload = f"*** SEARCH\n{old}*** REPLACE\n{new}*** END\n"
        command = (
            "printf "
            + shlex.quote(payload)
            + " | tny edit "
            + shlex.quote(str(self.target))
        )
        return {"name": "terminal", "arguments": {"command": command}}

    def run(self, case, *, disable=False, ephemeral=False, variant="normal"):
        self.variant = variant
        self.target, self.other = (
            self.workspace / f"case-{case}.txt",
            self.workspace / "other.txt",
        )
        if variant == "tilde-read":
            self.target = self.home / f"case-{case}.txt"
            (self.workspace / "~").mkdir(exist_ok=True)
            self.other = self.workspace / "~" / self.target.name
        # A real successful read can start with 'error:'; display text is not a label.
        self.before, self.after = f"error: original {case}\n", f"corrected {case}\n"
        self.target.write_text(self.before)
        self.other.write_text("unrelated content\n")
        self.requests, self.calls, self.guided, self.plan = [], [], False, []
        args = [
            str(self.binary),
            "--cwd",
            str(self.workspace),
            "--provider",
            "openai",
            "--model",
            "fixture",
            "--max-steps",
            "8",
        ]
        if disable:
            args.append("--no-self-improve")
        if ephemeral:
            args.append("--ephemeral")
        result = subprocess.run(
            args + ["ask", "--json", "Perform the fixture edit and verify the result."],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=30,
        )
        if result.returncode:
            raise AssertionError((result.returncode, result.stdout, result.stderr))
        actual = self.target.read_text()
        items = self.requests[-1].get("messages", self.requests[-1].get("input", []))
        outputs = [
            x.get("content", x.get("output", ""))
            for x in items
            if x.get("role") == "tool" or x.get("type") == "function_call_output"
        ]
        verified = []
        for action, output in zip(self.plan, outputs):
            if action == "bad":
                correct = output.startswith("error: old_string") or output.startswith(
                    "exit: 2\n"
                )
            elif action == "read":
                correct = self.before in output
            elif action == "good":
                correct = output.startswith("replaced 1 occurrence in ") or (
                    output.startswith("exit: 0\n") and "edited " in output
                )
            else:
                correct = False
            verified.append(
                {
                    "operation": action,
                    "verified_native_outcome": correct,
                    "result_first_line": output.splitlines()[0].replace(
                        str(self.root), "<fixture>"
                    )
                    if output
                    else "",
                }
            )
        outcomes_verified = len(outputs) == len(self.plan) and all(
            row["verified_native_outcome"] for row in verified
        )
        if variant == "normal" and not outcomes_verified:
            raise AssertionError((self.plan, outputs))
        failed_edits = sum(
            action == "bad"
            and (
                output.startswith("error: old_string") or output.startswith("exit: 2\n")
            )
            for action, output in zip(self.plan, outputs)
        )
        return {
            "failed_edits": failed_edits,
            "outcomes_verified": outcomes_verified,
            "outcomes": verified,
            "guidance": self.guidance,
            "case": str(case),
            "guided_at_start": self.guided,
            "tool_calls": len(self.calls),
            "provider_requests": len(self.requests),
            "passed": actual == self.after,
            "actual_sha256": hashlib.sha256(actual.encode()).hexdigest(),
            "expected_sha256": hashlib.sha256(self.after.encode()).hexdigest(),
            "task_preset": json.loads(result.stdout).get("task"),
            "variant": variant,
        }

    def state(self):
        return {
            p.name: json.loads(p.read_text())
            for p in (self.home / ".tny/learning").glob("*.json")
        }


@unittest.skipIf("/wasm/" in str(TNY), "native runner workflow")
class DefaultLearningTests(unittest.TestCase):
    def fixture(self, profile="all", wire="chat", binary=TNY):
        temp = tempfile.TemporaryDirectory(prefix="tny-default-learning-")
        self.addCleanup(temp.cleanup)
        fixture = Fixture(Path(temp.name), binary, profile, wire)
        self.addCleanup(fixture.close)
        return fixture

    def test_default_learns_without_a_task_in_both_wires_and_all_profiles(self):
        for wire, profile in (
            ("chat", "all"),
            ("responses", "all"),
            ("chat", "terminal"),
            ("chat", "terminal+edit"),
        ):
            with self.subTest(wire=wire, profile=profile):
                f = self.fixture(profile, wire)
                first, second = f.run(1), f.run(2)
                self.assertFalse(first["guided_at_start"])
                self.assertFalse(second["guided_at_start"])
                learned = f.run(3)
                self.assertTrue(learned["guided_at_start"])
                self.assertEqual(
                    [r["tool_calls"] for r in (first, second, learned)], [4, 4, 3]
                )
                self.assertTrue(all(r["passed"] for r in (first, second, learned)))
                self.assertTrue(
                    all(r["task_preset"] is None for r in (first, second, learned))
                )
                state = f.state()
                self.assertEqual(len(state), 1)
                encoded = json.dumps(state)
                for secret in (
                    "original",
                    "corrected",
                    "case-",
                    "MISSING",
                    str(f.workspace),
                ):
                    self.assertNotIn(secret, encoded)
                self.assertEqual(
                    next(iter(state.values()))["rules"][0 if profile == "all" else 2][
                        "successes"
                    ],
                    3,
                )

    def test_tui_uses_the_same_default_learning_without_a_preset(self):
        from test_tui import BANNER, Term

        f = self.fixture()
        f.run(1)
        f.run(2)
        f.target.write_text(f.before)
        f.requests, f.calls, f.guided, f.plan = [], [], False, []
        term = Term(
            [
                str(TNY),
                "--cwd",
                str(f.workspace),
                "--provider",
                "openai",
                "--model",
                "fixture",
            ],
            f.env,
            str(f.workspace),
        )
        self.addCleanup(term.close)
        term.expect(BANNER)
        term.send("Perform the fixture edit.\r")
        term.expect("fixture complete", timeout=20)
        self.assertTrue(f.guided)
        self.assertEqual(len(f.calls), 3)
        self.assertEqual(f.target.read_text(), f.after)
        term.send("\x04")
        self.assertEqual(term.wait(), 0)

    def test_optout_suppresses_learning_and_does_not_modify_existing_state(self):
        f = self.fixture()
        f.run(1)
        f.run(2)
        before = f.state()
        result = f.run(3, disable=True)
        self.assertFalse(result["guided_at_start"])
        self.assertEqual(result["tool_calls"], 4)
        self.assertEqual(f.state(), before)
        fresh = self.fixture()
        fresh.run(1, disable=True)
        self.assertFalse((fresh.home / ".tny/learning").exists())

    def test_ephemeral_does_not_load_or_write_workspace_learning(self):
        f = self.fixture()
        f.run(1)
        f.run(2)
        before = f.state()
        result = f.run(3, ephemeral=True)
        self.assertFalse(result["guided_at_start"])
        self.assertEqual(f.state(), before)

    def test_unrelated_target_intent_or_terminal_output_is_not_evidence(self):
        for profile, variant in (
            ("all", "wrong-target"),
            ("all", "wrong-intent"),
            ("terminal", "unrelated-terminal"),
            ("terminal", "tilde-read"),
            ("all", "unknown-tool"),
            ("all", "invalid-arguments"),
        ):
            with self.subTest(profile=profile, variant=variant):
                f = self.fixture(profile)
                f.run(1, variant=variant)
                f.run(2, variant=variant)
                self.assertFalse(f.run(3)["guided_at_start"])

    def test_negative_trial_demotes_previously_eligible_advice(self):
        f = self.fixture()
        f.run(1)
        f.run(2)
        failed = f.run(3, variant="failed-retry")
        self.assertTrue(failed["guided_at_start"])
        self.assertFalse(failed["passed"])
        self.assertFalse(f.run(4)["guided_at_start"])

    def test_setting_environment_cli_precedence_and_status(self):
        f = self.fixture()
        settings = f.home / ".tny/settings.json"
        settings.parent.mkdir(exist_ok=True)
        for config, value, flag, enabled in (
            ({}, None, False, True),
            ({"self_improve": False}, None, False, False),
            ({"self_improve": False}, "1", False, True),
            ({}, "1", True, False),
        ):
            settings.write_text(json.dumps(config))
            env = dict(f.env)
            if value is not None:
                env["TNY_SELF_IMPROVE"] = value
            result = subprocess.run(
                [
                    str(TNY),
                    "--cwd",
                    str(f.workspace),
                    "--provider",
                    "openai",
                    *(["--no-self-improve"] if flag else []),
                    "doctor",
                    "--json",
                ],
                env=env,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIs(json.loads(result.stdout)["self_improve"], enabled)
        settings.write_text('{"self_improve":"yes"}')
        bad = subprocess.run(
            [str(TNY), "doctor", "--json"], env=f.env, capture_output=True, timeout=10
        )
        self.assertNotEqual(bad.returncode, 0)
        self.assertFalse((f.home / ".tny/learning").exists())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
