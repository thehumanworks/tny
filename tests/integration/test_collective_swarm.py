#!/usr/bin/env python3
"""Collective mode public CLI/tools with synthetic localhost provider credentials."""

import fcntl
import json
import os
import pty
import select
import shlex
import signal
import struct
import subprocess
import termios
import threading
import time
import unittest
from pathlib import Path

from test_jobs import TNY, Handler, JobsFixture, argv_without_runner_binary
from test_subagent import chat_frames, tool_outputs, user_texts


def payload(text):
    return json.JSONDecoder().raw_decode(text[text.index("{") :])[0]


class CollectiveHandler(Handler):
    def do_POST(self):
        f = self.server.fixture
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        texts = user_texts(body, "chat")
        tag = next(t for t in texts if t.startswith("COLLECTIVE ")).split()[1]
        with f.lock:
            f.requests.setdefault(tag, []).append(body)
        self.enter(tag)
        try:
            call, answer = f.respond(tag, texts, tool_outputs(body, "chat"))
            self.reply(
                200,
                "text/event-stream",
                chat_frames(call=call) if call else chat_frames(text=answer),
            )
        except Exception as error:
            f.errors.append(repr(error))
            self.reply(500, "application/json", b'{"error":{"message":"fixture"}}')
        finally:
            self.leave()


