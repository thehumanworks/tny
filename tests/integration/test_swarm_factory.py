#!/usr/bin/env python3
"""Version-2 contribution contracts over the real durable swarm/job runtime.

Every provider request goes to a loopback stdlib fixture under a throwaway HOME.
The tests use real detached workers and managed Git worktrees; no live credential,
provider, fabricated job record, or second scheduler is involved.
"""

from __future__ import annotations

import copy
import json
import subprocess
import threading
import unittest
from pathlib import Path

from test_jobs import Handler, JobsFixture, argv_without_runner_binary


def actor(
    name,
    purpose,
    deliverable,
    acceptance,
    *,
    depends_on=None,
    workspace=None,
):
    value = {
        "name": name,
        "purpose": purpose,
        "deliverable": deliverable,
        "acceptance": acceptance,
    }
    if depends_on is not None:
        value["depends_on"] = depends_on
    if workspace is not None:
        value["workspace"] = workspace
    return value


def factory_definition():
    """Forward coordinator dependency exercises name resolution after flattening."""
    return {
        "version": 2,
        "purpose": "Produce a bounded implementation with independent evidence.",
        "coordinator": actor(
            "root-lead",
            "Own integration and report uncertainty.",
            "A root decision that distinguishes execution from acceptance.",
            ["No isolated commit is called merged without explicit integration."],
        ),
        "agents": [
            actor(
                "design",
                "Research the smallest design while DESIGN-HOLD is active.",
                "A design record with the hardest assumption.",
                ["The design reuses the durable DAG."],
                workspace={"policy": "shared_read_only"},
            ),
            actor(
                "independent-peer",
                "Inspect unrelated risks without waiting for DESIGN-HOLD.",
                "An independent risk note.",
                ["The note identifies uncertainty."],
            ),
        ],
        "swarms": [
            {
                "purpose": "Implement and independently review the change.",
                "coordinator": actor(
                    "review-coordinator",
                    "Review the implementer's evidence and synthesize upward.",
                    "A review decision with missing evidence stated explicitly.",
                    ["Acceptance remains declarative and is not inferred."],
                    depends_on=["implementer"],
                ),
                "agents": [
                    actor(
                        "implementer",
                        "Use ENVDUMP to make one isolated committed edit.",
                        "A committed isolated edit with exact test evidence.",
                        [
                            "The primary checkout is unchanged.",
                            "The answer does not claim an implicit merge.",
                        ],
                        depends_on=["design"],
                        workspace={"policy": "isolated"},
                    )
                ],
                "swarms": [],
            }
        ],
    }


def failure_definition():
    return {
        "version": 2,
        "purpose": "Prove failed dependencies do not spend a consumer call.",
        "coordinator": actor(
            "root-lead",
            "Report the failed handoff.",
            "A failure report.",
            ["No blocked consumer is called successful."],
        ),
        "agents": [
            actor(
                "broken-source",
                "Produce evidence that the fixture will reject.",
                "A prerequisite result.",
                ["The provider succeeds."],
            ),
            actor(
                "blocked-consumer",
                "Consume only verified prerequisite evidence.",
                "A downstream result.",
                ["The prerequisite is verified."],
                depends_on=["broken-source"],
            ),
        ],
        "swarms": [],
    }


def message_text(body):
    return "\n".join(
        message.get("content", "")
        for message in body.get("messages", [])
        if isinstance(message.get("content"), str)
    )


def participant_of(body):
    text = message_text(body)
    for name in (
        "design",
        "independent-peer",
        "review-coordinator",
        "implementer",
        "broken-source",
        "blocked-consumer",
    ):
        if f"Participant: {name}\n" in text:
            return name
    return None


