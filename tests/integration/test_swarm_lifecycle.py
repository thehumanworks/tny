#!/usr/bin/env python3
"""Purposeful activation identity, strict restore, and topology provenance."""

import copy
import json
import shutil
import unittest

from test_jobs import JobsFixture, argv_without_runner_binary


def definition():
    return {
        "version": 1,
        "purpose": "Produce implementation and verification evidence.",
        "coordinator": {"name": "root-lead", "purpose": "Own synthesis."},
        "agents": [{"name": "reviewer", "purpose": "Review evidence."}],
        "swarms": [
            {
                "purpose": "Verify independently.",
                "coordinator": {"name": "verify-lead", "purpose": "Coordinate checks."},
                "agents": [{"name": "tester", "purpose": "Inspect test evidence."}],
                "swarms": [],
            }
        ],
    }


class SwarmLifecycle(JobsFixture):
    def launch(self):
        path = self.workspace / "swarm.json"
        path.write_text(json.dumps(definition()))
        self.run_tny("--swarm-file", str(path), "ask", "ROOT_TASK", timeout=30)
        sessions = [
            path
            for path in (self.home / ".tny").rglob("session.json")
            if "swarm_definition" in json.loads(path.read_text())
        ]
        self.assertEqual(len(sessions), 1)
        session_path = sessions[0]
        session = json.loads(session_path.read_text())
        run_id = session["swarm_definition"]["run_id"]
        self.await_terminal(run_id)
        return session_path, session, run_id

    def resume(self, session_path, prompt="RESUMED_ROOT", check=False):
        return self.run_tny(
            "--resume",
            session_path.parent.name,
            "ask",
            prompt,
            check=check,
            timeout=30,
        )

    @staticmethod
    def make_launching(session):
        meta = session["swarm_definition"]
        meta["activation"] = "launching"
        meta.pop("run_id")
        session["team_runs"] = []
        session["messages"] = [
            message
            for message in session["messages"]
            if "Purposeful swarm activated as durable run"
            not in message.get("content", "")
        ]

    def test_interrupted_activation_adopts_exact_durable_identity_once(self):
        session_path, session, run_id = self.launch()
        meta = session["swarm_definition"]
        activation_id = meta["activation_id"]
        self.assertEqual(len(activation_id), 32)
        job_path = self.home / ".tny" / "jobs" / run_id / "job.json"
        self.assertEqual(
            json.loads(job_path.read_text())["swarm_activation_id"], activation_id
        )

        self.make_launching(session)
        session_path.write_text(json.dumps(session))
        before_jobs = sorted(
            path.parent.name for path in job_path.parents[1].glob("*/job.json")
        )
        before_requests = len(self.state["bodies"])

        recovered = self.resume(session_path, check=True)
        self.assertIn(b"answer:RESUMED_ROOT", recovered.stdout)
        restored = json.loads(session_path.read_text())["swarm_definition"]
        self.assertEqual(restored["activation"], "active")
        self.assertEqual(restored["activation_id"], activation_id)
        self.assertEqual(restored["run_id"], run_id)
        self.assertEqual(
            sorted(path.parent.name for path in job_path.parents[1].glob("*/job.json")),
            before_jobs,
        )
        self.assertEqual(len(self.state["bodies"]), before_requests + 1)

    def test_interrupted_activation_zero_match_retries_same_identity(self):
        session_path, session, old_run = self.launch()
        activation_id = session["swarm_definition"]["activation_id"]
        jobs = self.home / ".tny" / "jobs"
        shutil.rmtree(jobs / old_run)
        self.make_launching(session)
        session_path.write_text(json.dumps(session))

        self.resume(session_path, check=True)
        restored = json.loads(session_path.read_text())["swarm_definition"]
        self.assertEqual(restored["activation"], "active")
        self.assertEqual(restored["activation_id"], activation_id)
        self.assertNotEqual(restored["run_id"], old_run)
        records = list(jobs.glob("*/job.json"))
        self.assertEqual(len(records), 1)
        self.assertEqual(
            json.loads(records[0].read_text())["swarm_activation_id"], activation_id
        )

    def test_interrupted_activation_refuses_ambiguous_matching_jobs(self):
        session_path, session, run_id = self.launch()
        self.make_launching(session)
        session_path.write_text(json.dumps(session))

        jobs = self.home / ".tny" / "jobs"
        duplicate_id = "f" * 32
        while duplicate_id == run_id:
            duplicate_id = "e" * 32
        duplicate = jobs / duplicate_id
        shutil.copytree(jobs / run_id, duplicate)
        record_path = duplicate / "job.json"
        record = json.loads(record_path.read_text())
        record["id"] = duplicate_id
        record["run_id"] = duplicate_id
        record_path.write_text(json.dumps(record))
        before_requests = len(self.state["bodies"])

        refused = self.resume(session_path)
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn(b"activation", refused.stderr)
        self.assertEqual(len(self.state["bodies"]), before_requests)
        self.assertEqual(len(list(jobs.glob("*/job.json"))), 2)

    def test_corrupt_saved_state_and_run_provenance_fail_before_provider(self):
        session_path, session, run_id = self.launch()
        job_path = self.home / ".tny" / "jobs" / run_id / "job.json"
        original_job = json.loads(job_path.read_text())
        original_session = copy.deepcopy(session)
        before_requests = len(self.state["bodies"])

        session_mutations = {
            "cap_type": lambda value: value.__setitem__("swarm_cap", "3"),
            "cap_count": lambda value: value.__setitem__("swarm_cap", 2),
            "activation_type": lambda value: value["swarm_definition"].__setitem__(
                "activation", 7
            ),
            "active_without_identity": lambda value: value["swarm_definition"].pop(
                "activation_id"
            ),
            "run_id_type": lambda value: value["swarm_definition"].__setitem__(
                "run_id", 7
            ),
            "activation_id_type": lambda value: value["swarm_definition"].__setitem__(
                "activation_id", 7
            ),
            "wrong_participant_count": lambda value: value[
                "swarm_definition"
            ].__setitem__("participants", 2),
        }
        for name, mutate in session_mutations.items():
            with self.subTest(name=name):
                candidate = copy.deepcopy(original_session)
                mutate(candidate)
                session_path.write_text(json.dumps(candidate))
                refused = self.resume(session_path)
                self.assertNotEqual(refused.returncode, 0)
                self.assertEqual(len(self.state["bodies"]), before_requests)

        duplicate = json.dumps(original_session).replace(
            '"activation": "active"',
            '"activation": "active", "activation": "active"',
            1,
        )
        session_path.write_text(duplicate)
        self.assertNotEqual(self.resume(session_path).returncode, 0)
        self.assertEqual(len(self.state["bodies"]), before_requests)

        job_mutations = {
            "wrong_parent": lambda value: value.__setitem__(
                "parent_session_id", "0" * 16
            ),
            "wrong_digest": lambda value: value.__setitem__(
                "swarm_definition_sha256", "0" * 64
            ),
            "wrong_capacity": lambda value: value["admission"].__setitem__("cap", 2),
            "wrong_count": lambda value: value.__setitem__("concurrency", 2),
            "wrong_ordered_member": lambda value: value["items"][0].__setitem__(
                "swarm_name", "tester"
            ),
            "wrong_coordinator_link": lambda value: value["items"][2].__setitem__(
                "swarm_coordinator_task", 0
            ),
        }
        for name, mutate in job_mutations.items():
            with self.subTest(name=name):
                session_path.write_text(json.dumps(original_session))
                candidate = copy.deepcopy(original_job)
                mutate(candidate)
                job_path.write_text(json.dumps(candidate))
                refused = self.resume(session_path)
                self.assertNotEqual(refused.returncode, 0)
                self.assertEqual(len(self.state["bodies"]), before_requests)

        session_path.write_text(json.dumps(original_session))
        digest = original_job["swarm_definition_sha256"]
        field = f'"swarm_definition_sha256": "{digest}"'
        job_path.write_text(
            json.dumps(original_job).replace(field, f"{field}, {field}", 1)
        )
        self.assertNotEqual(self.resume(session_path).returncode, 0)
        self.assertEqual(len(self.state["bodies"]), before_requests)
        job_path.write_text(json.dumps(original_job))

    def test_public_team_requests_cannot_forge_purposeful_metadata(self):
        base = {
            "kind": "ask",
            "dag": True,
            "items": [
                {"prompt": "lead", "role": "lead"},
                {"prompt": "one", "role": "worker"},
                {"prompt": "two", "role": "worker"},
            ],
        }
        forged = []
        root = copy.deepcopy(base)
        root["swarm_definition_sha256"] = "0" * 64
        forged.append(root)
        partial = copy.deepcopy(base)
        partial["items"][1]["swarm_name"] = "forged"
        forged.append(partial)
        complete = copy.deepcopy(base)
        complete.update(
            {
                "swarm_definition_sha256": "0" * 64,
                "swarm_root_coordinator": "forged-lead",
                "swarm_purpose": "forged-purpose",
            }
        )
        for index, item in enumerate(complete["items"]):
            item.update(
                {
                    "swarm_name": f"forged-{index}",
                    "swarm_role": "coordinator" if index == 0 else "agent",
                    "swarm_group": 0,
                    "swarm_purpose": "forged",
                    "swarm_group_purpose": "forged",
                    "swarm_coordinator_task": 0,
                    "swarm_parent_coordinator_task": -1,
                }
            )
        forged.append(complete)

        for request in forged:
            with self.subTest(request=request):
                refused = self.run_tny(
                    "team",
                    "start",
                    "--request",
                    "-",
                    stdin=json.dumps(request).encode(),
                    check=False,
                )
                self.assertNotEqual(refused.returncode, 0)
        self.assertEqual(self.state["bodies"], [])
        self.assertFalse(list((self.home / ".tny" / "jobs").glob("*/job.json")))


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary())
