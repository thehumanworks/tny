#!/usr/bin/env python3
"""Captured parent via public ask -B; localhost provider, no fabricated records.

TNY=/absolute/frozen/tny python3 tests/integration/test_swarm_parent.py
"""

import base64
import hashlib
import json
import os
import shlex
import subprocess
import threading
import time
import unittest
from pathlib import Path

from code_mode_fixture import code_call
from test_jobs import TNY, Handler, JobsFixture, argv_without_runner_binary
from test_subagent import chat_frames, tool_outputs, user_texts


def barrier(started, release, finished):
    code = (
        "from pathlib import Path; import time; "
        f"Path({str(started)!r}).open('a').write('started\\n'); "
        f"r=Path({str(release)!r}); end=time.monotonic()+40\n"
        "while not r.exists() and time.monotonic()<end: time.sleep(.01)\n"
        "assert r.exists(), 'barrier timeout'\n"
        f"Path({str(finished)!r}).open('a').write('finished\\n')"
    )
    return "python3 -c " + shlex.quote(code)


def payload(output):
    # Terminal adapters prefix service JSON with exit/cwd metadata.
    assert "{" in output, output
    return json.JSONDecoder().raw_decode(output[output.index("{") :])[0]


class ParentHandler(Handler):
    def do_POST(self):
        f = self.server.fixture
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        texts = user_texts(body, "chat")
        tag = next(t for t in texts if t.startswith("PARENT-FLOW ")).split()[1]
        with f.lock:
            f.bodies.setdefault(tag, []).append(body)
        self.enter(tag)
        try:
            call, answer = f.response(tag, texts, tool_outputs(body, "chat"))
            self.reply(
                200,
                "text/event-stream",
                chat_frames(call=call) if call else chat_frames(text=answer),
            )
        except Exception as error:
            f.errors.append(repr(error))
            self.reply(500, "application/json", b'{"error":{"message":"fixture"}}')
        finally:
            self.leave()