class FactoryHandler(Handler):
    """Deterministic answers plus a held predecessor and one real tool call."""

    answers = {
        "design": "DESIGN-EVIDENCE-ONLY: use the existing DAG.",
        "independent-peer": "INDEPENDENT-EVIDENCE: unrelated risk review.",
        "implementer": "IMPLEMENTATION-EVIDENCE-ONLY: isolated commit retained.",
        "review-coordinator": "REVIEW-EVIDENCE: acceptance is still unverified.",
        "blocked-consumer": "BLOCKED-CONSUMER-MUST-NOT-RUN",
    }

    def stream_text(self, value):
        frames = [
            {"choices": [{"index": 0, "delta": {"content": value}}]},
            {
                "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                "usage": {"prompt_tokens": 3, "completion_tokens": 1},
            },
        ]
        data = (
            "".join(f"data: {json.dumps(frame)}\n\n" for frame in frames)
            + "data: [DONE]\n\n"
        ).encode()
        self.reply(200, "text/event-stream", data)

    def chat(self, prompt, after_tool=False):
        name = None
        for candidate in self.answers | {"broken-source": ""}:
            if f"Name: {candidate}\n" in prompt:
                name = candidate
                break
        if not name:
            super().chat(prompt, after_tool)
            return

        state = self.server.state
        if not after_tool:
            with self.server.lock:
                state["participant_order"].append(name)
            state["participant_events"].setdefault(name, threading.Event()).set()

        if name == "design" and not after_tool:
            if not state["design_release"].wait(timeout=30):
                self.reply(500, "application/json", b"{}")
                return
        if name == "broken-source":
            self.reply(
                500,
                "application/json",
                b'{"error":{"message":"fixture prerequisite failure"}}',
            )
            return
        if name == "implementer" and not after_tool:
            super().chat(prompt, after_tool)
            return
        if (
            name == "implementer"
            and after_tool
            and state.get("fail_after_implementation")
        ):
            self.reply(
                500,
                "application/json",
                b'{"error":{"message":"post-edit fixture failure"}}',
            )
            return
        self.stream_text(self.answers[name])