class ModeAcceptance(JobsFixture):
    def policy(self):
        return "\n".join(
            m.get("content", "")
            for m in self.state["bodies"][-1].get("messages", [])
            if m.get("role") == "system"
        )

    def test_cli_forms_preserve_task_and_user_instructions(self):
        forms = [
            ("--swarm", "ask"),
            ("--swarm", "1", "ask"),
            ("--swarm=2", "ask"),
            ("ask", "--swarm"),
            ("ask", "--swarm", "1"),
            ("ask", "--swarm=2"),
        ]
        for form in forms:
            with self.subTest(form=form):
                before = len(self.state["bodies"])
                result = self.run_tny(
                    "--task",
                    "review",
                    "--system-prompt",
                    "USER-SYSTEM-SENTINEL",
                    *form,
                    "collective-mode-sentinel",
                    timeout=15,
                )
                self.assertEqual(len(self.state["bodies"]), before + 1)
                self.assertIn("Collective collaboration policy", self.policy())
                self.assertIn("USER-SYSTEM-SENTINEL", self.policy())
                self.assertIn(b"collective-mode-sentinel", result.stdout)
                sessions = list((self.home / ".tny").rglob("session.json"))
                self.assertTrue(
                    all(
                        json.loads(p.read_text())["task"]["name"] == "review"
                        for p in sessions
                    )
                )
        self.run_tny("ask", "ordinary-control")
        self.assertNotIn("Collective collaboration policy", self.policy())

    def test_bad_counts_do_not_contact_provider(self):
        for count in (
            "0",
            "-1",
            "+1",
            "1.2",
            "1x",
            "17",
            "999999999999999999999999999",
            "",
        ):
            for form in (("--swarm=" + count, "ask"), ("ask", "--swarm=" + count)):
                with self.subTest(form=form):
                    result = self.run_tny(
                        *form, "must-not-run", check=False, timeout=10
                    )
                    self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.state["bodies"], [])

    def test_ephemeral_refusal_precedes_provider(self):
        for args in (
            ("--ephemeral", "--swarm=2", "ask"),
            ("ask", "--swarm", "--ephemeral"),
        ):
            self.assertNotEqual(
                self.run_tny(*args, "must-not-run", check=False).returncode, 0
            )
        self.assertEqual(self.state["bodies"], [])

    def test_ask_local_swarm_refuses_ssh_before_connection(self):
        # A fake SSH command is evidence that no remote setup was attempted.
        bin_dir = self.home / "bin"
        bin_dir.mkdir()
        marker = self.home / "ssh-was-run"
        executable = bin_dir / "ssh"
        executable.write_text(
            "#!/bin/sh\ntouch " + shlex.quote(str(marker)) + "\nexit 99\n"
        )
        executable.chmod(0o700)
        env = dict(self.env, PATH=str(bin_dir) + os.pathsep + self.env["PATH"])
        result = self.run_tny(
            "--ssh",
            "fixture.invalid",
            "ask",
            "--swarm=1",
            "no effects",
            env=env,
            check=False,
            timeout=10,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"swarm requires", result.stderr)
        self.assertFalse(marker.exists())
        self.assertEqual(self.state["bodies"], [])

    def test_enable_existing_session_then_restore_stable_policy(self):
        self.run_tny("ask", "ordinary-first-turn")
        paths = list((self.home / ".tny").rglob("session.json"))
        self.assertEqual(len(paths), 1)
        sid = paths[0].parent.name
        self.run_tny(
            "--resume", sid, "--swarm=2", "ask", "collective-second-turn", timeout=15
        )
        policy = self.policy()
        self.run_tny("--resume", sid, "ask", "restored-third-turn", timeout=15)
        self.assertEqual(policy, self.policy())
        text = paths[0].read_text()
        for message in (
            "ordinary-first-turn",
            "collective-second-turn",
            "restored-third-turn",
        ):
            self.assertIn(message, text)
        self.assertEqual(json.loads(text)["swarm_cap"], 2)
        self.assertNotIn("Collective collaboration policy", text)

    def test_tui_idle_enable_rebind_and_new_cap(self):
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 45, 120, 0, 0))
        process = subprocess.Popen(
            [TNY, "--provider", "openai", "--no-color"],
            cwd=self.workspace,
            env=dict(self.env, TERM="xterm-256color", TNY_TOOLS="all"),
            stdin=slave,
            stdout=slave,
            stderr=slave,
            start_new_session=True,
        )
        os.close(slave)
        raw = bytearray()

        def until(predicate, seconds=15):
            end = time.monotonic() + seconds
            while not predicate():
                self.assertIsNone(process.poll(), bytes(raw[-2000:]))
                self.assertLess(time.monotonic(), end, bytes(raw[-2000:]))
                if select.select([master], [], [], 0.05)[0]:
                    raw.extend(os.read(master, 65536))

        try:
            until(lambda: b"/help for commands" in raw)
            os.write(master, b"ordinary-tui-before-swarm\r")
            until(lambda: b"answer:ordinary-tui-before-swarm" in raw)
            time.sleep(0.1)
            raw.clear()
            os.write(master, b"/swarm 2\r")
            until(lambda: b"collective swarm selected" in raw)
            os.write(master, b"collective-tui-after-swarm\r")
            until(lambda: b"answer:ordinary-tui-before-swarm collective-tui" in raw)
            self.assertIn("Collective collaboration policy", self.policy())
            time.sleep(0.1)
            raw.clear()
            os.write(master, b"/new\r")
            until(lambda: b"new session" in raw)
            raw.clear()
            os.write(master, b"/swarm 3\r")
            until(lambda: b"collective swarm selected" in raw)
            os.write(master, b"new-cap-three\r")
            until(lambda: b"answer:new-cap-three" in raw)
            self.assertIn("At most 3 collaborators", self.policy())
            time.sleep(0.1)
            os.write(master, b"/quit\r")
            end = time.monotonic() + 10
            while process.poll() is None and time.monotonic() < end:
                if select.select([master], [], [], 0.05)[0]:
                    try:
                        raw.extend(os.read(master, 65536))
                    except OSError:
                        pass
            self.assertEqual(process.wait(timeout=1), 0)
            saved = [
                json.loads(p.read_text())
                for p in (self.home / ".tny").rglob("session.json")
            ]
            self.assertEqual(sorted(s["swarm_cap"] for s in saved), [2, 3])
            old = next(s for s in saved if s["swarm_cap"] == 2)
            self.assertIn("ordinary-tui-before-swarm", json.dumps(old))
            self.assertIn("collective-tui-after-swarm", json.dumps(old))
            self.assertEqual(len(self.state["bodies"]), 3)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=3)
            os.close(master)


