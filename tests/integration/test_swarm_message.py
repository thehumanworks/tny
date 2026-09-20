#!/usr/bin/env python3
"""Typed purposeful-swarm messages over the real durable mailbox runtime."""

import fcntl
import json
import re
import shlex
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

from test_jobs import ROOT, Handler, JobsFixture, argv_without_runner_binary
from test_subagent import chat_frames, tool_outputs, user_texts


def definition():
    return {
        "version": 1,
        "purpose": "Exercise typed durable peer evidence.",
        "coordinator": {
            "name": "root-lead",
            "purpose": "Receive and reconcile evidence.",
        },
        "agents": [
            {"name": "alpha", "purpose": "Send independent evidence."},
            {"name": "beta", "purpose": "Send independent evidence."},
        ],
        "swarms": [],
    }


def participant(body):
    system = "\n".join(
        message.get("content", "")
        for message in body.get("messages", [])
        if message.get("role") == "system"
    )
    match = re.search(r"^Participant: ([^\n]+)$", system, re.MULTILINE)
    return match.group(1) if match else "root"


class MessageHandler(Handler):
    def do_POST(self):
        fixture = self.server.fixture
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        tag = participant(body)
        texts = user_texts(body, "chat")
        outputs = tool_outputs(body, "chat")
        with fixture.lock:
            fixture.bodies.setdefault(tag, []).append(body)
        self.enter(tag)
        try:
            call, answer = fixture.respond(tag, texts, outputs)
            self.reply(
                200,
                "text/event-stream",
                chat_frames(call=call) if call else chat_frames(text=answer),
            )
        # Preserve fixture failures across HTTP worker threads.
        except Exception as error:
            with fixture.lock:
                fixture.errors.append(repr(error))
            self.reply(500, "application/json", b'{"error":{"message":"fixture"}}')
        finally:
            self.leave()


