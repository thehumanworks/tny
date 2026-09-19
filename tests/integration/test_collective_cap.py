#!/usr/bin/env python3
"""Real repeat starts share one parent admission cap; synthetic localhost only."""

import json
import signal
import subprocess
import threading
import time
import unittest

from test_jobs import TNY, Handler, JobsFixture
from test_subagent import chat_frames, tool_outputs, user_texts
from test_swarm_parent import payload


class CapHandler(Handler):
    def do_POST(self):
        f = self.server.fixture
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        texts = user_texts(body, "chat")
        tag = next(t for t in texts if t.startswith("CAP-FLOW ")).split()[1]
        with f.lock:
            f.bodies.setdefault(tag, []).append(body)
        try:
            call, answer = f.response(tag, tool_outputs(body, "chat"))
            self.reply(
                200,
                "text/event-stream",
                chat_frames(call=call) if call else chat_frames(text=answer),
            )
        except Exception as e:
            f.errors.append(repr(e))
            f.release.set()
            self.reply(500, "application/json", b'{"error":{"message":"fixture"}}')


class CapAcceptance(JobsFixture):
    def setUp(self):
        super().setUp()
        self.server.RequestHandlerClass = CapHandler
        self.server.fixture = self
        self.env["TNY_TOOLS"] = "all"
        self.env["TNY_SELF_IMPROVE"] = "0"
        self.lock = threading.Lock()
        self.release = threading.Event()
        self.first = threading.Event()
        self.bodies = {}
        self.errors = []
        self.runs = []
        self.outputs = []

    def tearDown(self):
        self.release.set()
        super().tearDown()

    def team(self, count, tag):
        return {
            "kind": "ask",
            "dag": True,
            "concurrency": 1,
            "items": [
                {
                    "role": "worker",
                    "prompt": f"CAP-FLOW {tag}-{i}",
                    "workspace": {"policy": "shared_read_only"},
                }
                for i in range(count)
            ],
        }

    def call(self, id, name, args):
        return (id, name, json.dumps(args)), None

    def start(self, id, count, tag):
        return self.call(
            id, "team_control", {"action": "start", "request": self.team(count, tag)}
        )

    def response(self, tag, outputs):
        if tag == "worker1-0":
            self.first.set()
            assert self.release.wait(15), "first worker never released"
            return None, "FIRST-DONE"
        if tag == "worker2-0":
            return None, "SECOND-DONE"
        assert tag == "parent", tag
        self.outputs = outputs
        n = len(outputs)
        if n == 0:
            return self.start("over-cap", 2, "must-not-start")
        if n == 1:
            assert "cap" in outputs[-1].lower(), outputs[-1]
            assert not self.job_dirs(), "over-cap allocated a job"
            return self.call(
                "no-subagent",
                "subagent",
                {"action": "create", "prompt": "CAP-FLOW must-not-start"},
            )
        if n == 2:
            assert "SWARM_ADMISSION" in outputs[-1], outputs[-1]
            return self.call(
                "no-job-bypass", "job_submit", self.team(2, "must-not-start")
            )
        if n == 3:
            assert "cap" in outputs[-1].lower(), outputs[-1]
            assert not self.job_dirs(), "job_submit bypass allocated a job"
            return self.start("first-valid", 1, "worker1")
        if n == 4:
            self.runs.append(payload(outputs[-1])["run_id"])
            assert self.first.wait(5), "first single worker not started"
            wait_args = ["mailbox", "wait", "--run", self.runs[0], "--timeout-ms"]
            empty = self.run_tny(*wait_args, "0")
            assert json.loads(empty.stdout)["error"] == "MAILBOX_EMPTY"
            deadline = self.run_tny(*wait_args, "20")
            assert json.loads(deadline.stdout)["error"] == "MAILBOX_DEADLINE"
            process = subprocess.Popen(
                [TNY, "--provider", "openai", *wait_args, "30000"],
                cwd=self.workspace,
                env=self.env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                time.sleep(0.1)
                process.send_signal(signal.SIGINT)
                stdout, _ = process.communicate(timeout=3)
                assert process.returncode == 130, process.returncode
                assert json.loads(stdout)["error"] == "MAILBOX_CANCELLED", stdout
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=3)
            return self.start("second-valid", 1, "worker2")
        if n == 5:
            self.runs.append(payload(outputs[-1])["run_id"])

            def queued(record):
                item = record["items"][0]
                return "queued_capacity" in json.dumps(
                    item
                ) or "queued_fifo" in json.dumps(item)

            self.await_state(self.runs[-1], queued, timeout=8, what="shared cap queue")
            assert "worker2-0" not in self.bodies, "second run exceeded active cap1"
            self.release.set()
            return self.call(
                "finish",
                "team_control",
                {
                    "action": "wait-any",
                    "request": {"id": self.runs[-1], "timeout_ms": 10000},
                },
            )
        assert n == 6, (n, outputs)
        assert payload(outputs[-1])["kind"] == "team_completion", outputs[-1]
        return None, "CAP-ACCEPTANCE-PASS"

    def test_single_collaborator_and_aggregate_cap(self):
        result = self.run_tny(
            "--swarm=1", "ask", "CAP-FLOW parent", check=False, timeout=40
        )
        self.assertEqual(result.returncode, 0, (result.stderr.decode(), self.errors))
        self.assertFalse(self.errors, self.errors)
        self.assertIn(b"CAP-ACCEPTANCE-PASS", result.stdout)
        self.assertEqual(set(self.bodies), {"parent", "worker1-0", "worker2-0"})
        self.assertEqual(len(self.bodies["parent"]), 7)
        self.assertEqual(len(self.runs), 2)
        for id in self.runs:
            self.assertEqual(self.await_terminal(id)["state"], "succeeded")
        print(
            "CAP PROOF: rejected over-cap team/subagent/job_submit; two real runs share cap1; second queued until first finished"
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