class CapturedParent(JobsFixture):
    def setUp(self):
        super().setUp()
        self.server.RequestHandlerClass = ParentHandler
        self.server.fixture = self
        self.lock = threading.Lock()
        self.bodies, self.errors = {}, []
        self.run_id = None
        self.submitted = threading.Event()
        self.phase = "start"
        self.seen, self.collected, self.inspected = [], [], []
        self.deadline = time.monotonic() + 100
        self.check_command = (
            'pwd && test "$(cat one.txt)" = one && test ! -e two.txt '
            "&& printf 'PARENT-CHECK-PASS\\n'"
        )
        for key in list(self.env):
            if key.startswith(("TNY_", "GIT_")):
                self.env.pop(key)
        self.env["PATH"] = str(Path(TNY).parent) + os.pathsep + self.env["PATH"]
        self.env["GIT_CONFIG_NOSYSTEM"] = "1"
        self.git("init", "-q")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        (self.workspace / "base.txt").write_text("base\n")
        self.git("add", ".")
        self.git("commit", "-qm", "base")

    def git(self, *args):
        return subprocess.run(
            ["git", "-C", str(self.workspace), *args],
            env=self.env,
            capture_output=True,
            text=True,
            check=True,
            timeout=15,
        ).stdout.strip()

    def marker(self, name):
        return self.home / name

    def terminal(self, name, command):
        return (name, "terminal", json.dumps({"command": command})), None

    def team(self, action, request):
        if self.profile == "all":
            return (
                action,
                "team_control",
                json.dumps({"action": action, "request": request}),
            ), None
        path = self.marker("request-" + action + ".json")
        path.write_text(json.dumps(request))
        return self.terminal(
            action, "tny team " + action + " --request " + shlex.quote(str(path))
        )

    def mail(self, name, target, text):
        if self.profile == "all":
            return (
                name,
                "team_mailbox",
                json.dumps(
                    {
                        "action": "send",
                        "run": self.run_id,
                        "to": -1 if target == "lead" else target,
                        "id": name,
                        "text": text,
                    }
                ),
            ), None
        return self.terminal(
            name,
            f"tny mailbox send --run {self.run_id} --to {target} --id {name} --text "
            + shlex.quote(text),
        )

    def workspace_call(self, action, task):
        if self.profile == "all":
            return (
                action,
                "job_workspace_" + action,
                json.dumps({"run": self.run_id, "task": task, "attempt": 1}),
            ), None
        return self.terminal(
            action,
            f"tny task-workspace {action} --run {self.run_id} --task {task} --attempt 1 --json",
        )

    def response(self, tag, texts, outputs):
        assert time.monotonic() < self.deadline, self.phase
        step = len(outputs)
        if tag in ("one", "two"):
            assert self.submitted.wait(10), "parent did not get an immediate job handle"
        if tag == "one":
            if step == 0:
                return self.terminal(
                    "busy-one",
                    barrier(
                        self.marker("one-started"),
                        self.marker("release"),
                        self.marker("one-finished"),
                    ),
                )
            assert sum("clarification=approved" in t for t in texts) == 1, texts
            if step == 1:
                return self.terminal(
                    "edit-one",
                    "printf 'one\\n' > one.txt && git add one.txt && git commit -qm one",
                )
            return None, "ONE-DONE; prose is not verification"
        if tag == "two":
            if step == 0:
                return self.terminal(
                    "busy-two",
                    barrier(
                        self.marker("two-started"),
                        self.marker("one-started"),
                        self.marker("two-ready"),
                    ),
                )
            if step == 1:
                return self.mail("question", "lead", "clarification=requested")
            if step == 2:
                assert "question" in outputs[-1] and "error:" not in outputs[-1], (
                    outputs[-1]
                )
                return self.terminal(
                    "hold-two",
                    barrier(
                        self.marker("question-sent"),
                        self.marker("release"),
                        self.marker("two-finished"),
                    ),
                )
            if step == 3:
                return self.terminal(
                    "edit-two",
                    "printf 'two\\n' > two.txt && git add two.txt && git commit -qm two",
                )
            return None, "TWO-DONE"
        assert tag == "parent", tag
        if self.phase == "start":
            self.phase = "launched"
            return self.team(
                "start",
                {
                    "kind": "ask",
                    "dag": True,
                    "concurrency": 2,
                    "items": [
                        {
                            "role": "worker",
                            "prompt": "PARENT-FLOW " + n,
                            "workspace": {"policy": "isolated"},
                        }
                        for n in ("one", "two")
                    ],
                },
            )
        if self.phase == "launched":
            result = payload(outputs[-1])
            self.run_id = result["run_id"]
            self.submitted.set()
            assert result["verification"] == "unverified", result
            self.phase = "clarify"
            return self.terminal(
                "parent-live",
                barrier(
                    self.marker("parent-active"),
                    self.marker("question-sent"),
                    self.marker("parent-ready"),
                ),
            )
        if self.phase == "clarify":
            assert (
                self.marker("one-started").exists()
                and self.marker("two-started").exists()
            )
            assert not self.marker("one-finished").exists(), "tool interrupted"
            assert sum("clarification=requested" in t for t in texts) == 1, texts
            status = self.status(self.run_id)
            assert [i["state"] for i in status["items"]] == ["running"] * 2, status
            self.phase = "release"
            return self.mail("answer", 0, "clarification=approved")
        if self.phase == "release":
            assert "answer" in outputs[-1] and "error:" not in outputs[-1], outputs[-1]
            self.phase = "wait"
            return self.terminal(
                "release", "touch " + shlex.quote(str(self.marker("release")))
            )
        if self.phase == "waiting":
            result = payload(outputs[-1])
            if result["kind"] == "team_completion":
                self.seen.append(result["cursor"])
                self.phase = "collecting"
                return self.team(
                    "collect",
                    {
                        "id": self.run_id,
                        "item": result["cursor"]["item"],
                        "expected_attempt": 1,
                        "max_bytes": 4096,
                    },
                )
            assert result["kind"] == "team_wait_timeout", result
            self.phase = "wait"
        if self.phase == "collecting":
            assert "error:" not in outputs[-1], outputs[-1]
            self.collected.append(payload(outputs[-1]))
            self.phase = "wait" if len(self.collected) < 2 else "status"
        if self.phase == "wait":
            self.phase = "waiting"
            return self.team(
                "wait-any",
                {
                    "id": self.run_id,
                    "expected_attempt": 1,
                    "timeout_ms": 1000,
                    "seen": self.seen,
                },
            )
        if self.phase == "status":
            self.phase = "finished"
            return self.team("status", {"id": self.run_id})
        if self.phase == "finished":
            # Item completion can precede supervisor finalization. The parent
            # observes the whole job through its own public service call.
            status = payload(outputs[-1])["job"]
            if status["state"] != "succeeded":
                assert status["state"] in ("queued", "running"), status
                self.phase = "status"
                return self.terminal("settle", "sleep 0.05")
            assert status["verification"] == "unverified", status
            assert not (self.workspace / "one.txt").exists()
            assert not (self.workspace / "two.txt").exists()
            self.phase = "inspect"
            return self.workspace_call("inspect", 0)
        if self.phase == "inspect":
            if "owner" in outputs[-1] and "error:" in outputs[-1]:
                return self.workspace_call("inspect", len(self.inspected))
            result = payload(outputs[-1])
            assert not result["accepted"], result
            assert ("one" if not self.inspected else "two") in result["patch"], result
            self.inspected.append(result)
            if len(self.inspected) < 2:
                return self.workspace_call("inspect", 1)
            self.phase = "integrated"
            return self.workspace_call("integrate", 0)
        if self.phase == "integrated":
            result = payload(outputs[-1])
            assert result["status"] == "integrated" and not result["accepted"], result
            self.phase = "checked"
            # Explicit caller-configured check, never taken from worker prose.
            return self.terminal("manual-check", self.check_command)
        if self.phase == "checked":
            assert "PARENT-CHECK-PASS" in outputs[-1] and (
                "exit: 0" in outputs[-1] or "exit code: 0" in outputs[-1]
            ), outputs[-1]
            self.phase = "refused"
            return self.team(
                "verify",
                {
                    "id": self.run_id,
                    "item": 0,
                    "expected_attempt": 1,
                    "command": "touch "
                    + shlex.quote(str(self.marker("verify-must-not-run"))),
                    "cwd": str(self.workspace),
                    "timeout_ms": 1000,
                },
            )
        assert self.phase == "refused"
        assert "unsupported" in outputs[-1] or "unavailable" in outputs[-1], outputs[-1]
        assert sum("clarification=requested" in t for t in texts) == 1, texts
        return None, "PARENT-DONE: manual check passed; team remains unverified"

    def exercise(self, profile):
        self.profile = profile
        self.env["TNY_TOOLS"] = profile
        launched = self.run_tny(
            "ask", "-B", "--json", "--stdin", stdin=b"PARENT-FLOW parent"
        )
        handle = json.loads(launched.stdout)
        session_id = handle["session_id"]
        result = self.run_tny(
            "session", session_id, "--wait", "--json", timeout=110, check=False
        )
        self.assertEqual(result.returncode, 0, (result.stderr.decode(), self.errors))
        self.assertFalse(self.errors, self.errors)
        self.assertEqual(self.phase, "refused")
        record = self.status(self.run_id)
        self.assertEqual(record["state"], "succeeded")
        self.assertEqual(record["verification"], "unverified")
        root = json.loads(Path(record["metadata_path"]).read_text())
        self.assertEqual(root["parent_session_id"], session_id)
        mailbox = json.loads(
            (Path(record["metadata_path"]).parent / "mailbox.json").read_text()
        )
        messages = {message["id"]: message for message in mailbox["messages"]}
        self.assertEqual(set(messages), {"question", "answer"})
        self.assertEqual(messages["question"]["sender_task"], 1)
        self.assertEqual(messages["question"]["recipient_task"], -1)
        self.assertEqual(messages["answer"]["sender_task"], -1)
        self.assertEqual(messages["answer"]["recipient_task"], 0)
        # Native receipt/cursor persistence prevents model-context replay;
        # acknowledgment is a separate explicit operation, even after delivery.
        self.assertTrue(all(message["state"] == 1 for message in messages.values()))
        self.assertEqual(len(root["items"]), 2)
        self.assertTrue(all(i["role"] == "worker" for i in root["items"]))
        self.assertEqual(len(self.collected), 2)
        worker_sessions = {item["session_id"] for item in record["items"]}
        self.assertEqual(len(worker_sessions), 2)
        self.assertNotIn(session_id, worker_sessions)
        for collection in self.collected:
            self.assertEqual(collection["kind"], "team_collection")
            self.assertEqual(collection["execution"], "succeeded")
            self.assertEqual(collection["verification"], "unverified")
            self.assertEqual(collection["result_integrity"], "matched")
            item = record["items"][collection["item"]]
            self.assertEqual(collection["session_id"], item["session_id"])
            answer = base64.b64decode(collection["result_base64"], validate=True)
            self.assertFalse(collection["result_truncated"])
            self.assertEqual(hashlib.sha256(answer).hexdigest(), item["result_sha256"])
        self.assertFalse(self.marker("verify-must-not-run").exists())
        self.assertEqual(len({c["item"] for c in self.seen}), 2)
        for n in ("one", "two"):
            self.assertEqual(self.marker(n + "-started").read_text(), "started\n")
            self.assertEqual(self.marker(n + "-finished").read_text(), "finished\n")
        self.assertEqual((self.workspace / "one.txt").read_text(), "one\n")
        self.assertFalse((self.workspace / "two.txt").exists())
        for index, n in enumerate(("one", "two")):
            tree = Path(record["items"][index]["workspace_cwd"])
            self.assertEqual((tree / (n + ".txt")).read_text(), n + "\n")
        sessions = [
            p
            for p in (self.home / ".tny").rglob("session.json")
            if p.parent.name == session_id
        ]
        self.assertEqual(len(sessions), 1, sessions)
        session = json.loads(sessions[0].read_text())
        checks = [
            message
            for message in session["messages"]
            if message.get("role") == "tool"
            and message.get("tool_call_id") == "manual-check"
        ]
        self.assertEqual(len(checks), 1, checks)
        check = checks[0]["content"]
        self.assertIn("PARENT-CHECK-PASS", check)
        self.assertIn(str(self.workspace), check)
        calls = [
            call
            for message in session["messages"]
            for call in message.get("tool_calls", [])
            if call["id"] == "manual-check"
        ]
        self.assertEqual(len(calls), 1, calls)
        expected_name, expected_arguments = code_call(
            "terminal", json.dumps({"command": self.check_command})
        )
        self.assertEqual(calls[0]["function"]["name"], expected_name)
        self.assertEqual(
            json.loads(calls[0]["function"]["arguments"]),
            json.loads(expected_arguments),
        )
        self.assertTrue("exit: 0" in check or "exit code: 0" in check, check)
        self.assertEqual(len(self.bodies["one"]), 3, "worker one replayed")
        self.assertEqual(len(self.bodies["two"]), 5, "worker two replayed")

    def test_all_tools_captured_parent(self):
        self.exercise("all")

    def test_terminal_captured_parent(self):
        self.exercise("terminal")


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary())