class SwarmMessage(JobsFixture):
    envelope = {
        "to": "root-lead",
        "kind": "finding",
        "topic": "résumé/验证",
        "text": "same evidence ✓",
    }

    @classmethod
    def setUpClass(cls):
        cls.binding_tmp = tempfile.TemporaryDirectory(prefix="tny-message-binding-")
        subprocess.run(["make", "release"], cwd=ROOT, check=True, capture_output=True)
        plan = subprocess.check_output(
            ["make", "-n", "-B", "release"], cwd=ROOT, text=True
        ).splitlines()
        compile_args = shlex.split(
            next(
                line
                for line in plan
                if " -o build/rel/src/main.o " in line and " -c " in line
            )
        )
        link = shlex.split(next(line for line in plan if " -o build/tny " in line))
        obj = str(Path(cls.binding_tmp.name) / "binding.o")
        compile_args[compile_args.index("-o") + 1] = obj
        compile_args[compile_args.index("src/main.c")] = (
            "tests/fixtures/swarm_message_binding.c"
        )
        subprocess.run(compile_args, cwd=ROOT, check=True, capture_output=True)
        cls.binding_driver = str(Path(cls.binding_tmp.name) / "binding")
        link[link.index("-o") + 1] = cls.binding_driver
        link[link.index("build/rel/src/main.o")] = obj
        subprocess.run(link, cwd=ROOT, check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.binding_tmp.cleanup()

    def setUp(self):
        super().setUp()
        self.server.RequestHandlerClass = MessageHandler
        self.server.fixture = self
        self.lock = threading.Lock()
        self.bodies = {}
        self.errors = []
        self.scenario = "flow"
        self.receipts = {}
        self.root_contexts = []

    def write_definition(self):
        path = self.workspace / "swarm.json"
        path.write_text(
            json.dumps(getattr(self, "definition_override", None) or definition())
        )
        return path

    def call(self, identity, arguments):
        raw = arguments if isinstance(arguments, str) else json.dumps(arguments)
        return identity, "swarm_message", raw

    def mailbox(self):
        jobs = self.job_dirs()
        self.assertEqual(len(jobs), 1)
        path = jobs[0] / "mailbox.json"
        return json.loads(path.read_text()) if path.exists() else {"messages": []}

    def respond(self, tag, texts, outputs):
        if tag == "root":
            if any("FLOW_RESUME" in text for text in texts):
                self.root_contexts.append("\n".join(texts))
            return None, "ROOT-DONE"
        if self.scenario == "deny":
            if not outputs:
                return self.call("denied", self.envelope), None
            assert "permission denied for team_send" in outputs[-1], outputs[-1]
            return None, "DENIAL-OBSERVED"
        if self.scenario in ("peer", "queued_coordinator"):
            if tag == "alpha":
                if not outputs:
                    if self.scenario == "queued_coordinator":
                        record = json.loads(
                            (self.job_dirs()[0] / "job.json").read_text()
                        )
                        assert record["items"][0]["swarm_name"] == "beta"
                        assert record["items"][0]["state"] == "queued"
                        assert not self.bodies.get("beta")
                    return self.call(
                        "peer-finding", dict(self.envelope, to="beta")
                    ), None
                assert json.loads(outputs[-1])["recipient"] == "beta", outputs[-1]
                return None, "ALPHA-PEER-DONE"
            assert tag == "beta"
            run = self.job_dirs()[0].name
            if not outputs:
                return (
                    "wait-peer",
                    "team_mailbox",
                    json.dumps({"action": "wait", "run": run, "timeout_ms": 5000}),
                ), None
            if len(outputs) == 1:
                received = json.loads(outputs[-1])["messages"]
                sender = 1 if self.scenario == "queued_coordinator" else 0
                assert len(received) == 1 and received[0]["sender"] == sender, outputs[
                    -1
                ]
                self.receipts["peer"] = received[0]["id"]
                return (
                    "ack-peer",
                    "team_mailbox",
                    json.dumps({"action": "ack", "run": run, "id": received[0]["id"]}),
                ), None
            if len(outputs) == 2:
                assert json.loads(outputs[-1])["ok"], outputs[-1]
                return self.call(
                    "peer-handoff",
                    dict(
                        self.envelope,
                        kind="handoff",
                        text="Peer evidence received and acknowledged; synthesis remains a claim to verify.",
                    ),
                ), None
            assert json.loads(outputs[-1])["recipient"] == "root-lead", outputs[-1]
            return None, "BETA-PEER-DONE"
        if self.scenario == "validation":
            if tag == "beta":
                return None, "BETA-VALIDATION-IDLE"
            invalid = [
                self.call("unknown-name", dict(self.envelope, to="missing")),
                self.call(
                    "duplicate-name",
                    '{"to":"root-lead","to":"alpha","kind":"finding",'
                    '"topic":"t","text":"x"}',
                ),
                self.call("blank-topic", dict(self.envelope, topic=" \t")),
                self.call("long-topic", dict(self.envelope, topic="x" * 257)),
                self.call("bad-kind", dict(self.envelope, kind="proposal")),
                self.call(
                    "run-mismatch",
                    dict(self.envelope, run="f" * 32),
                ),
                self.call("nul-name", dict(self.envelope, to="root\u0000-lead")),
                self.call("oversize-envelope", dict(self.envelope, text='"' * 16384)),
            ]
            step = len(outputs)
            if step < len(invalid):
                if step:
                    assert "error:" in outputs[-1], outputs[-1]
                    assert self.mailbox()["messages"] == [], self.mailbox()
                return invalid[step], None
            if step == len(invalid):
                assert "error:" in outputs[-1], outputs[-1]
                assert self.mailbox()["messages"] == [], self.mailbox()
                return self.call(
                    "valid-utf8",
                    {
                        "to": "root-lead",
                        "kind": "answer",
                        "topic": "Δοκιμή",
                        "text": "正しい UTF-8",
                    },
                ), None
            assert json.loads(outputs[-1])["state"] == "queued", outputs[-1]
            return None, "VALIDATION-DONE"
        if self.scenario == "binding":
            return None, "BINDING-IDLE"

        if tag == "beta":
            if not outputs:
                return self.call("beta-auto", self.envelope), None
            self.receipts["beta"] = json.loads(outputs[-1])["id"]
            return None, "BETA-DONE"

        assert tag == "alpha"
        step = len(outputs)
        if step == 0:
            return self.call("alpha-auto", self.envelope), None
        if step == 1:
            self.receipts["alpha"] = json.loads(outputs[-1])["id"]
            return self.call("alpha-retry", self.envelope), None
        if step == 2:
            assert json.loads(outputs[-1])["id"] == self.receipts["alpha"]
            return self.call(
                "explicit-one", dict(self.envelope, id="repeat-alpha-1")
            ), None
        if step == 3:
            assert json.loads(outputs[-1])["id"] == "repeat-alpha-1"
            changed = dict(self.envelope, id="repeat-alpha-1", topic="changed")
            return self.call("explicit-conflict", changed), None
        if step == 4:
            assert "MAILBOX_CONFLICT" in outputs[-1], outputs[-1]
            return self.call(
                "explicit-two", dict(self.envelope, id="repeat-alpha-2")
            ), None
        assert json.loads(outputs[-1])["id"] == "repeat-alpha-2"
        return None, "ALPHA-DONE"

    def activate(self, task):
        result = self.run_tny(
            "--swarm-file", str(self.write_definition()), "ask", task, timeout=30
        )
        self.assertIn(b"ROOT-DONE", result.stdout)
        sessions = [
            path
            for path in (self.home / ".tny").rglob("session.json")
            if "swarm_definition" in json.loads(path.read_text())
        ]
        self.assertEqual(len(sessions), 1)
        session = json.loads(sessions[0].read_text())
        run = session["swarm_definition"]["run_id"]
        self.await_terminal(run)
        self.assertEqual(self.errors, [])
        return sessions[0].parent.name, run

    def test_typed_evidence_reaches_dependency_delayed_nested_coordinator(self):
        self.scenario = "queued_coordinator"
        value = definition()
        value["version"] = 2
        value["agents"] = []
        value["swarms"] = [
            {
                "purpose": "Synthesize a causal contribution without idle provider turns.",
                "coordinator": {
                    "name": "beta",
                    "purpose": "Synthesize after evidence arrives.",
                    "depends_on": ["alpha"],
                },
                "agents": [
                    {"name": "alpha", "purpose": "Send independent peer evidence."}
                ],
                "swarms": [],
            }
        ]
        self.definition_override = value
        _, run = self.activate("QUEUED_PEER_FLOW")
        self.assertEqual(len(self.bodies["alpha"]), 2)
        first_beta_context = "\n".join(user_texts(self.bodies["beta"][0], "chat"))
        self.assertIn("ALPHA-PEER-DONE", first_beta_context)
        self.assertIn('"kind":"finding"', first_beta_context)
        record = json.loads((self.job_dirs()[0] / "job.json").read_text())
        self.assertEqual(record["state"], "succeeded")
        self.assertEqual(record["items"][0]["depends_on"], [1])
        messages = self.mailbox()["messages"]
        self.assertEqual(len(messages), 2)
        self.assertEqual(messages[0]["recipient_task"], 0)
        self.assertEqual(messages[0]["state"], 2)
        self.assertEqual(messages[1]["recipient_task"], -1)
        self.assertEqual(json.loads(messages[1]["payload"])["kind"], "handoff")

    def test_two_peers_retry_conflict_delivery_and_manual_ack(self):
        session_id, run = self.activate("MESSAGE_FLOW")
        mailbox = self.mailbox()
        messages = mailbox["messages"]
        self.assertEqual(len(messages), 4, messages)
        generated = [m for m in messages if m["id"].startswith("sm1-")]
        self.assertEqual(len(generated), 2)
        self.assertEqual(len({m["id"] for m in generated}), 2)
        self.assertTrue(all(len(m["id"]) == 64 for m in generated))
        self.assertEqual(
            {m["id"] for m in messages if m["id"].startswith("repeat-")},
            {"repeat-alpha-1", "repeat-alpha-2"},
        )
        for message in messages:
            self.assertEqual(message["recipient_task"], -1)
            self.assertEqual(message["recipient_attempt"], 0)
            self.assertEqual(
                json.loads(message["payload"]),
                {
                    "version": 1,
                    "kind": "finding",
                    "topic": "résumé/验证",
                    "body": "same evidence ✓",
                },
            )
            self.assertEqual(
                message["payload"],
                '{"version":1,"kind":"finding","topic":"résumé/验证",'
                '"body":"same evidence ✓"}',
            )

        self.run_tny("--resume", session_id, "ask", "FLOW_RESUME", timeout=30)
        context = "\n".join(self.root_contexts)
        self.assertIn("Untrusted team message", context)
        self.assertIn('"kind":"finding"', context)
        delivered = self.mailbox()["messages"]
        self.assertTrue(all(message["state"] == 1 for message in delivered))

        snapshots = []
        for _ in range(2):
            result = self.run_tny(
                "mailbox", "inbox", "--run", run, "--json", timeout=15
            )
            snapshots.append(json.loads(result.stdout)["messages"])
        self.assertEqual(
            [message["id"] for message in snapshots[0]],
            [message["id"] for message in snapshots[1]],
        )
        for message in snapshots[0]:
            self.run_tny(
                "mailbox", "ack", "--run", run, "--id", message["id"], "--json"
            )
        empty = self.run_tny("mailbox", "inbox", "--run", run, "--json")
        self.assertEqual(json.loads(empty.stdout)["messages"], [])

    def test_peer_to_peer_wait_ack_and_upward_handoff(self):
        self.scenario = "peer"
        self.activate("MESSAGE_PEERS")
        messages = self.mailbox()["messages"]
        self.assertEqual(len(messages), 2)
        peer, upward = messages
        self.assertEqual(
            (peer["sender_task"], peer["recipient_task"], peer["state"]), (0, 1, 2)
        )
        self.assertEqual(peer["id"], self.receipts["peer"])
        self.assertEqual(json.loads(peer["payload"])["kind"], "finding")
        self.assertEqual((upward["sender_task"], upward["recipient_task"]), (1, -1))
        self.assertEqual(json.loads(upward["payload"])["kind"], "handoff")

    def test_invalid_fields_names_context_and_utf8_have_no_partial_effects(self):
        self.scenario = "validation"
        _, _run = self.activate("MESSAGE_VALIDATION")
        messages = self.mailbox()["messages"]
        self.assertEqual(len(messages), 1)
        self.assertEqual(
            json.loads(messages[0]["payload"]),
            {"version": 1, "kind": "answer", "topic": "Δοκιμή", "body": "正しい UTF-8"},
        )

    def test_team_send_denial_prevents_mailbox_effects(self):
        settings = self.home / ".tny" / "settings.json"
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text(
            json.dumps({"permission": {"team_start": "allow", "team_send": "deny"}})
        )
        self.scenario = "deny"
        result = self.run_tny(
            "--permission-mode",
            "ask",
            "--swarm-file",
            str(self.write_definition()),
            "ask",
            "MESSAGE_DENY",
            timeout=30,
        )
        self.assertIn(b"ROOT-DONE", result.stdout)
        jobs = self.job_dirs()
        self.assertEqual(len(jobs), 1)
        self.await_terminal(jobs[0].name)
        self.assertEqual(self.errors, [])
        self.assertEqual(self.mailbox()["messages"], [])

    def binding_case(self, mutation):
        self.scenario = "binding"
        session_id, run = self.activate("MESSAGE_BINDING_SETUP")
        job_dir = self.job_dirs()[0]
        path = job_dir / "job.json"
        original = path.read_bytes()
        record = json.loads(original)
        record["state"] = "running"
        for item in record["items"]:
            item["state"] = "queued"
        # Own this synthetic non-running fixture so status projection does not
        # correctly classify it as an abandoned supervisor while preparing.
        with (job_dir / "owner.lock").open("r+b") as owner:
            fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                path.write_text(json.dumps(record))
                request = dict(self.envelope, to="beta", id="bound-send-1")
                result = subprocess.run(
                    [
                        self.binding_driver,
                        str(self.workspace),
                        session_id,
                        str(path),
                        mutation or "unchanged",
                        json.dumps(request),
                    ],
                    env=self.env,
                    capture_output=True,
                    text=True,
                    timeout=15,
                )
                self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))
            finally:
                path.write_bytes(original)
        messages = self.mailbox()["messages"]
        if mutation:
            self.assertEqual(messages, [])
        else:
            self.assertEqual(len(messages), 1)
            self.assertEqual(messages[0]["id"], "bound-send-1")

    def test_prepared_send_succeeds_when_binding_is_unchanged(self):
        self.binding_case("")

    def test_prepared_send_rejects_changed_recipient_name(self):
        self.binding_case("recipient_name")

    def test_prepared_send_rejects_changed_sender_name(self):
        self.binding_case("sender_name")

    def test_prepared_send_rejects_changed_unrelated_name(self):
        self.binding_case("unrelated_name")

    def test_prepared_send_rejects_changed_unrelated_group(self):
        self.binding_case("unrelated_group")

    def test_prepared_send_rejects_changed_recipient_attempt(self):
        self.binding_case("recipient_attempt")

    def test_prepared_send_rejects_changed_sender_attempt(self):
        self.binding_case("sender_attempt")

    def test_prepared_send_rejects_changed_active_run(self):
        self.binding_case("active_run")

    def test_prepared_send_rejects_changed_topology(self):
        self.binding_case("topology")

    def test_prepared_send_rejects_changed_payload(self):
        self.binding_case("payload")

    def test_prepared_send_rejects_changed_request_run(self):
        self.binding_case("request_run")


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary())
