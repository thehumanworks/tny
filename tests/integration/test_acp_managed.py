#!/usr/bin/env python3
"""Managed ACP workers through real jobs, admission, teams and MCP dispatch.

Only the external agent/provider is a fixture. Real detached supervisors,
session runners, adapter processes and MCP relays execute under a private HOME.
Protocol state is per-process, and credential observations record names only.
"""

from __future__ import annotations

import base64
import json
import os
import signal
import subprocess
import sys
import time
import unittest
from pathlib import Path

from test_acp_client import AGENT
from test_jobs import TNY, Handler, JobsFixture, argv_without_runner_binary
from test_subagent import chat_frames


class ManagedAcp(JobsFixture):
    def setUp(self):
        super().setUp()
        self.facts_dir = self.home / "acp-facts"
        self.facts_dir.mkdir()
        self.settings = self.home / ".tny/settings.json"
        self.settings.parent.mkdir(exist_ok=True)
        self.config = {
            "jobs": {
                "ask_env": [
                    "ACP_FIXTURE_STATE_DIR",
                    "ACP_FIXTURE_NAME",
                    "ACP_FIXTURE_VERSION",
                    "ACP_FIXTURE_MCP",
                    "ACP_FIXTURE_EFFORT",
                    "ACP_FIXTURE_SCENARIOS",
                    "ACP_FIXTURE_USAGE",
                ]
            },
            "acp": {
                "fixture": {"command": str(AGENT), "model": "selected-model"},
                "other": {"command": str(AGENT), "model": "selected-model"},
            },
        }
        self.settings.write_text(json.dumps(self.config))
        self.env = {
            key: value
            for key, value in self.env.items()
            if not key.startswith("ACP_FIXTURE_")
        }
        self.env.update(
            TNY_SETTINGS_PATH=str(self.settings),
            TNY_TOOLS="all",
            TNY_SELF_IMPROVE="0",
            TNY_ACP_BRIDGE_EXECUTABLE=TNY,
            TNY_ACP_RPC_TIMEOUT_MS="5000",
            ACP_FIXTURE_STATE_DIR=str(self.facts_dir),
            ACP_FIXTURE_NAME="@agentclientprotocol/claude-agent-acp",
            ACP_FIXTURE_VERSION="0.75.1",
            ACP_FIXTURE_MCP="1",
            ACP_FIXTURE_EFFORT="1",
        )
        for args in (
            ("init", "-q"),
            (
                "-c",
                "user.name=Fixture",
                "-c",
                "user.email=fixture@invalid",
                "commit",
                "-q",
                "--allow-empty",
                "-m",
                "baseline",
            ),
        ):
            subprocess.run(
                ["git", "-C", str(self.workspace), *args],
                check=True,
                capture_output=True,
            )

    def run_tny(self, *args, **kwargs):
        kwargs.setdefault("provider", "acp@fixture")
        kwargs.setdefault("timeout", 40)
        return super().run_tny(*args, **kwargs)

    def facts(self):
        return [json.loads(path.read_text()) for path in self.facts_dir.glob("*.json")]

    def tagged(self, tag):
        return [item for item in self.facts() if item.get("tag") == tag]

    def until(self, predicate, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if value := predicate():
                return value
            time.sleep(0.05)
        self.fail("bounded fixture observation timed out")

    def scenarios(self, value):
        self.env["ACP_FIXTURE_SCENARIOS"] = json.dumps(value)

    def batch(self, items, *, flags=(), provider="acp@fixture", check=True, **extra):
        request = {"kind": "ask", "dag": True, "concurrency": 3, "items": items}
        request.update(extra)
        run = self.run_tny(
            *flags,
            "jobs",
            "submit",
            "batch",
            "--json",
            stdin=json.dumps(request).encode(),
            check=check,
            provider=provider,
        )
        return json.loads(run.stdout) if check else run

    @staticmethod
    def item(tag, **extra):
        return {"prompt": "ACP-MANAGED:" + tag, **extra}

    @staticmethod
    def admission(cap=1, claims=16):
        return {
            "label": "managed",
            "provider_scope": "same_alias",
            "cap": cap,
            "queue_cap": 16,
            "claim_limit": claims,
        }

    def assert_success(self, job):
        final = self.await_terminal(job["id"], timeout=30)
        self.assertEqual(
            final["state"], "succeeded", (final, self.startup_diagnostics(final))
        )
        self.assertTrue(all(i["state"] == "succeeded" for i in final["items"]), final)
        return final

    def test_dag_wait_results_and_frozen_model_effort_constraints(self):
        job = self.batch(
            [self.item("first"), self.item("second", depends_on=[0])],
            flags=("--model", "selected-model", "--effort", "high", "--max-steps", "4"),
        )
        waiting = self.run_tny("jobs", "wait", job["id"], "--timeout", "30", "--json")
        self.assertEqual(json.loads(waiting.stdout)["state"], "succeeded")
        final = self.assert_success(job)
        first, second = self.tagged("first")[0], self.tagged("second")[0]
        self.assertGreaterEqual(
            second["prompt_started_at"], first["prompt_finished_at"]
        )
        for facts in (first, second):
            self.assertEqual(facts["model_at_prompt"], "selected-model")
            self.assertEqual(facts["selected_effort"], "high")
            options = facts["session_meta"]["claudeCode"]["options"]
            self.assertEqual(options["maxTurns"], 4)
            self.assertTrue(options["strictMcpConfig"])
            self.assertEqual(options["tools"], [])
            self.assertNotEqual(facts["process_cwd"], str(self.workspace))
            self.assertFalse(facts["credential_env_present"])
            self.assertFalse(facts["cleanup_receipt_env_present"])
        for item in final["items"]:
            self.assertIn("ACP-OK", Path(item["log_path"]).read_text())
            self.assertRegex(item["result_sha256"], r"^[0-9a-f]{64}$")
            self.assertRegex(item["session_id"], r"^[0-9a-f]{16}$")
            self.assertFalse(item["usage_known"])
            sessions = list(
                (self.home / ".tny/sessions").glob(
                    f"*/{item['session_id']}/session.json"
                )
            )
            self.assertEqual(len(sessions), 1)
            saved = json.loads(sessions[0].read_text())
            self.assertEqual(saved["turns"], 1)
            self.assertEqual(saved["messages"][-1]["role"], "assistant")
            self.assertIn("ACP-OK", saved["messages"][-1]["content"])

    def test_context_only_usage_remains_unknown_in_job_summary(self):
        self.env["ACP_FIXTURE_USAGE"] = json.dumps(
            [
                {"used": 100, "size": 200000},
                {
                    "used": 125,
                    "size": 1000000,
                    "cost": {"amount": 0.03, "currency": "USD"},
                },
            ]
        )
        final = self.assert_success(self.batch([self.item("usage-only")]))
        item = final["items"][0]
        self.assertFalse(item["usage_known"], item)
        self.assertIsNone(item["usage_input_tokens"])
        self.assertIsNone(item["usage_output_tokens"])
        self.assertEqual(final["usage"]["unknown_items"], 1)
        events = [
            json.loads(line) for line in Path(item["log_path"]).read_text().splitlines()
        ]
        usage = [event for event in events if event.get("type") == "usage"]
        self.assertTrue(usage, events)
        self.assertTrue(all(event.get("tokens_reported") is False for event in usage))

    def test_unknown_or_mismatched_child_rejected_before_session(self):
        for claim_count, version in enumerate(("unknown", "0.75.0"), 1):
            with self.subTest(version=version):
                self.env["ACP_FIXTURE_VERSION"] = version
                before = {item["pid"] for item in self.facts()}
                job = self.batch([self.item("never")], admission=self.admission())
                final = self.await_terminal(job["id"], timeout=20)
                self.assertEqual(final["state"], "failed", final)
                facts = [item for item in self.facts() if item["pid"] not in before]
                self.assertEqual(len(facts), 1)
                self.assertIn("initialize", facts[0])
                self.assertNotIn("new_cwd", facts[0])
                self.assertNotIn("load_requested", facts[0])
                self.assertNotIn("prompted", facts[0])
                self.assertEqual(final["items"][0]["admission_claims"], claim_count)
        before = len(self.facts())
        self.run_tny("jobs", "status", job["id"], "--json", check=False)
        self.run_tny("jobs", "--help")
        self.assertEqual(len(self.facts()), before, "inspection launched an adapter")

    def test_plain_jobs_require_child_authority_without_dag_or_admission(self):
        for batch in (False, True):
            for verified in (True, False):
                with self.subTest(batch=batch, verified=verified):
                    self.env["ACP_FIXTURE_VERSION"] = (
                        "0.75.1" if verified else "unknown"
                    )
                    previous = {f["pid"] for f in self.facts()}
                    if batch:
                        job = self.batch([self.item("plain")], dag=False)
                    else:
                        run = self.run_tny(
                            "jobs",
                            "submit",
                            "ask",
                            "--prompt",
                            "ACP-MANAGED:plain",
                            "--json",
                        )
                        job = json.loads(run.stdout)
                    final = self.await_terminal(job["id"], timeout=20)
                    self.assertFalse(final.get("dag", False))
                    self.assertFalse(final.get("admission"))
                    facts = [f for f in self.facts() if f["pid"] not in previous]
                    self.assertEqual(len(facts), 1)
                    self.assertIn("initialize", facts[0])
                    self.assertNotEqual(facts[0]["process_cwd"], str(self.workspace))
                    if verified:
                        self.assertEqual(final["state"], "succeeded", final)
                        self.assertEqual(final["cleanup"], "complete", final)
                        self.assertTrue(facts[0]["prompted"])
                        options = facts[0]["session_meta"]["claudeCode"]["options"]
                        self.assertTrue(options["strictMcpConfig"])
                        self.assertEqual(options["tools"], [])
                        receipt = Path(final["items"][0]["log_path"] + ".acp-cleanup")
                        self.assertEqual(receipt.read_text(), "complete\n")
                    else:
                        self.assertEqual(final["state"], "failed", final)
                        self.assertNotIn("new_cwd", facts[0])
                        self.assertNotIn("load_requested", facts[0])
                        self.assertNotIn("prompted", facts[0])

    def test_read_only_yolo_denies_write_and_terminal_but_allows_read(self):
        source = self.workspace / "read-source.txt"
        source.write_text("readonly-read-proof")
        self.scenarios(
            {
                "readonly": {
                    "calls": [
                        {"name": "read_file", "arguments": {"path": source.name}},
                        {
                            "name": "write_file",
                            "arguments": {"path": "forbidden.txt", "content": "no"},
                        },
                    ]
                },
                "readonlyshell": {
                    "calls": [
                        {
                            "name": "terminal",
                            "arguments": {"command": "printf no > forbidden-shell.txt"},
                        }
                    ]
                },
            }
        )
        job = self.batch(
            [
                self.item("readonly", workspace={"policy": "shared_read_only"}),
                self.item("readonlyshell", workspace={"policy": "shared_read_only"}),
            ],
            flags=("--permission-mode", "yolo"),
        )
        final = self.await_terminal(job["id"], timeout=30)
        self.assertEqual(final["state"], "failed", final)
        facts = self.tagged("readonly")[0]
        answers = facts["tool_results"]
        self.assertIn("readonly-read-proof", json.dumps(answers[0]))
        for answer in [answers[1], self.tagged("readonlyshell")[0]["tool_results"][0]]:
            self.assertTrue(answer.get("result", {}).get("isError"), answer)
        self.assertFalse((self.workspace / "forbidden.txt").exists())
        self.assertFalse((self.workspace / "forbidden-shell.txt").exists())
        for item in final["items"]:
            log = Path(item["log_path"]).read_text()
            self.assertIn('"tool_ok":false', log)
            events = [json.loads(line) for line in log.splitlines()]
            self.assertEqual(
                [e["stop_reason"] for e in events if e["type"] == "turn_end"], [2]
            )

    def test_native_lead_selects_named_acp_without_inheriting_http_credentials(self):
        request = {
            "kind": "ask",
            "dag": True,
            "items": [
                self.item(
                    "native_selected",
                    provider="acp:fixture",
                    model="selected-model",
                    effort="high",
                )
            ],
        }

        class NativeJobsLead(Handler):
            def chat(self, _prompt, has_tools):
                self.reply(
                    200,
                    "text/event-stream",
                    chat_frames(text="NATIVE-LEAD-DONE")
                    if has_tools
                    else chat_frames(
                        call=("choose-reviewer", "job_submit", json.dumps(request)),
                    ),
                )

        self.server.RequestHandlerClass = NativeJobsLead
        lead = self.run_tny(
            "ask", "--json", "Select the ACP reviewer", provider="openai"
        )
        self.assertIn("NATIVE-LEAD-DONE", json.loads(lead.stdout)["output"])
        jobs = [self.record_at(path) for path in self.job_dirs()]
        self.assertEqual(len(jobs), 1)
        job = jobs[0]
        self.assertTrue(job["parent_session_id"])
        self.assert_success(job)
        facts = self.tagged("native_selected")[0]
        self.assertEqual(facts["model_at_prompt"], "selected-model")
        self.assertEqual(facts["selected_effort"], "high")
        self.assertFalse(facts["credential_env_present"])
        self.assertEqual(
            len(self.state["requests"]), 2, "only the fixture lead uses HTTP"
        )

    def test_explicit_own_profile_admission_and_mixed_scope_refusal(self):
        job = self.batch(
            [self.item("self", provider="acp:fixture")],
            admission=self.admission(),
        )
        self.assert_success(job)
        before = len(self.facts())
        refused = self.batch(
            [self.item("wrong", provider="acp:other")],
            admission=self.admission(),
            check=False,
        )
        self.assertNotEqual(refused.returncode, 0, refused.stdout)
        self.assertEqual(len(self.facts()), before)
        refused = self.batch(
            [self.item("wrongnative", provider="acp:fixture")],
            provider="openai",
            admission=self.admission(),
            check=False,
        )
        self.assertNotEqual(refused.returncode, 0, refused.stdout)
        self.assertEqual(len(self.facts()), before)

    def test_retry_frozen_named_argv_and_changed_config_rejection(self):
        fail = self.home / "fail"
        fail.touch()
        self.scenarios({"retry": {"fail_while": str(fail)}})
        job = self.batch(
            [self.item("retry")], flags=("--effort", "high"), admission=self.admission()
        )
        failed = self.await_terminal(job["id"], timeout=20)
        self.assertEqual(failed["state"], "failed", failed)
        self.config["acp"]["fixture"]["command"] = str(AGENT) + " changed-command"
        self.settings.write_text(json.dumps(self.config))
        refused = self.run_tny(
            "--effort", "high", "jobs", "retry", job["id"], "--json", check=False
        )
        self.assertNotEqual(refused.returncode, 0, refused.stdout)
        self.assertEqual(len(self.tagged("retry")), 1)
        self.config["acp"]["fixture"]["command"] = str(AGENT)
        self.settings.write_text(json.dumps(self.config))
        fail.unlink()
        self.run_tny("--effort", "high", "jobs", "retry", job["id"], "--json")
        final = self.assert_success(job)
        self.assertEqual(final["items"][0]["admission_claims"], 2)
        facts = self.tagged("retry")
        self.assertEqual(len(facts), 2)
        self.assertEqual([f["argv"] for f in facts], [[], []])
        self.assertTrue(all(f["selected_effort"] == "high" for f in facts))

    def test_queued_named_command_uses_frozen_argv_after_profile_changes(self):
        self.config["acp"]["fixture"]["args"] = ["frozen literal"]
        self.settings.write_text(json.dumps(self.config))
        release = self.home / "frozen-release"
        self.scenarios({"first_frozen": {"release": str(release)}})
        first = self.batch([self.item("first_frozen")], admission=self.admission())
        self.until(lambda: self.tagged("first_frozen"))
        second = self.batch([self.item("second_frozen")], admission=self.admission())
        self.await_state(
            second["id"],
            lambda r: r["items"][0].get("admission_reason") == "queued_capacity",
            timeout=10,
        )
        self.config["acp"]["fixture"]["command"] = "/bin/false"
        self.settings.write_text(json.dumps(self.config))
        release.touch()
        self.assert_success(first)
        self.assert_success(second)
        self.assertEqual(self.tagged("second_frozen")[0]["argv"], ["frozen literal"])

    def test_explicit_limit_rejects_generic_before_session_and_reaches_verified_resume(
        self,
    ):
        generic_env = dict(
            self.env, ACP_FIXTURE_NAME="fixture", ACP_FIXTURE_VERSION="1"
        )
        refused = self.run_tny(
            "--max-steps",
            "2",
            "ask",
            "--json",
            "ACP-MANAGED:genericlimit",
            env=generic_env,
            check=False,
        )
        self.assertNotEqual(refused.returncode, 0, refused.stdout)
        self.assertIn("max-steps", refused.stderr.decode())
        generic = self.facts()[0]
        self.assertIn("initialize", generic)
        self.assertNotIn("new_cwd", generic)
        self.assertNotIn("prompted", generic)
        first = self.run_tny(
            "--max-steps", "2", "ask", "--json", "ACP-MANAGED:limitnew"
        )
        self.assertIsNone(json.loads(first.stdout)["steps"])
        session = json.loads(first.stdout)["session_id"]
        self.run_tny(
            "--max-steps",
            "2",
            "--resume",
            session,
            "ask",
            "--json",
            "ACP-MANAGED:limitload",
        )
        for tag in ("limitnew", "limitload"):
            facts = self.tagged(tag)[0]
            options = facts["session_meta"]["claudeCode"]["options"]
            self.assertEqual(options["maxTurns"], 2)
            self.assertTrue(options["strictMcpConfig"])
        self.assertIn("load_requested", self.tagged("limitload")[0])

    def test_guarded_relative_executable_rejected_before_spawn(self):
        (self.workspace / "fixture-agent").symlink_to(AGENT)
        refused = self.run_tny(
            "--agent",
            "./fixture-agent",
            "ask",
            "--json",
            "must not initialize",
            provider="acp",
            env=dict(self.env, TNY_ACP_REQUIRE_TOOLS_AUTHORITY="1"),
            check=False,
        )
        self.assertNotEqual(refused.returncode, 0, refused.stdout)
        self.assertIn("absolute", refused.stderr.decode().lower())
        self.assertEqual(
            self.facts(), [], "relative executable started inside scratch cwd"
        )

    def test_equivalent_subagent_profile_keeps_frozen_command_model_and_effort(self):
        self.config["acp"]["fixture"].update(
            args=["--", "frozen literal"], model="default-model", effort="low"
        )
        self.settings.write_text(json.dumps(self.config))
        changed = json.loads(json.dumps(self.config))
        changed["acp"]["fixture"]["args"] = ["--", "changed literal"]
        self.scenarios(
            {
                "subparent": {
                    "calls": [
                        {
                            "name": "write_file",
                            "arguments": {
                                "path": str(self.settings),
                                "content": json.dumps(changed),
                            },
                        },
                        {
                            "name": "subagent",
                            "arguments": {
                                "action": "create",
                                "provider": "acp:fixture",
                                "prompt": "ACP-MANAGED:subchild",
                            },
                        },
                    ]
                }
            }
        )
        self.run_tny(
            "--model",
            "selected-model",
            "--effort",
            "high",
            "ask",
            "--json",
            "ACP-MANAGED:subparent",
        )
        parent = self.tagged("subparent")[0]
        self.assertIn("ACP-OK", json.dumps(parent["tool_results"][1]))
        child = self.tagged("subchild")[0]
        self.assertEqual(child["argv"], ["--", "frozen literal"])
        self.assertEqual(child["model_at_prompt"], "selected-model")
        self.assertEqual(child["selected_effort"], "high")

    def test_managed_python_command_preserves_literal_double_dash(self):
        self.config["acp"]["fixture"].update(
            command=sys.executable,
            args=["--", str(AGENT), "counted sentinel"],
        )
        self.settings.write_text(json.dumps(self.config))
        job = self.batch([self.item("counted")])
        self.assert_success(job)
        self.assertEqual(self.tagged("counted")[0]["argv"], ["counted sentinel"])

    def test_independent_batches_share_cap_fifo_and_survive_submitter_exit(self):
        release = self.home / "release"
        self.scenarios({"held": {"release": str(release)}})
        first = self.batch([self.item("held")], admission=self.admission())
        self.until(lambda: self.tagged("held"))
        second = self.batch([self.item("second")], admission=self.admission())
        third = self.batch([self.item("third")], admission=self.admission())
        for job in (second, third):
            queued = self.await_state(
                job["id"],
                lambda r: (
                    r["items"][0].get("admission_reason")
                    in ("queued_capacity", "queued_fifo")
                ),
                timeout=10,
            )
            self.assertNotEqual(queued["state"], "succeeded")
        self.assertEqual(len(self.facts()), 1)
        release.touch()
        for job in (first, second, third):
            final = self.assert_success(job)
            self.assertEqual(final["items"][0]["admission_reason"], "released")
        times = [self.tagged(tag)[0] for tag in ("held", "second", "third")]
        self.assertLessEqual(
            times[0]["prompt_finished_at"], times[1]["prompt_started_at"]
        )
        self.assertLessEqual(
            times[1]["prompt_finished_at"], times[2]["prompt_started_at"]
        )

    def test_collective_actual_mcp_team_start_and_completion(self):
        self.scenarios(
            {
                "lead": {
                    "calls": [
                        {
                            "name": "team_control",
                            "arguments": {
                                "action": "start",
                                "request": {
                                    "kind": "ask",
                                    "dag": True,
                                    "concurrency": 1,
                                    "items": [
                                        self.item(
                                            "collective",
                                            role="worker",
                                            provider="acp:fixture",
                                            workspace={"policy": "shared_read_only"},
                                        )
                                    ],
                                },
                            },
                        },
                        {
                            "name": "team_control",
                            "arguments": {
                                "action": "wait-any",
                                "request": {
                                    "id": "$previous_id",
                                    "timeout_ms": 10000,
                                    "seen": [],
                                },
                            },
                        },
                        {
                            "name": "team_control",
                            "arguments": {
                                "action": "collect",
                                "request": {
                                    "id": "$first_id",
                                    "item": 0,
                                    "max_bytes": 4096,
                                },
                            },
                        },
                    ]
                }
            }
        )
        self.run_tny("--swarm=1", "ask", "--json", "ACP-MANAGED:lead")
        facts = self.tagged("lead")[0]
        self.assertEqual(len(facts["tool_results"]), 3)
        self.assertIn("succeeded", json.dumps(facts["tool_results"][1]))
        collected = json.loads(facts["tool_results"][2]["result"]["content"][0]["text"])
        self.assertEqual(collected["result_integrity"], "matched")
        self.assertIn(b"ACP-OK", base64.b64decode(collected["result_base64"]))
        self.assertEqual(len(self.tagged("collective")), 1)
        jobs = [self.record_at(path) for path in self.job_dirs()]
        self.assertEqual(len(jobs), 1)
        self.assert_success(jobs[0])

    def write_manifest(self):
        definition = self.home / "swarm.json"
        definition.write_text(
            json.dumps(
                {
                    "version": 2,
                    "purpose": "Execute ACP fixture workers.",
                    "coordinator": {"name": "lead", "purpose": "Collect evidence."},
                    "agents": [
                        {"name": "writer", "purpose": "ACP-MANAGED:manifestwriter"},
                        {
                            "name": "reviewer",
                            "purpose": "ACP-MANAGED:manifestreviewer",
                            "workspace": {"policy": "shared_read_only"},
                        },
                    ],
                    "swarms": [],
                }
            )
        )
        return definition

    def test_manifest_swarm_launches_real_managed_workers(self):
        definition = self.write_manifest()
        run = self.run_tny(
            "--swarm-file", str(definition), "ask", "--json", "manifest-root"
        )
        jobs = [self.record_at(path) for path in self.job_dirs()]
        self.assertEqual(len(jobs), 1)
        final = self.assert_success(jobs[0])
        self.assertEqual(len(final["items"]), 2)
        self.assertEqual(final["admission"]["cap"], 2)
        workers = [
            f for f in self.facts() if f.get("process_cwd") != str(self.workspace)
        ]
        self.assertCountEqual(
            [f.get("tag") for f in workers], ["manifestwriter", "manifestreviewer"]
        )
        session_id = json.loads(run.stdout)["session_id"]
        path = next((self.home / ".tny/sessions").glob(f"*/{session_id}/session.json"))
        self.assertEqual(json.loads(path.read_text())["acp_pending_context"], [])
        context = "Purposeful swarm activated as durable run " + final["id"]
        self.assertTrue(
            any(context in json.dumps(f.get("prompt", [])) for f in self.facts())
        )

    def test_failed_manifest_prompt_retains_coordination_context_then_resume_acknowledges(
        self,
    ):
        definition = self.write_manifest()
        failed = self.run_tny(
            "--swarm-file",
            str(definition),
            "ask",
            "--json",
            "activation-fails",
            env=dict(self.env, ACP_FIXTURE_MODE="prompt-error"),
            check=False,
        )
        self.assertNotEqual(failed.returncode, 0, failed.stdout)
        sessions = [
            p
            for p in self.home.rglob("session.json")
            if "swarm_definition" in json.loads(p.read_text())
        ]
        self.assertEqual(len(sessions), 1)
        path = sessions[0]
        saved = json.loads(path.read_text())
        pending = saved["acp_pending_context"]
        self.assertTrue(pending)
        job_id = saved["swarm_definition"]["run_id"]
        self.assert_success({"id": job_id})
        previous = {f["pid"] for f in self.facts()}
        self.run_tny("--resume", saved["id"], "ask", "--json", "activation-retry")
        self.assertEqual(json.loads(path.read_text())["acp_pending_context"], [])
        self.assertEqual(len(self.job_dirs()), 1, "resume launched a duplicate swarm")
        resumed = [f for f in self.facts() if f["pid"] not in previous]
        self.assertEqual(len(resumed), 1)
        prompt = json.dumps(resumed[0]["prompt"])
        self.assertIn("Purposeful swarm activated as durable run " + job_id, prompt)

    def test_cancellation_owns_adapter_mcp_and_descendant_before_release(self):
        release = self.home / "never-release"
        self.scenarios({"cancel": {"release": str(release), "descendant": True}})
        job = self.batch([self.item("cancel")], admission=self.admission())
        facts = self.until(
            lambda: next(
                (f for f in self.tagged("cancel") if f.get("descendant_pid")), None
            )
        )
        self.run_tny("jobs", "cancel", job["id"], "--expected-attempt", "1", "--json")
        final = self.await_terminal(job["id"], timeout=20)
        self.assertEqual(final["state"], "cancelled", final)
        for pid in (facts["pid"], facts["mcp_pid"], facts["descendant_pid"]):

            def gone():
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    return True
                return False

            self.until(gone, timeout=5)
        self.assertEqual(final["items"][0]["admission_reason"], "released")

    def test_owner_loss_retains_uncertain_cleanup_admission_hold(self):
        release = self.home / "owner-release"
        self.scenarios({"owner": {"release": str(release)}})
        first = self.batch([self.item("owner")], admission=self.admission())
        facts = self.until(lambda: next(iter(self.tagged("owner")), None))
        second = self.batch([self.item("never")], admission=self.admission())
        self.await_state(
            second["id"],
            lambda r: r["items"][0].get("admission_reason") == "queued_capacity",
            timeout=10,
        )
        self.run_tny(
            "jobs", "cancel", second["id"], "--expected-attempt", "1", "--json"
        )
        self.await_terminal(second["id"], timeout=10)
        try:
            os.kill(self.worker_pid(first["id"]), signal.SIGKILL)
            final = self.await_terminal(first["id"], timeout=15)
            self.assertEqual(final["cleanup"], "unknown", final)
            ledger = json.loads(
                (self.home / ".tny/admission/managed/same_alias/state.json").read_text()
            )
            claim = next(
                entry for entry in ledger["entries"] if entry["run"] == first["id"]
            )
            self.assertEqual(claim["state"], 2)
            refused = self.run_tny("jobs", "retry", first["id"], "--json", check=False)
            self.assertNotEqual(refused.returncode, 0, refused.stdout)
            self.assertFalse(self.tagged("never"))
        finally:
            release.touch()

            def stopped():
                try:
                    os.kill(facts["pid"], 0)
                except ProcessLookupError:
                    return True
                return False

            self.until(stopped, timeout=10)


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary(), verbosity=2)
