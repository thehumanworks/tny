#!/usr/bin/env python3
"""Immutable instruction inheritance for file-defined and durable DAG workers."""

import json
import unittest
from pathlib import Path

from test_jobs import (
    ACCOUNT,
    API_KEY,
    TOKEN,
    JobsDAG,
    JobsFixture,
    argv_without_runner_binary,
    pids_argv,
)


def swarm_definition():
    return {
        "version": 1,
        "purpose": "Verify immutable inherited context.",
        "coordinator": {"name": "root", "purpose": "Coordinate the check."},
        "agents": [{"name": "worker", "purpose": "Report inherited context."}],
        "swarms": [],
    }


def system_text(body):
    return "\n".join(
        message.get("content", "")
        for message in body.get("messages", [])
        if message.get("role") == "system" and isinstance(message.get("content"), str)
    )


class FileSwarmContext(JobsFixture):
    def setUp(self):
        super().setUp()
        self.swarm = self.workspace / "swarm.json"
        self.swarm.write_text(json.dumps(swarm_definition()))

    def worker_systems(self):
        return [
            system_text(body)
            for body in self.state["bodies"]
            if "Participant: worker" in system_text(body)
        ]

    def test_explicit_system_task_and_private_snapshot_are_inherited(self):
        task_dir = self.workspace / ".tny" / "tasks"
        task_dir.mkdir(parents=True)
        task_dir.joinpath("context-review.md").write_text(
            "---\nname: context-review\ndescription: Fixture\n---\n\n"
            "TASK-BODY-SENTINEL\n"
        )
        result = self.run_tny(
            "--task",
            "context-review",
            "--system-prompt",
            "SYSTEM-SENTINEL",
            "--swarm-file",
            str(self.swarm),
            "ask",
            "ROOT-CONTEXT",
            timeout=30,
        )
        self.assertIn(b"answer:ROOT-CONTEXT", result.stdout)
        run = self.await_terminal(self.saved_run_id())
        systems = self.worker_systems()
        self.assertEqual(len(systems), 1, self.state["bodies"])
        self.assertIn("SYSTEM-SENTINEL", systems[0])
        self.assertIn("TASK-BODY-SENTINEL", systems[0])

        sidecar = Path(run["metadata_path"]).parent / "context.json"
        self.assertEqual(sidecar.stat().st_mode & 0o777, 0o600)
        private = sidecar.read_text()
        public = json.dumps(run)
        for credential in (API_KEY, TOKEN, ACCOUNT, self.env["OPENAI_BASE_URL"]):
            self.assertNotIn(credential, private)
        self.assertNotIn("SYSTEM-SENTINEL", public)
        self.assertNotIn("TASK-BODY-SENTINEL", public)
        live_argv = "\n".join(pids_argv(str(sidecar.parent.name), str(self.home)))
        self.assertNotIn("SYSTEM-SENTINEL", live_argv)
        self.assertNotIn("TASK-BODY-SENTINEL", live_argv)

    def test_context_disabled_remains_disabled_for_worker(self):
        (self.workspace / "AGENTS.md").write_text("MUST-NOT-RELOAD-CONTEXT\n")
        (self.workspace / ".tny.json").write_text(json.dumps({"context": False}))
        self.run_tny(
            "--swarm-file", str(self.swarm), "ask", "ROOT-NO-CONTEXT", timeout=30
        )
        self.await_terminal(self.saved_run_id())
        systems = self.worker_systems()
        self.assertEqual(len(systems), 1, self.state["bodies"])
        self.assertNotIn("MUST-NOT-RELOAD-CONTEXT", systems[0])

    def saved_run_id(self):
        sessions = list((self.home / ".tny").rglob("session.json"))
        roots = [json.loads(path.read_text()) for path in sessions]
        runs = [
            root["swarm_definition"]["run_id"]
            for root in roots
            if "swarm_definition" in root
        ]
        self.assertEqual(len(runs), 1)
        return runs[0]


class DurableContextSnapshot(JobsDAG):
    def submit_barrier(self):
        run, payload = self.dag_submit(
            [
                {"prompt": "DAG_BARRIER snapshot producer"},
                {"prompt": "CONTEXT_CONSUMER", "depends_on": [0]},
            ],
            concurrency=1,
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertTrue(self.state["dag_entered"].wait(30))
        return payload["id"]

    def test_agents_change_after_capture_is_not_reloaded(self):
        agents = self.workspace / "AGENTS.md"
        agents.write_text("CAPTURED-INSTRUCTIONS\n")
        job_id = self.submit_barrier()
        agents.write_text("AMBIENT-CHANGED-INSTRUCTIONS\n")
        self.state["dag_release"].set()
        final = self.await_terminal(job_id)
        self.assertEqual(final["state"], "succeeded", final)
        consumer = next(
            body
            for body in self.state["bodies"]
            if any(
                message.get("role") == "user"
                and "CONTEXT_CONSUMER" in str(message.get("content", ""))
                for message in body.get("messages", [])
            )
        )
        system = system_text(consumer)
        self.assertIn("CAPTURED-INSTRUCTIONS", system)
        self.assertNotIn("AMBIENT-CHANGED-INSTRUCTIONS", system)

    def test_missing_sidecar_fails_closed_before_consumer_request(self):
        job_id = self.submit_barrier()
        (self.jobs_root() / job_id / "context.json").unlink()
        self.state["dag_release"].set()
        final = self.await_terminal(job_id)
        self.assertEqual(final["items"][1]["state"], "failed", final)
        self.assertFalse(
            any("CONTEXT_CONSUMER" in prompt for prompt in self.ask_requests())
        )

    def test_corrupt_sidecar_fails_closed_before_consumer_request(self):
        job_id = self.submit_barrier()
        sidecar = self.jobs_root() / job_id / "context.json"
        snapshot = json.loads(sidecar.read_text())
        snapshot["payload"]["instructions_snapshot"] += "CORRUPT"
        sidecar.write_text(json.dumps(snapshot))
        self.state["dag_release"].set()
        final = self.await_terminal(job_id)
        self.assertEqual(final["items"][1]["state"], "failed", final)
        self.assertFalse(
            any("CONTEXT_CONSUMER" in prompt for prompt in self.ask_requests())
        )

    def test_oversized_capture_is_refused_before_job_or_request(self):
        (self.workspace / "AGENTS.md").write_text("x" * (2 * 1024 * 1024 + 1))
        run, _payload = self.dag_submit([{"prompt": "MUST_NOT_POST"}])
        self.assertNotEqual(run.returncode, 0)
        self.assertFalse(self.job_dirs())
        self.assertFalse(self.state["bodies"])


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary())
