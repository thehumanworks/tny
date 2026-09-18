#!/usr/bin/env python3
"""Run-filtered CLI and PTY task tree over real durable jobs, not session guesses."""

import json
import unittest

from test_jobs import TNY, JobsFixture, argv_without_runner_binary
from test_tui import Term


class SwarmAgents(JobsFixture):
    def start_run(self):
        request = {
            "kind": "ask",
            "dag": True,
            "concurrency": 3,
            "items": [
                {
                    "prompt": "READ worker one",
                    "label": "reader-one\n\u001b[31m",
                    "role": "worker",
                },
                {"prompt": "READ worker two", "label": "reader-two", "role": "worker"},
                {
                    "prompt": "READ lead",
                    "label": "coordinator",
                    "role": "lead",
                    "depends_on": [0, 1],
                },
            ],
        }
        _, launched = self.submit(
            "ask", "--request", "-", stdin=json.dumps(request).encode()
        )
        return launched["id"]

    def test_json_and_plain_tree_use_only_the_requested_runs_members(self):
        first = self.start_run()
        second = self.start_run()
        expected = self.await_terminal(first)
        self.await_terminal(second)
        before = len(self.ask_requests())
        result = self.run_tny("agents", "--run", first, "--json")
        projected = json.loads(result.stdout)
        self.assertEqual(projected["kind"], "agents")
        self.assertEqual(projected["run"]["id"], first)
        self.assertEqual(projected["run"]["items"], expected["items"])
        self.assertEqual(projected["run"]["verification"], "unverified")
        self.assertNotIn(second, result.stdout.decode())
        self.assertEqual(
            [item["task_id"] for item in projected["run"]["items"]], [0, 1, 2]
        )
        plain = self.run_tny("agents", "--run", first).stdout.decode()
        for label in ("reader-one", "reader-two", "coordinator"):
            self.assertIn(label, plain)
        self.assertIn("verification: unverified", plain)
        self.assertNotIn("\u001b", plain, "untrusted label injected terminal controls")
        self.assertEqual(
            len(self.ask_requests()), before, "inspection started provider work"
        )
        ordinary = json.loads(self.run_tny("agents", "--json").stdout)
        self.assertIn("agents", ordinary)
        self.assertNotIn("run", ordinary)

    def test_invalid_and_non_dag_ids_are_refused(self):
        before = len(self.ask_requests())
        for args in (("--run", "../wrong"), ("--run",), ("--run", "f" * 32)):
            result = self.run_tny("agents", *args, "--json", check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(result.stdout)
        self.assertEqual(len(self.ask_requests()), before)
        _, ordinary = self.submit("ask", stdin=b"ordinary batch")
        self.await_terminal(ordinary["id"])
        result = self.run_tny("agents", "--run", ordinary["id"], "--json", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"not an ordinary batch", result.stderr)

    def test_tty_tree_is_status_only_and_does_not_launch_a_provider(self):
        run_id = self.start_run()
        self.await_terminal(run_id)
        before = len(self.ask_requests())
        term = Term([TNY, "agents", "--run", run_id], self.env, str(self.workspace))
        try:
            term.expect("task status")
            term.expect("coordinator")
            term.expect("unverified")
            term.send("\r")
            term.pump(0.1)
            term.send("q")
            self.assertEqual(term.wait(), 0)
            self.assertEqual(len(self.ask_requests()), before)
        finally:
            term.close()


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary())