class SwarmFactory(JobsFixture):
    hold = 0.05

    def setUp(self):
        super().setUp()
        self.server.RequestHandlerClass = FactoryHandler
        self.state["participant_order"] = []
        self.state["participant_events"] = {}
        self.state["design_release"] = threading.Event()
        self.addCleanup(self.state["design_release"].set)
        self.git("init", "-q")
        self.git("config", "user.name", "Factory Fixture")
        self.git("config", "user.email", "factory@example.invalid")
        (self.workspace / "base.txt").write_text("base\n")
        self.git("add", ".")
        self.git("commit", "-qm", "baseline")

    def git(self, *args):
        return subprocess.run(
            ["git", "-C", str(self.workspace), *args],
            env=self.env,
            capture_output=True,
            text=True,
            check=True,
            timeout=15,
        ).stdout.strip()

    def write_definition(self, value, name="factory.json", explicit_base=True):
        value = copy.deepcopy(value)

        # The definition itself is an untracked fixture file. Pin the clean
        # baseline explicitly; never silently copy uncommitted root files.
        def pin(group):
            for actor_value in [group["coordinator"], *group["agents"]]:
                workspace = actor_value.get("workspace", {})
                if explicit_base and workspace.get("policy") == "isolated":
                    workspace.setdefault("base", self.git("rev-parse", "HEAD"))
            for child in group["swarms"]:
                pin(child)

        pin(value)
        path = self.workspace / name
        path.write_text(json.dumps(value))
        return path

    def saved_session(self):
        found = []
        for path in (self.home / ".tny").rglob("session.json"):
            value = json.loads(path.read_text())
            if "swarm_definition" in value:
                found.append((path, value))
        self.assertEqual(len(found), 1)
        return found[0]

    def participant_bodies(self, name):
        return [body for body in self.state["bodies"] if participant_of(body) == name]

    def wait_for_participant(self, name, timeout=20):
        with self.server.lock:
            event = self.state["participant_events"].setdefault(name, threading.Event())
        self.assertTrue(event.wait(timeout), f"{name} did not reach the provider")

    def launch(self, value, task="FACTORY_ROOT_TASK"):
        path = self.write_definition(value)
        result = self.run_tny(
            "--swarm-file", str(path), "ask", task, timeout=30, check=False
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode()[-2000:])
        session_path, session = self.saved_session()
        return session_path, session, session["swarm_definition"]["run_id"]

    def test_dependency_order_evidence_isolation_and_contract_policy(self):
        self.state["envdump"] = (
            "printf 'isolated-change\\n' > factory.txt; "
            "git add factory.txt; git commit -qm factory-change"
        )
        _, _, run_id = self.launch(factory_definition())

        self.wait_for_participant("design")
        self.wait_for_participant("independent-peer")
        self.assertFalse(self.participant_bodies("implementer"))
        self.assertFalse(self.participant_bodies("review-coordinator"))
        with self.server.lock:
            first = list(self.state["participant_order"])
        self.assertCountEqual(first, ["design", "independent-peer"])

        self.state["design_release"].set()
        record = self.await_terminal(run_id)
        self.assertEqual(
            [item["state"] for item in record["items"]],
            ["succeeded", "succeeded", "succeeded", "succeeded"],
            (record, self.startup_diagnostics(record)),
        )
        with self.server.lock:
            order = list(self.state["participant_order"])
        self.assertLess(order.index("design"), order.index("implementer"))
        self.assertLess(order.index("implementer"), order.index("review-coordinator"))

        items = {item["swarm_name"]: item for item in record["items"]}
        implementer_text = message_text(self.participant_bodies("implementer")[0])
        self.assertIn("DESIGN-EVIDENCE-ONLY", implementer_text)
        review_text = message_text(self.participant_bodies("review-coordinator")[0])
        self.assertIn("IMPLEMENTATION-EVIDENCE-ONLY", review_text)
        self.assertNotIn("DESIGN-EVIDENCE-ONLY", review_text)
        review_lower = review_text.lower()
        for clause in (
            "direct predecessor",
            "untrusted",
            "attempt",
            "result_sha256",
            "log_sha256",
        ):
            self.assertIn(clause, review_lower)
        predecessor = items["implementer"]
        for value in (
            predecessor["swarm_name"],
            str(predecessor["index"]),
            str(predecessor["attempt"]),
            predecessor["session_id"],
            predecessor["result_sha256"],
            predecessor["log_sha256"],
            predecessor["workspace_cwd"],
            predecessor["workspace_base"],
            predecessor["workspace_branch"],
            predecessor["workspace_revision"],
        ):
            self.assertIn(value, review_text)

        self.assertEqual(items["implementer"]["depends_on"], [0])
        self.assertEqual(items["review-coordinator"]["depends_on"], [3])
        self.assertEqual(items["implementer"]["workspace_policy"], "isolated")
        isolated = Path(items["implementer"]["workspace_cwd"])
        self.assertEqual((isolated / "factory.txt").read_text(), "isolated-change\n")
        self.assertFalse((self.workspace / "factory.txt").exists())
        self.assertTrue(items["implementer"]["workspace_branch"])
        self.assertTrue(items["implementer"]["workspace_base"])
        self.assertEqual(items["implementer"]["workspace_inspection"], "recorded")

        initial = self.participant_bodies("implementer")[0]
        initial_text = message_text(initial)
        self.assertIn(
            "A committed isolated edit with exact test evidence.", initial_text
        )
        self.assertIn("The primary checkout is unchanged.", initial_text)
        self.assertIn(
            "Acceptance criteria (declarative, not automatically proven)",
            initial_text,
        )
        self.assertIn("Workspace capability: isolated", initial_text)
        self.assertIn(
            "terminal", {tool["function"]["name"] for tool in initial["tools"]}
        )

        peer = self.participant_bodies("independent-peer")[0]
        self.assertIn("Workspace capability: shared_writable", message_text(peer))
        self.assertTrue(
            {"terminal", "write_file", "edit_file"}
            <= {tool["function"]["name"] for tool in peer["tools"]}
        )

    def test_failed_predecessor_blocks_consumer_before_provider(self):
        _, _, run_id = self.launch(failure_definition(), "PREREQUISITE_ROOT_TASK")
        record = self.await_terminal(run_id)
        items = {item["swarm_name"]: item for item in record["items"]}
        self.assertEqual(items["broken-source"]["state"], "failed", record)
        self.assertEqual(items["blocked-consumer"]["state"], "failed", record)
        self.assertEqual(items["blocked-consumer"]["error_code"], "dependency_blocked")
        self.assertTrue(self.participant_bodies("broken-source"))
        self.assertFalse(self.participant_bodies("blocked-consumer"))

    def test_failed_isolated_preparation_blocks_downstream_without_inspection(self):
        path = self.write_definition(factory_definition(), explicit_base=False)
        self.state["design_release"].set()
        result = self.run_tny(
            "--swarm-file", str(path), "ask", "PREPARATION_ROOT", timeout=30
        )
        self.assertEqual(result.returncode, 0)
        _, session = self.saved_session()
        record = self.await_terminal(session["swarm_definition"]["run_id"], timeout=15)
        items = {item["swarm_name"]: item for item in record["items"]}
        self.assertEqual(items["implementer"]["error_code"], "preparation_failed")
        self.assertEqual(
            items["review-coordinator"]["error_code"], "dependency_blocked"
        )
        self.assertFalse(self.participant_bodies("implementer"))
        self.assertFalse(self.participant_bodies("review-coordinator"))

    def test_inherited_read_only_cannot_request_isolated(self):
        path = self.write_definition(factory_definition())
        env = dict(self.env, TNY_TEAM_READ_ONLY="1")
        result = self.run_tny(
            "--swarm-file",
            str(path),
            "ask",
            "MUST_NOT_POST",
            env=env,
            check=False,
            timeout=20,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"permission denied for team_start", result.stderr)
        self.assertEqual(self.state["bodies"], [])
        self.assertEqual(self.job_dirs(), [])

    def test_public_job_request_cannot_assert_compiler_swarm_identity(self):
        request = json.dumps(
            {
                "kind": "ask",
                "dag": True,
                "swarm_manifest_version": 2,
                "items": [
                    {
                        "role": "worker",
                        "label": "forged",
                        "prompt": "MUST_NOT_POST",
                        "swarm_name": "forged",
                    }
                ],
            }
        )
        run, _ = self.submit("batch", stdin=request.encode(), check=False)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn(b"compiler-owned", run.stderr)
        self.assertEqual(self.state["bodies"], [])
        self.assertEqual(self.job_dirs(), [])

    def test_tampered_snapshot_workspace_and_dependencies_refuse_resume(self):
        self.state["design_release"].set()
        session_path, session, run_id = self.launch(factory_definition())
        record = self.await_terminal(run_id)
        job_path = Path(record["metadata_path"])
        session_bytes = session_path.read_bytes()
        job_bytes = job_path.read_bytes()
        before_bodies = len(self.state["bodies"])
        before_jobs = len(self.job_dirs())
        session_id = session_path.parent.name

        def refused_resume():
            result = self.run_tny(
                "--resume",
                session_id,
                "ask",
                "MUST_NOT_POST",
                check=False,
                timeout=20,
            )
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertEqual(len(self.state["bodies"]), before_bodies)
            self.assertEqual(len(self.job_dirs()), before_jobs)

        changed = json.loads(session_bytes)
        changed["swarm_definition"]["snapshot"]["agents"][0]["deliverable"] = (
            "tampered snapshot"
        )
        session_path.write_text(json.dumps(changed))
        refused_resume()
        session_path.write_bytes(session_bytes)

        changed = json.loads(job_bytes)
        implementer = next(
            item for item in changed["items"] if item["swarm_name"] == "implementer"
        )
        implementer["workspace_policy"] = "shared_read_only"
        job_path.write_text(json.dumps(changed))
        refused_resume()
        job_path.write_bytes(job_bytes)

        changed = json.loads(job_bytes)
        coordinator = next(
            item
            for item in changed["items"]
            if item["swarm_name"] == "review-coordinator"
        )
        coordinator["depends_on"] = []
        job_path.write_text(json.dumps(changed))
        refused_resume()
        job_path.write_bytes(job_bytes)

    def test_maximum_fan_in_retains_references_and_explicit_omissions(self):
        names = [f"source-{i}" for i in range(15)]
        answers = {name: f"EVIDENCE-{name}:" + "x" * 2600 for name in names}
        answers["review-coordinator"] = "SYNTHESIS"
        self.server.RequestHandlerClass = type(
            "FanInHandler", (FactoryHandler,), {"answers": answers}
        )
        value = {
            "version": 2,
            "purpose": "Bounded fan-in",
            "coordinator": {"name": "root", "purpose": "Accept only checked work"},
            "agents": [
                {"name": name, "purpose": "Provide independent evidence"}
                for name in names
            ]
            + [
                {
                    "name": "review-coordinator",
                    "purpose": "Synthesize direct evidence",
                    "depends_on": names,
                }
            ],
            "swarms": [],
        }
        _, _, run_id = self.launch(value, "FANIN_ROOT")
        run = self.await_terminal(run_id, timeout=30)
        self.assertEqual(run["state"], "succeeded", run)
        prompt = message_text(self.participant_bodies("review-coordinator")[0])
        evidence = prompt.split("BEGIN UNTRUSTED DIRECT-DEPENDENCY EVIDENCE", 1)[
            1
        ].split("END UNTRUSTED DIRECT-DEPENDENCY EVIDENCE", 1)[0]
        self.assertLessEqual(len(evidence.encode()), 16384)
        references = json.JSONDecoder().raw_decode(
            evidence.split("Durable references:\n", 1)[1]
        )[0]
        self.assertEqual([r["name"] for r in references], names)
        omitted = json.loads(
            evidence.split(
                "Omitted summaries (task indices; use durable references): ", 1
            )[1].strip()
        )
        self.assertTrue(omitted)
        self.assertTrue(all(index in range(15) for index in omitted))
        summaries = json.JSONDecoder().raw_decode(
            evidence.split("Bounded final-answer summaries:\n", 1)[1]
        )[0]
        self.assertEqual(len(summaries) + len(omitted), 15)
        self.assertTrue(all(s["truncated"] for s in summaries))

    def test_completed_run_dynamic_corruption_is_refused_before_provider(self):
        self.state["design_release"].set()
        session_path, _, run_id = self.launch(factory_definition())
        record = self.await_terminal(run_id)
        path = Path(record["metadata_path"])
        original = path.read_text()
        before_requests = len(self.state["bodies"])
        cases = [
            (0, "result_sha256", "0" * 64),
            (0, "log_sha256", "0" * 64),
            (0, "session_id", "0" * 16),
            (0, "attempt", 2),
            (0, "dependency_sha256", "0" * 64),
            (3, "workspace_branch", "refs/heads/forged"),
            (3, "workspace_base", "0" * 40),
            (3, "workspace_origin", "/not-the-owner"),
            (3, "workspace_revision", "0" * 40),
            (3, "workspace_cwd", "/not-the-workspace"),
        ]
        for index, key, value in cases:
            with self.subTest(field=key):
                changed = json.loads(original)
                changed["items"][index][key] = value
                path.write_text(json.dumps(changed))
                refused = self.run_tny(
                    "--resume",
                    session_path.parent.name,
                    "ask",
                    "MUST_NOT_POST",
                    check=False,
                    timeout=20,
                )
                self.assertNotEqual(refused.returncode, 0)
                self.assertEqual(len(self.state["bodies"]), before_requests)
                path.write_text(original)
        # Duplicate keys already fail the recursive record uniqueness validator.
        path.write_text(
            original.replace('"attempt": 1', '"attempt": 1, "attempt": 1', 1)
        )
        refused = self.run_tny(
            "--resume",
            session_path.parent.name,
            "ask",
            "MUST_NOT_POST",
            check=False,
            timeout=20,
        )
        self.assertNotEqual(refused.returncode, 0)
        self.assertEqual(len(self.state["bodies"]), before_requests)
        path.write_text(original)

    def test_failed_isolated_workspace_provenance_is_checked_on_resume(self):
        self.state["design_release"].set()
        self.state["fail_after_implementation"] = True
        self.state["envdump"] = (
            "printf retained > factory.txt; git add factory.txt; git commit -qm retained"
        )
        session_path, _, run_id = self.launch(factory_definition())
        record = self.await_terminal(run_id, timeout=30)
        item = next(i for i in record["items"] if i["swarm_name"] == "implementer")
        self.assertEqual(item["state"], "failed")
        self.assertEqual(item["workspace_inspection"], "recorded")
        retained = Path(item["workspace_cwd"]) / "factory.txt"
        self.assertEqual(retained.read_text(), "retained")
        self.state["fail_after_implementation"] = False
        self.run_tny(
            "--resume", session_path.parent.name, "ask", "INSPECT_RETAINED", timeout=30
        )
        before = len(self.state["bodies"])
        retained.write_text("changed after failure")
        refused = self.run_tny(
            "--resume",
            session_path.parent.name,
            "ask",
            "MUST_NOT_POST",
            check=False,
            timeout=20,
        )
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn(b"workspace provenance changed", refused.stderr)
        self.assertEqual(len(self.state["bodies"]), before)

    def test_shared_writable_handoff_does_not_invent_clean_or_head_state(self):
        self.state["design_release"].set()
        value = factory_definition()
        value["swarms"][0]["agents"][0]["workspace"] = {"policy": "shared_writable"}
        self.write_definition(value)
        self.git("add", ".")
        self.git("commit", "-qm", "declaration")
        baseline = self.git("rev-parse", "HEAD")
        self.state["envdump"] = (
            "printf shared > factory.txt; git add factory.txt; "
            "git commit -qm shared; printf dirty >> base.txt"
        )
        _, _, run_id = self.launch(value)
        self.assertEqual(self.await_terminal(run_id)["state"], "succeeded")
        self.assertNotEqual(self.git("rev-parse", "HEAD"), baseline)
        self.assertTrue(self.git("status", "--porcelain"))
        context = message_text(self.participant_bodies("review-coordinator")[0])
        references = json.JSONDecoder().raw_decode(
            context.split("Durable references:\n", 1)[1]
        )[0]
        workspace = next(
            r["workspace"] for r in references if r["name"] == "implementer"
        )
        self.assertIsNone(workspace["head_revision"])
        self.assertIsNone(workspace["dirty"])
        self.assertEqual(workspace["baseline_revision"], baseline)

    def test_resume_adopts_without_duplicate_participant_launch(self):
        self.state["design_release"].set()
        session_path, session, run_id = self.launch(factory_definition(), "FIRST_ROOT")
        self.await_terminal(run_id)
        participant_calls = sum(
            participant_of(body) is not None for body in self.state["bodies"]
        )
        jobs = [path.name for path in self.job_dirs()]
        resumed = self.run_tny(
            "--resume",
            session_path.parent.name,
            "ask",
            "SECOND_ROOT",
            timeout=30,
            check=False,
        )
        self.assertEqual(resumed.returncode, 0, resumed.stderr.decode()[-2000:])
        self.assertEqual([path.name for path in self.job_dirs()], jobs)
        self.assertEqual(
            sum(participant_of(body) is not None for body in self.state["bodies"]),
            participant_calls,
        )
        _, saved = self.saved_session()
        self.assertEqual(saved["swarm_definition"]["run_id"], run_id)
        self.assertEqual(session["swarm_definition"]["run_id"], run_id)


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary())
