#!/usr/bin/env python3
"""Native public-path team collaboration with a real busy tool and client loss."""

import json
import re
import shlex
import subprocess
import threading
import time
import unittest
from pathlib import Path

from test_jobs import TNY, Handler, JobsFixture, argv_without_runner_binary
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
        self.release = self.home / "release-workers"
        self.reply_ready = self.home / "reply-ready"

    def marker(self, name):
        return self.home / name

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
        assert self.submitted.wait(10), (
            "submit did not return while workers were running"
        )
        step = len(outputs)
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
                    self.release,
                    self.marker(tag + "-finished"),
                )
                return self.terminal(tag + "-busy", command), None
            if tag == "two":
                assert not any("answer=bounded" in text for text in texts), texts
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
                    "reply-ready", "touch " + shlex.quote(str(self.reply_ready))
                ), None
            return None, "WORKER-ONE-OK"
        raise AssertionError("unexpected fixture task: " + tag)

    def collaborate(self, profile):
        self.profile = profile
        self.env["TNY_TOOLS"] = profile
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
                    "workspace": {"policy": "shared_writable"},
                }
                for name in ("lead", "one", "two")
            ],
        }
        _, launched = self.submit(
            "ask", "--request", "-", stdin=json.dumps(request).encode()
        )
        self.run_id = launched["id"]
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
            [item["state"] for item in recovered["items"]], ["running"] * 3
        )
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

    def test_all_tools_busy_mail_and_client_recovery(self):
        self.collaborate("all")

    def test_terminal_profile_uses_the_same_mailbox_service(self):
        self.collaborate("terminal")


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary())