class CollectiveFlow(JobsFixture):
    def setUp(self):
        super().setUp()
        self.server.RequestHandlerClass = CollectiveHandler
        self.server.fixture = self
        self.lock = threading.Lock()
        self.requests, self.errors = {}, []
        self.run_id = None
        self.submitted = threading.Event()
        self.release = threading.Event()
        self.worker_started = threading.Event()
        self.profile = "all"
        self.scenario = "peers"
        self.env["PATH"] = (
            str(
                Path(
                    os.environ.get(
                        "TNY", str(Path(__file__).resolve().parents[2] / "build/tny")
                    )
                ).parent
            )
            + os.pathsep
            + self.env["PATH"]
        )

    def tearDown(self):
        self.release.set()
        super().tearDown()

    def call(self, name, tool, args):
        return (name, tool, json.dumps(args)), None

    def team(self, action, request, name=None):
        if self.profile == "all":
            return self.call(
                name or action, "team_control", {"action": action, "request": request}
            )
        path = self.home / ("team-" + (name or action) + ".json")
        path.write_text(json.dumps(request))
        return self.call(
            name or action,
            "terminal",
            {"command": "tny team " + action + " --request " + shlex.quote(str(path))},
        )

    def mail(self, name, action, **args):
        request = {"action": action, "run": self.run_id, **args}
        if self.profile == "all":
            return self.call(name, "team_mailbox", request)
        command = ["tny", "mailbox", action, "--run", self.run_id]
        for key, value in args.items():
            command += ["--" + key.replace("_", "-"), str(value)]
        return self.call(name, "terminal", {"command": shlex.join(command)})

    def request(self, count):
        return {
            "kind": "ask",
            "dag": True,
            "concurrency": count,
            "items": [
                {
                    "role": "worker",
                    "prompt": "COLLECTIVE " + str(i),
                    "workspace": {"policy": "shared_read_only"},
                }
                for i in range(count)
            ],
        }

    def respond(self, tag, texts, outputs):
        step = len(outputs)
        if tag != "lead":
            assert self.submitted.wait(10), "missing parent receipt"
            if self.scenario == "adopt":
                self.worker_started.set()
                assert self.release.wait(15), "old work was not released"
            if self.scenario != "peers":
                return None, "ONE-COLLABORATOR-DONE"
        if tag == "lead":
            if step == 0:
                request = self.request(1 if self.scenario == "one" else 2)
                if self.scenario == "bypass":
                    return self.call("bypass", "job_submit", request)
                if self.scenario == "subagent":
                    return self.call(
                        "bypass",
                        "subagent",
                        {"action": "create", "prompt": "must not run"},
                    )
                return self.team("start", request)
            if self.scenario in ("reject", "bypass", "subagent"):
                assert "error" in outputs[0].lower(), outputs
                return None, "CAP-REFUSED"
            if step == 1:
                first = payload(outputs[0])
                self.run_id = first.get("run_id") or first.get("id")
                assert self.run_id, first
                self.submitted.set()
            if self.scenario == "adopt":
                assert self.worker_started.wait(5), "old worker never ran"
                return None, "LEGACY-STARTED"
            if step <= (1 if self.scenario == "one" else 2):
                seen = [payload(o)["cursor"] for o in outputs[1:]]
                return self.team(
                    "wait-any",
                    {"id": self.run_id, "timeout_ms": 30000, "seen": seen},
                    "completion-" + str(step),
                )
            assert all(
                payload(o)["item"]["state"] == "succeeded" for o in outputs[1:]
            ), outputs
            return None, "VERIFIED-CONVERGENCE"
        if tag == "0":
            if outputs:
                assert payload(outputs[-1])["ok"], outputs[-1]
            if step == 0:
                return self.mail(
                    "proposal",
                    "publish",
                    id="proposal",
                    text=json.dumps(
                        {
                            "topic": "design",
                            "thread": "a",
                            "type": "proposal",
                            "body": "proposal-evidence" + "x" * 16000,
                        }
                    ),
                )
            if step == 1:
                assert len(outputs[0]) < 1024, "publication echoed fanout bodies"
                assert all("text" not in m for m in payload(outputs[0])["messages"])
                return self.mail("challenge-wait", "wait", timeout_ms=10000)
            if step == 2:
                assert "peer-counterexample" in outputs[-1], outputs
                return self.mail("challenge-ack", "ack", id="challenge")
            if step == 3:
                return self.mail(
                    "reply",
                    "send",
                    to=1,
                    id="reply",
                    text="counterexample-accepted; revised-evidence",
                )
            return None, "REVISED-DECISION"
        if tag == "1":
            if step == 0:
                return self.mail("proposal-wait", "wait", timeout_ms=10000)
            if step == 1:
                assert "proposal-evidence" in outputs[-1], outputs
                return self.mail("proposal-ack", "ack", id="proposal.p2")
            if step == 2:
                return self.mail(
                    "challenge",
                    "send",
                    to=0,
                    id="challenge",
                    text="peer-counterexample",
                )
            if step == 3:
                return self.mail("reply-wait", "wait", timeout_ms=10000)
            if step == 4:
                assert "counterexample-accepted" in outputs[-1], outputs
                return self.mail("reply-ack", "ack", id="reply")
            return None, "CHALLENGE-RESOLVED"
        raise AssertionError(tag)

    def exercise(self, scenario, profile="all"):
        self.scenario, self.profile = scenario, profile
        self.env["TNY_TOOLS"] = profile
        cap = 2 if scenario == "peers" else 1
        result = self.run_tny(
            "--swarm=" + str(cap), "ask", "COLLECTIVE lead", timeout=65, check=False
        )
        self.assertFalse(self.errors, self.errors)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        if scenario in ("reject", "bypass", "subagent"):
            self.assertEqual(self.job_dirs(), [])
            self.assertEqual(set(self.requests), {"lead"})
            self.assertIn(b"CAP-REFUSED", result.stdout)
            return
        self.assertIn(b"VERIFIED-CONVERGENCE", result.stdout)
        job = json.loads((self.jobs_root() / self.run_id / "job.json").read_text())
        self.assertEqual(job["admission"]["cap"], cap)
        self.assertTrue(job["peer_messages"])
        self.assertEqual(len(job["items"]), cap)
        for tag, bodies in self.requests.items():
            systems = [
                next(m["content"] for m in b["messages"] if m["role"] == "system")
                for b in bodies
            ]
            self.assertTrue(all(s == systems[0] for s in systems))
            self.assertIn("Collective collaboration policy", systems[0])
            if tag != "lead":
                self.assertIn("not a recursive orchestrator", systems[0])
        if scenario == "peers":
            self.assertEqual(
                {tag: len(v) for tag, v in self.requests.items()},
                {"lead": 4, "0": 5, "1": 6},
            )
            mailbox = json.loads(
                (self.jobs_root() / self.run_id / "mailbox.json").read_text()
            )["messages"]
            self.assertEqual(len(mailbox), 4)
            self.assertEqual(sum(m["publication"] == "proposal" for m in mailbox), 2)
            self.assertTrue(
                all(m["state"] == 2 for m in mailbox if m["recipient_task"] >= 0)
            )
            for p in (self.home / ".tny").rglob("session.json"):
                session = json.loads(p.read_text())
                receipts = session.get("team_receipts", [])
                self.assertEqual(len(receipts), len(set(receipts)))

    def test_enable_refuses_preexisting_active_owned_work(self):
        self.scenario = "adopt"
        self.env["TNY_TOOLS"] = "all"
        original = self.run_tny("ask", "COLLECTIVE lead", timeout=20)
        self.assertIn(b"LEGACY-STARTED", original.stdout)
        parents = [
            p
            for p in (self.home / ".tny").rglob("session.json")
            if self.run_id in json.loads(p.read_text()).get("team_runs", [])
        ]
        self.assertEqual(len(parents), 1)
        before = len(self.requests["lead"])
        refused = self.run_tny(
            "--swarm=1",
            "--resume",
            parents[0].parent.name,
            "ask",
            "must-not-run",
            check=False,
        )
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn(b"active or uncertain owned work", refused.stderr)
        self.assertEqual(len(self.requests["lead"]), before)
        self.assertEqual(json.loads(parents[0].read_text())["swarm_cap"], 0)
        self.release.set()
        self.assertEqual(self.await_terminal(self.run_id)["state"], "succeeded")
        for bodies in self.requests.values():
            self.assertNotIn("Collective collaboration policy", json.dumps(bodies))
        retry = self.run_tny("--swarm=1", "jobs", "retry", self.run_id, check=False)
        self.assertNotEqual(retry.returncode, 0)
        self.assertIn(b"legacy retry cannot bypass admission", retry.stderr)

    def test_typed_peer_challenge_convergence(self):
        self.exercise("peers")

    def test_terminal_peer_challenge_convergence(self):
        self.exercise("peers", "terminal")

    def test_one_collaborator_is_valid(self):
        self.exercise("one")
        self.assertEqual(self.await_terminal(self.run_id)["state"], "succeeded")
        refused = self.run_tny(
            "mailbox",
            "publish",
            "--run",
            self.run_id,
            "--id",
            "too-late",
            "--text",
            "late",
            check=False,
        )
        self.assertNotEqual(refused.returncode, 0)
        self.assertEqual(json.loads(refused.stdout)["error"], "MAILBOX_TERMINAL")
        observed = self.run_tny(
            "mailbox", "wait", "--run", self.run_id, "--timeout-ms", "0"
        )
        self.assertEqual(json.loads(observed.stdout)["error"], "MAILBOX_TERMINAL")

    def test_team_cap_rejection(self):
        self.exercise("reject")

    def test_direct_jobs_cannot_bypass_cap(self):
        self.exercise("bypass")

    def test_isolated_subagent_cannot_bypass_cap(self):
        self.exercise("subagent")


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary(), verbosity=2)
