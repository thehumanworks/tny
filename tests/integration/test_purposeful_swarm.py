#!/usr/bin/env python3
"""Executable file-defined nested swarms over the real durable team runtime."""

import json
import unittest

from test_jobs import JobsFixture, argv_without_runner_binary


def definition(agent_name="root-agent"):
    return {
        "version": 1,
        "purpose": "Produce independent implementation and verification evidence.",
        "coordinator": {"name": "root-lead", "purpose": "Own the final synthesis."},
        "agents": [
            {"name": agent_name, "purpose": "Inspect the implementation evidence."}
        ],
        "swarms": [
            {
                "purpose": "Verify the result independently.",
                "coordinator": {
                    "name": "verify-lead",
                    "purpose": "Coordinate checks and synthesize upward.",
                },
                "agents": [{"name": "test-agent", "purpose": "Inspect test evidence."}],
                "swarms": [],
            }
        ],
    }


class PurposefulSwarm(JobsFixture):
    def write_definition(self, value=None):
        path = self.workspace / "swarm.json"
        path.write_text(json.dumps(value or definition()))
        return path

    def saved_session(self):
        paths = list((self.home / ".tny").rglob("session.json"))
        roots = [p for p in paths if "swarm_definition" in json.loads(p.read_text())]
        self.assertEqual(len(roots), 1)
        return roots[0], json.loads(roots[0].read_text())

    def test_nested_definition_launches_one_flat_fenced_team(self):
        path = self.write_definition()
        result = self.run_tny(
            "--swarm-file", str(path), "ask", "PURPOSEFUL_ROOT_TASK", timeout=30
        )
        self.assertIn(b"answer:PURPOSEFUL_ROOT_TASK", result.stdout)
        session_path, session = self.saved_session()
        meta = session["swarm_definition"]
        self.assertEqual(meta["activation"], "active")
        self.assertEqual(meta["participants"], 3)
        self.assertEqual(session["swarm_cap"], 3)
        self.assertEqual(meta["source"], str(path.resolve()))
        run_id = meta["run_id"]
        run = self.await_terminal(run_id)
        self.assertEqual(run["parent_session_id"], session_path.parent.name)
        self.assertEqual(run["concurrency"], 3)
        self.assertEqual(run["admission"]["cap"], 3)
        self.assertTrue(run["peer_messages"])
        self.assertEqual(run["swarm_root_coordinator"], "root-lead")
        self.assertEqual(
            [
                (item["swarm_name"], item["swarm_role"], item["swarm_group"])
                for item in run["items"]
            ],
            [
                ("root-agent", "agent", 0),
                ("verify-lead", "coordinator", 1),
                ("test-agent", "agent", 1),
            ],
        )
        self.assertTrue(all(item["role"] == "worker" for item in run["items"]))
        systems = [
            message.get("content", "")
            for body in self.state["bodies"]
            for message in body.get("messages", [])
            if message.get("role") == "system"
        ]
        self.assertTrue(any("Participant: verify-lead" in text for text in systems))
        self.assertTrue(any("Participant: test-agent" in text for text in systems))

    def test_resume_uses_snapshot_and_explicit_changed_file_refuses(self):
        path = self.write_definition()
        self.run_tny("--swarm-file", str(path), "ask", "FIRST_ROOT", timeout=30)
        session_path, saved = self.saved_session()
        self.await_terminal(saved["swarm_definition"]["run_id"])
        before_jobs = len(list((self.home / ".tny" / "jobs").glob("*/job.json")))
        before_requests = len(self.state["bodies"])
        path.write_text("changed and no longer JSON")
        self.run_tny(
            "--resume", session_path.parent.name, "ask", "SECOND_ROOT", timeout=30
        )
        self.assertEqual(
            len(list((self.home / ".tny" / "jobs").glob("*/job.json"))), before_jobs
        )
        self.assertEqual(len(self.state["bodies"]), before_requests + 1)

        path.write_text(json.dumps(definition("changed-agent")))
        before_requests = len(self.state["bodies"])
        refused = self.run_tny(
            "--swarm-file",
            str(path),
            "--resume",
            session_path.parent.name,
            "ask",
            "MUST_NOT_POST",
            check=False,
            timeout=15,
        )
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn(b"differs from the saved session", refused.stderr)
        self.assertEqual(len(self.state["bodies"]), before_requests)

    def test_activation_honors_denial_and_unresolved_ask_without_submitting(self):
        path = self.write_definition()
        settings = self.home / ".tny" / "settings.json"
        settings.parent.mkdir(parents=True, exist_ok=True)
        for rules in ({"rules": [{"tool": "team_start", "allow": False}]}, {}):
            with self.subTest(rules=rules):
                settings.write_text(json.dumps({"permission": rules}))
                result = self.run_tny(
                    "--permission-mode",
                    "ask",
                    "--swarm-file",
                    str(path),
                    "ask",
                    "MUST_NOT_POST",
                    check=False,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b"permission", result.stderr)
                self.assertEqual(self.state["bodies"], [])
                self.assertFalse(list((self.home / ".tny" / "jobs").glob("*/job.json")))

    def test_invalid_and_unsupported_inputs_have_no_execution_effects(self):
        invalid = definition()
        invalid["swarms"][0].pop("coordinator")
        path = self.write_definition(invalid)
        result = self.run_tny(
            "--swarm-file", str(path), "ask", "MUST_NOT_POST", check=False
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.state["bodies"], [])
        self.assertFalse((self.home / ".tny" / "jobs").exists())

        path = self.write_definition()
        result = self.run_tny(
            "--swarm-file",
            str(path),
            "--ephemeral",
            "ask",
            "MUST_NOT_POST",
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.state["bodies"], [])
        self.assertFalse((self.home / ".tny" / "jobs").exists())


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary())
