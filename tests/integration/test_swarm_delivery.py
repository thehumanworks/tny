#!/usr/bin/env python3
"""Native public-path team collaboration with a real busy tool and client loss."""

import json
import os
import re
import shlex
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

from test_jobs import TNY, WASM, Handler, JobsFixture, argv_without_runner_binary
from test_subagent import chat_frames, tool_outputs, user_texts


def wait_for(predicate, seconds=30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("observable fixture barrier did not arrive")


def wait_command(started, release, finished):
    # This is one real terminal call. Message send must neither interrupt nor
    # replay it. Each invocation appends a marker, so a replay is observable.
    code = (
        "from pathlib import Path; import time; "
        f"s=Path({str(started)!r}); r=Path({str(release)!r}); "
        "s.open('a').write('started\\n'); end=time.monotonic()+45\n"
        "while not r.exists() and time.monotonic()<end: time.sleep(.01)\n"
        "assert r.exists(), 'fixture release timed out'\n"
        f"Path({str(finished)!r}).open('a').write('finished\\n')"
    )
    return "python3 -c " + shlex.quote(code)


class SwarmHandler(Handler):
    def do_POST(self):
        fixture = self.server.fixture
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        texts = user_texts(body, "chat")
        prompt = next((text for text in texts if text.startswith("SWARM ")), "")
        tag = prompt.split()[1] if prompt else "unknown"
        outputs = tool_outputs(body, "chat")
        with fixture.lock:
            fixture.bodies.setdefault(tag, []).append(body)
        self.enter(prompt)
        try:
            call, answer = fixture.response(tag, texts, outputs)
            data = chat_frames(call=call) if call else chat_frames(text=answer)
            self.reply(200, "text/event-stream", data)
        except Exception as error:  # Surface fixture assertions to the test owner.
            with fixture.lock:
                fixture.errors.append(str(error))
            self.reply(
                500, "application/json", b'{"error":{"message":"fixture assertion"}}'
            )
        finally:
            self.leave()


class SwarmDelivery(JobsFixture):
    def setUp(self):
        super().setUp()
        self.server.RequestHandlerClass = SwarmHandler
        self.server.fixture = self
        self.lock = threading.Lock()
        self.bodies = {}
        self.errors = []
        self.run_id = None
        self.submitted = threading.Event()
        self.profile = "all"
        self.unrelated_entered = threading.Event()
        self.unrelated_release = threading.Event()
        self.cancel_release = threading.Event()
        self.release = self.home / "release-workers"
        self.reply_ready = self.home / "reply-ready"

    def marker(self, name):
        return self.home / name

    def git(self, *args):
        return subprocess.run(
            ["git", "-C", str(self.workspace), *args],
            env=self.env,
            text=True,
            capture_output=True,
            check=True,
            timeout=20,
        ).stdout.strip()

    def mailbox_call(self, name, target, text):
        args = {
            "action": "send",
            "run": self.run_id,
            "to": target,
            "id": name,
            "text": text,
        }
        if self.profile == "all":
            return name, "team_mailbox", json.dumps(args)
        command = "tny mailbox send --run {} --to {} --id {} --text {}".format(
            self.run_id, target, name, shlex.quote(text)
        )
        return name, "terminal", json.dumps({"command": command})

    def terminal(self, name, command):
        return name, "terminal", json.dumps({"command": command})

    def response(self, tag, texts, outputs):
        if tag == "unrelated":
            self.unrelated_entered.set()
            self.unrelated_release.wait(30)
            return None, "UNRELATED-OK"
        assert self.submitted.wait(10), (
            "submit did not return while workers were running"
        )
        step = len(outputs)
        if tag.startswith("passive"):
            return None, "PASSIVE-OK"
        if tag == "owned-background":
            if step == 0:
                return (
                    "detach-refused",
                    "terminal",
                    json.dumps(
                        {
                            "command": "printf detached > detached-canary",
                            "background": True,
                        }
                    ),
                ), None
            assert "owned job" in outputs[-1] and "exit: 0" not in outputs[-1], outputs[
                -1
            ]
            return None, "NO-DETACH"
        if tag == "cancel-self":
            if step == 0:
                return (
                    "cancel-self",
                    "team_control",
                    json.dumps(
                        {
                            "action": "cancel",
                            "request": {
                                "id": self.run_id,
                                "item": 1,
                                "expected_attempt": 1,
                            },
                        }
                    ),
                ), None
            self.cancel_release.wait(30)
            return None, "CANCEL-REQUESTED"
        if tag == "lead":
            if step == 0:
                command = wait_command(
                    self.marker("lead-started"),
                    self.marker("one-started"),
                    self.marker("lead-ready"),
                )
                return self.terminal("lead-ready", command), None
            if step == 1:
                return self.mailbox_call(
                    "question-1", 1, "answer=bounded; inspect only the assigned task"
                ), None
            if step == 2:
                assert "question-1" in outputs[-1] and "error:" not in outputs[-1], (
                    outputs[-1]
                )
                self.marker("question-sent").write_text("sent")
                command = wait_command(
                    self.marker("lead-busy"),
                    self.reply_ready,
                    self.marker("lead-finished"),
                )
                return self.terminal("lead-collect", command), None
            assert sum("result=bounded" in text for text in texts) == 1, texts
            return None, "LEAD-OK"
        if tag in ("one", "two"):
            if step == 0:
                command = wait_command(
                    self.marker(tag + "-started"),
                    self.marker("release-two") if tag == "two" else self.release,
                    self.marker(tag + "-finished"),
                )
                return self.terminal(tag + "-busy", command), None
            if tag == "two":
                assert not any("answer=bounded" in text for text in texts), texts
                if step == 1:
                    return self.terminal(
                        "edit-two",
                        "printf 'worker two\\n' > same.txt && git add same.txt && git commit -m worker-two",
                    ), None
                return None, "WORKER-TWO-OK"
            assert sum("answer=bounded" in text for text in texts) == 1, texts
            if step == 1:
                return self.mailbox_call(
                    "reply-1",
                    0,
                    "result=bounded; clarification applied only to task one",
                ), None
            if step == 2:
                assert "reply-1" in outputs[-1] and "error:" not in outputs[-1], (
                    outputs[-1]
                )
                return self.terminal(
                    "reply-ready",
                    "printf 'worker one\\n' > same.txt && git add same.txt && git commit -m worker-one && touch "
                    + shlex.quote(str(self.reply_ready)),
                ), None
            if step == 3:
                if self.profile == "all":
                    return (
                        "member-status",
                        "team_control",
                        json.dumps(
                            {"action": "status", "request": {"id": self.run_id}}
                        ),
                    ), None
                command = "tny team status --request " + shlex.quote(
                    str(self.home / "status-request.json")
                )
                return self.terminal("member-status", command), None
            assert self.run_id in outputs[-1] and "error:" not in outputs[-1], outputs[
                -1
            ]
            return None, "WORKER-ONE-OK"
        raise AssertionError("unexpected fixture task: " + tag)

    def collaborate(self, profile):
        self.profile = profile
        self.env["TNY_TOOLS"] = profile
        for key in list(self.env):
            if key.startswith(("TNY_TEAM_", "TNY_ADMISSION_")) or key in (
                "TNY_NESTED",
                "TNY_NESTED_MODE",
                "TNY_SESSION_ID",
                "TNY_SESSION_SOCK",
            ):
                self.env.pop(key)
        self.git("init", "-b", "main")
        self.git("config", "user.name", "Swarm Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        (self.workspace / "same.txt").write_text("base\n")
        self.git("add", "same.txt")
        self.git("commit", "-m", "base")
        request = {
            "kind": "ask",
            "dag": True,
            "peer_messages": True,
            "concurrency": 3,
            "items": [
                {
                    "prompt": "SWARM " + name,
                    "role": "lead" if name == "lead" else "worker",
                    "label": name,
                    "workspace": {
                        "policy": "shared_writable" if name == "lead" else "isolated"
                    },
                }
                for name in ("lead", "one", "two")
            ],
        }
        _, launched = self.submit(
            "ask", "--request", "-", stdin=json.dumps(request).encode()
        )
        self.run_id = launched["id"]
        (self.home / "status-request.json").write_text(json.dumps({"id": self.run_id}))
        self.submitted.set()
        wait_for(
            lambda: all(
                self.marker(name).exists()
                for name in ("one-started", "two-started", "question-sent")
            )
        )
        self.assertFalse(
            self.marker("one-finished").exists(), "send interrupted the busy tool"
        )
        self.assertFalse(self.marker("two-finished").exists())
        running = self.status(self.run_id)
        self.assertEqual([item["state"] for item in running["items"]], ["running"] * 3)
        self.assertEqual(running["verification"], "unverified")
        mailbox = Path(running["metadata_path"]).parent / "mailbox.json"
        before = mailbox.read_bytes()
        forged = dict(
            self.env,
            TNY_NESTED="1",
            TNY_TEAM_RUN=self.run_id,
            TNY_TEAM_TASK="0",
            TNY_TEAM_ATTEMPT="1",
            TNY_TEAM_CAPABILITY="0" * 64,
        )
        denied = self.run_tny(
            "mailbox",
            "send",
            "--run",
            self.run_id,
            "--to",
            "1",
            "--id",
            "forged",
            "--text",
            "not-authorized",
            env=forged,
            check=False,
        )
        self.assertNotEqual(denied.returncode, 0)
        self.assertEqual(
            mailbox.read_bytes(), before, "wrong member modified the inbox"
        )

        self.marker("release-two").write_text("release")
        wait_for(lambda: self.status(self.run_id)["items"][2]["state"] == "succeeded")
        completed_hash = self.status(self.run_id)["items"][2]["result_sha256"]
        self.assertEqual((self.workspace / "same.txt").read_text(), "base\n")

        # A client wait is disposable; killing it must not cancel its workers.
        client = subprocess.Popen(
            [TNY, "--provider", "openai", "jobs", "wait", self.run_id, "--json"],
            cwd=self.workspace,
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            client.terminate()
            client.communicate(timeout=10)
        finally:
            if client.poll() is None:
                client.kill()
                client.communicate(timeout=10)
        recovered = json.loads(
            self.run_tny("agents", "--run", self.run_id, "--json").stdout
        )["run"]
        self.assertEqual(
            [item["state"] for item in recovered["items"]],
            ["running", "running", "succeeded"],
        )
        self.assertEqual(recovered["items"][2]["result_sha256"], completed_hash)
        self.release.write_text("release")
        finished = self.await_terminal(self.run_id, timeout=60)
        self.assertEqual(finished["state"], "succeeded", finished)
        self.assertFalse(self.errors, self.errors)
        for name in ("one", "two"):
            self.assertEqual(self.marker(name + "-started").read_text(), "started\n")
            self.assertEqual(self.marker(name + "-finished").read_text(), "finished\n")
        self.assertEqual(len({item["session_id"] for item in finished["items"]}), 3)
        self.assertEqual(
            finished["verification"], "unverified", "execution was mislabeled accepted"
        )
        self.assertEqual(finished["usage"]["known_input_tokens"], 120)
        self.assertEqual(finished["usage"]["known_output_tokens"], 24)
        self.assertEqual(len(self.bodies["two"]), 3, "completed task was replayed")
        self.assertEqual((self.workspace / "same.txt").read_text(), "base\n")
        inspection = {}

        def inspect_ready():
            result = self.run_tny(
                "task-workspace",
                "inspect",
                "--run",
                self.run_id,
                "--task",
                "1",
                "--attempt",
                "1",
                "--json",
                check=False,
            )
            if result.returncode:
                self.assertIn(b"owner", result.stderr)
                return False
            inspection.update(json.loads(result.stdout))
            return True

        wait_for(inspect_ready)
        self.assertIn("worker one", inspection["patch"])
        self.assertFalse(inspection["accepted"])
        first = self.run_tny(
            "task-workspace",
            "integrate",
            "--run",
            self.run_id,
            "--task",
            "1",
            "--attempt",
            "1",
            "--json",
        )
        self.assertEqual(json.loads(first.stdout)["status"], "integrated")
        self.git("commit", "-m", "explicit operator integration")
        conflict = self.run_tny(
            "task-workspace",
            "integrate",
            "--run",
            self.run_id,
            "--task",
            "2",
            "--attempt",
            "1",
            "--json",
            check=False,
        )
        self.assertEqual(conflict.returncode, 2, conflict.stderr)
        self.assertEqual(json.loads(conflict.stdout)["status"], "conflict")
        self.assertFalse(json.loads(conflict.stdout)["accepted"])
        self.assertTrue(self.git("ls-files", "--unmerged"))
        for index, content in ((1, "worker one\n"), (2, "worker two\n")):
            tree = Path(finished["items"][index]["workspace_cwd"])
            self.assertEqual((tree / "same.txt").read_text(), content)
        self.assertEqual(finished["usage"]["unknown_items"], 0)
        record_text = Path(finished["metadata_path"]).read_text()
        self.assertNotIn("TNY_TEAM_CAPABILITY=", record_text)
        for item in finished["items"]:
            self.assertRegex(item["result_sha256"], r"^[0-9a-f]{64}$")
        one_users = user_texts(self.bodies["one"][-1], "chat")
        self.assertEqual(sum("answer=bounded" in text for text in one_users), 1)
        self.assertTrue(
            any(
                re.search(
                    r"Untrusted team message; run [0-9a-f]{32}; id question-1", text
                )
                for text in one_users
            )
        )

    def test_owned_jobs_cannot_detach_first_party_terminal_work(self):
        self.submitted.set()
        for profile in ("all", "terminal"):
            self.env["TNY_TOOLS"] = profile
            for dag in (False, True):
                request = {
                    "kind": "ask",
                    "concurrency": 1,
                    "items": [{"prompt": "SWARM owned-background"}],
                }
                if dag:
                    request.update(
                        dag=True,
                        admission={
                            "label": "owned-" + profile,
                            "provider_scope": "fixture-account",
                            "cap": 1,
                            "queue_cap": 4,
                            "claim_limit": 2,
                        },
                    )
                    request["items"][0]["workspace"] = {"policy": "shared_writable"}
                _, launched = self.submit(
                    "ask", "--request", "-", stdin=json.dumps(request).encode()
                )
                record = self.await_terminal(launched["id"])
                self.assertEqual(record["state"], "succeeded", record)
                self.assertFalse((self.workspace / "detached-canary").exists())
                self.assertFalse((self.home / ".tny" / "terminal").exists())
        self.assertFalse(self.errors, self.errors)

    def test_enrolled_background_ask_refuses_before_session_or_provider_effects(self):
        env = dict(self.env, TNY_ADMISSION_ENROLLED="1", TNY_JOB_PARENT_PID="1")
        before = list((self.home / ".tny").glob("sessions/*/*/session.json"))
        result = self.run_tny(
            "ask", "-B", "--json", "SWARM forbidden", env=env, check=False
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"shared admission", result.stderr)
        self.assertFalse(self.state["requests"])
        self.assertEqual(
            list((self.home / ".tny").glob("sessions/*/*/session.json")), before
        )

    def test_member_self_cancel_uses_a_scoped_adapter_and_leaves_unrelated_work(self):
        self.env.pop("TNY_NESTED", None)
        self.env["TNY_TOOLS"] = "all"
        _, unrelated = self.submit("ask", stdin=b"SWARM unrelated")
        try:
            self.assertTrue(self.unrelated_entered.wait(10))
            request = {
                "kind": "ask",
                "dag": True,
                "concurrency": 3,
                "items": [
                    {"role": "lead", "prompt": "SWARM passive-lead"},
                    {"role": "worker", "prompt": "SWARM cancel-self"},
                    {"role": "worker", "prompt": "SWARM passive-worker"},
                ],
            }
            _, launched = self.submit(
                "ask", "--request", "-", stdin=json.dumps(request).encode()
            )
            self.run_id = launched["id"]
            self.submitted.set()
            stopped = self.await_terminal(self.run_id, timeout=25)
            self.assertEqual(stopped["items"][1]["state"], "cancelled", stopped)
            self.assertEqual(stopped["items"][0]["state"], "succeeded")
            self.assertEqual(stopped["items"][2]["state"], "succeeded")
            self.assertEqual(self.status(unrelated["id"])["state"], "running")
        finally:
            self.cancel_release.set()
            self.unrelated_release.set()
        self.assertEqual(self.await_terminal(unrelated["id"])["state"], "succeeded")

    def test_all_tools_busy_mail_and_client_recovery(self):
        self.collaborate("all")

    def test_terminal_profile_uses_the_same_mailbox_service(self):
        self.collaborate("terminal")


class SwarmUnsupported(unittest.TestCase):
    @unittest.skipUnless(WASM, "wasm refusal runs in the wasm CI lane")
    def test_native_services_refuse_before_job_or_workspace_side_effects(self):
        with tempfile.TemporaryDirectory(prefix="tny-swarm-wasm-") as root:
            env = dict(os.environ, HOME=root)
            env.pop("TNY_NESTED", None)
            request = {
                "kind": "ask",
                "dag": True,
                "items": [
                    {"role": role, "prompt": "do not execute"}
                    for role in ("lead", "worker", "worker")
                ],
            }
            cases = [
                (["team", "start", "--request", "-"], json.dumps(request).encode()),
                (["mailbox", "inbox", "--run", "0" * 32], b""),
                (
                    [
                        "task-workspace",
                        "inspect",
                        "--run",
                        "0" * 32,
                        "--task",
                        "0",
                        "--attempt",
                        "1",
                    ],
                    b"",
                ),
            ]
            for args, data in cases:
                run = subprocess.run(
                    [TNY, "--provider", "openai", *args],
                    cwd=root,
                    env=env,
                    input=data,
                    capture_output=True,
                    timeout=120,
                )
                self.assertNotEqual(run.returncode, 0)
                self.assertIn(b"native", run.stderr.lower())
                self.assertFalse((Path(root) / ".tny" / "jobs").exists())
                self.assertFalse((Path(root) / ".tny" / "worktrees").exists())


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary())
