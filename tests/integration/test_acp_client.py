#!/usr/bin/env python3
"""ACP client integration: real subprocess transport and owning MCP tool runtime.

All tests are deterministic local fixtures, including an HTTP request capture
which compares complete native tool schemas to MCP tools/list. No live provider.
"""

from __future__ import annotations

import base64
import http.server
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TNY = Path(os.environ.get("TNY", str(ROOT / "build/tny"))).resolve()
AGENT = ROOT / "tests/integration/fake_acp_agent.py"


def clean_env(home: Path) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("ACP_FIXTURE_", "OPENAI_", "CODEX_", "TNY_"))
    }
    env.update(
        HOME=str(home),
        TNY_SETTINGS_PATH=str(home / ".tny/settings.json"),
        TNY_TOOLS="all",
        TNY_SELF_IMPROVE="0",
        TNY_ACP_BRIDGE_EXECUTABLE=str(TNY),
        TNY_ACP_RPC_TIMEOUT_MS="3000",
        ACP_FIXTURE_NAME="@agentclientprotocol/claude-agent-acp",
        ACP_FIXTURE_VERSION="0.75.1",
    )
    return env


class NativeCapture(http.server.BaseHTTPRequestHandler):
    """Capture a chat-wire schema and return a valid no-tool completion."""

    def log_message(self, *_args):
        pass

    def do_POST(self):
        self.server.captured = json.loads(
            self.rfile.read(int(self.headers["Content-Length"]))
        )
        chunks = [
            {
                "choices": [
                    {"index": 0, "delta": {"role": "assistant", "content": "NATIVE-OK"}}
                ]
            },
            {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]},
        ]
        payload = "".join("data: " + json.dumps(chunk) + "\n\n" for chunk in chunks)
        payload += "data: [DONE]\n\n"
        raw = payload.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


class AcpClientTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tny-acp-client-")
        self.root = Path(self.temporary.name)
        self.home = self.root / "home"
        self.workspace = self.root / "workspace"
        self.workspace.mkdir()
        (self.home / ".tny").mkdir(parents=True)
        (self.home / ".tny/settings.json").write_text("{}")
        self.state = self.root / "state.json"
        self.env = clean_env(self.home)
        self.env["ACP_FIXTURE_STATE"] = str(self.state)

    def tearDown(self):
        self.temporary.cleanup()

    def state_json(self):
        return json.loads(self.state.read_text()) if self.state.exists() else {}

    def command(self, *flags, prompt="fixture"):
        return [
            str(TNY),
            "--cwd",
            str(self.workspace),
            "--provider",
            "acp",
            "--agent",
            str(AGENT),
            *flags,
            "ask",
            "--json",
            prompt,
        ]

    def ask(self, *flags, mode="normal", env=None, prompt="fixture", success=True):
        result = subprocess.run(
            self.command(*flags, prompt=prompt),
            env=dict(self.env, ACP_FIXTURE_MODE=mode, **(env or {})),
            cwd=self.workspace,
            text=True,
            capture_output=True,
            timeout=20,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            output = json.loads(result.stdout)
            self.assertIn("ACP-OK café 🐕", output["output"])
            return output
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_lazy_help_version_and_literal_argv(self):
        for flags in (("--version",), ("ask", "--help")):
            result = subprocess.run(
                [str(TNY), "--provider", "acp", "--agent", str(AGENT), *flags],
                env=self.env,
                capture_output=True,
                text=True,
                timeout=5,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(self.state.exists(), "help/version spawned the agent")
        marker = self.workspace / "must-not-exist"
        literal = f"$(touch {marker}); `touch {marker}`"
        result = subprocess.run(
            [
                str(TNY),
                "--cwd",
                str(self.workspace),
                "--provider",
                "acp",
                "--agent",
                str(AGENT),
                "--",
                literal,
                "argument with spaces",
                "",
                "--",
                "ask",
                "--json",
                "fixture",
            ],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.state_json()["argv"], [literal, "argument with spaces", ""]
        )
        self.assertFalse(marker.exists(), "agent arguments were evaluated by a shell")

    def test_turn_with_closed_stdin(self):
        result = subprocess.run(
            self.command("--ephemeral"),
            env=self.env,
            cwd=self.workspace,
            preexec_fn=lambda: os.close(0),
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn("ACP-OK café 🐕", json.loads(result.stdout)["output"])
        self.assertTrue(self.state_json().get("prompted"))

    def test_json_runner_model_and_resume(self):
        first = self.ask("--model", "selected-model")
        self.assertEqual(first["provider"], "acp")
        facts = self.state_json()
        self.assertEqual(facts["initialize"]["protocolVersion"], 1)
        self.assertNotEqual(facts["new_cwd"], str(self.workspace.resolve()))
        self.assertEqual(facts["model_at_prompt"], "selected-model")
        self.assertEqual(facts["mcp_in_new"], 1)
        self.assertTrue(first["session_id"])
        self.ask(
            "--model", "selected-model", "--resume", first["session_id"], prompt="again"
        )
        facts = self.state_json()
        self.assertEqual(facts["load_requested"], "fixture-session-1")
        self.assertEqual(facts["model_at_prompt"], "selected-model")
        self.assertEqual(facts["mcp_in_load"], 1)

    def test_load_capability_fails_closed(self):
        first = self.ask(mode="no-load")
        failure = self.ask(
            "--resume", first["session_id"], mode="no-load", success=False
        )
        self.assertIn("load", failure.stderr.lower())
        self.assertNotIn("load_requested", self.state_json())

    def test_model_config_grouped_and_legacy(self):
        for mode in ("grouped", "legacy-model"):
            with self.subTest(mode=mode):
                self.ask("--model", "selected-model", mode=mode)
                self.assertEqual(self.state_json()["model_at_prompt"], "selected-model")
        self.assertEqual(self.state_json()["set_model_config"]["configId"], "engine")
        self.assertEqual(self.state_json()["set_model"]["modelId"], "selected-model")

    def test_explicit_model_errors_precede_prompt(self):
        for mode, model in (
            ("no-models", "selected-model"),
            ("no-model-values", "selected-model"),
            ("normal", "unknown-model"),
            ("grouped", "unknown-model"),
            ("reject-model", "selected-model"),
            ("unconfirmed-model", "selected-model"),
        ):
            with self.subTest(mode=mode):
                self.state.unlink(missing_ok=True)
                result = self.ask("--model", model, mode=mode, success=False)
                self.assertTrue(
                    "model" in result.stderr.lower()
                    or "session/set_config_option" in result.stderr,
                    result.stderr,
                )
                self.assertFalse(self.state_json().get("prompted"))

    def test_unsupported_auth_missing_agent_and_timeout(self):
        for mode, expected in (
            ("auth", "authenticate"),
            ("unsupported-new", "-32601"),
            ("init-timeout", "agent did not answer initialize in time"),
        ):
            with self.subTest(mode=mode):
                start = time.monotonic()
                result = self.ask(
                    mode=mode,
                    success=False,
                    env={"TNY_ACP_RPC_TIMEOUT_MS": "100"}
                    if mode == "init-timeout"
                    else None,
                )
                self.assertIn(expected, result.stderr.lower())
                self.assertLess(time.monotonic() - start, 5)
        result = subprocess.run(
            [
                str(TNY),
                "--provider",
                "acp",
                "--agent",
                "/missing/fixture-agent",
                "ask",
                "hello",
            ],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=5,
        )
        self.assertNotEqual(result.returncode, 0)

    def test_split_utf8_frames(self):
        for width in ("1", "7", "31"):
            with self.subTest(width=width):
                self.ask(env={"ACP_FIXTURE_SPLIT": width})

    def test_bad_frames_and_crash_terminate(self):
        for mode in ("malformed", "oversized", "crash"):
            with self.subTest(mode=mode):
                start = time.monotonic()
                self.ask(mode=mode, success=False)
                self.assertLess(time.monotonic() - start, 10)

    def test_permissions_allow_and_deny(self):
        for policy, allowed in (("ask", False), ("yolo", True)):
            with self.subTest(policy=policy):
                self.ask(
                    "--permission-mode", policy, mode="permission", success=allowed
                )
                outcome = self.state_json()["permission"]["result"]["outcome"]
                self.assertEqual(outcome.get("optionId") == "yes", allowed)
                if not allowed:
                    self.assertIn(outcome["outcome"], ("selected", "cancelled"))

    def test_unadvertised_callbacks_and_session_validation(self):
        output = self.ask(mode="callbacks")
        caps = self.state_json()["initialize"]["clientCapabilities"]
        self.assertFalse(caps.get("terminal"))
        self.assertFalse(caps.get("fs", {}).get("readTextFile"))
        self.assertFalse(caps.get("fs", {}).get("writeTextFile"))
        self.assertNotIn("WRONG-SESSION-TEXT", output["output"])
        for answer in self.state_json()["callbacks"]:
            self.assertIn("error", answer)

    def test_cancellation_is_bounded(self):
        process = subprocess.Popen(
            self.command(),
            env=dict(self.env, ACP_FIXTURE_MODE="cancel", TNY_ISOLATE="0"),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and not self.state_json().get(
                "cancel_ready"
            ):
                time.sleep(0.02)
            self.assertTrue(self.state_json().get("cancel_ready"))
            start = time.monotonic()
            process.send_signal(signal.SIGINT)
            stdout, stderr = process.communicate(timeout=6)
            self.assertLess(time.monotonic() - start, 6)
            self.assertTrue(self.state_json().get("cancelled"), stdout + stderr)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=3)

    def test_active_acp_background_survives_frontend_loss(self):
        # test_tui's standalone runner interprets argv[1] as a binary path;
        # unittest uses that slot for this selected test name.
        argv = sys.argv
        try:
            sys.argv = [argv[0]]
            from test_tui import Term
        finally:
            sys.argv = argv

        release = self.root / "release-acp"
        env = dict(
            self.env,
            ACP_FIXTURE_SCENARIOS=json.dumps({"background": {"release": str(release)}}),
        )
        term = Term(
            [
                str(TNY),
                "--provider",
                "acp",
                "--agent",
                str(AGENT),
                "--no-extensions",
            ],
            env,
            str(self.workspace),
        )
        session = None
        try:
            term.expect("tny ")
            term.send("ACP-MANAGED:background\r")
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not self.state_json().get("prompted"):
                term.pump(0.05)
            self.assertTrue(self.state_json().get("prompted"), term.buf)
            session = next((self.home / ".tny/sessions").glob("*/*/session.json"))
            runner_pid = int((session.parent / "pid").read_text())
            term.send("\x1b[D")
            term.expect("Agents — all saved sessions")
            saved = json.loads(session.read_text())
            self.assertTrue(saved.get("background"), saved)
            self.assertEqual(int((session.parent / "pid").read_text()), runner_pid)
            self.assertEqual(saved["status"], "running")
            term.proc.kill()  # terminal/frontend loss does not own the ACP turn
            term.proc.wait(timeout=5)
            release.touch()
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                saved = json.loads(session.read_text())
                if saved.get("status") == "done":
                    break
                time.sleep(0.05)
            self.assertEqual(saved["status"], "done", saved)
            self.assertIn("ACP-OK café 🐕", saved["result"]["output"])
        finally:
            release.touch()
            term.close()
            if session and json.loads(session.read_text()).get("status") == "running":
                subprocess.run(
                    [str(TNY), "session", "stop", session.parent.name, "--kill"],
                    env=env,
                    cwd=self.workspace,
                    capture_output=True,
                    timeout=12,
                )

    def test_named_profile_model_and_resume(self):
        (self.home / ".tny/settings.json").write_text(
            json.dumps(
                {"acp": {"fixture": {"command": str(AGENT), "model": "selected-model"}}}
            )
        )
        result = subprocess.run(
            [
                str(TNY),
                "--cwd",
                str(self.workspace),
                "--provider",
                "acp@fixture",
                "ask",
                "--json",
                "fixture",
            ],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        output = json.loads(result.stdout)
        self.assertEqual(output["provider"], "acp@fixture")
        self.assertEqual(self.state_json()["model_at_prompt"], "selected-model")
        # Persisted provider/command/model must suffice without new selectors.
        result = subprocess.run(
            [
                str(TNY),
                "--cwd",
                str(self.workspace),
                "ask",
                "--json",
                "--resume",
                output["session_id"],
                "again",
            ],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.state_json()["load_requested"], "fixture-session-1")

    def test_ssh_fails_before_any_local_execution(self):
        remote = self.root / "remote"
        remote.mkdir()
        bins = self.fake_ssh(remote)
        result = self.ask(
            "--ssh",
            "fixture@example.test",
            "--ssh-cwd",
            str(remote),
            env={
                "PATH": str(bins) + os.pathsep + self.env["PATH"],
                "ACP_FIXTURE_NAME": "fixture",
            },
            success=False,
        )
        self.assertIn("verified Claude ACP", result.stderr)
        self.assertFalse(self.state_json().get("prompted"))
        self.assertNotIn("new_cwd", self.state_json())

    def test_native_tool_schema_exact_parity_and_roundtrips(self):
        calls = [
            {
                "name": "write_file",
                "arguments": {"path": "fixture.txt", "content": "ACP file contents\n"},
            },
            {"name": "read_file", "arguments": {"path": "fixture.txt"}},
            {
                "name": "terminal",
                "arguments": {"command": "printf 'ACP-SHELL-OK'", "timeout_s": 2},
            },
            {"name": "read_file", "arguments": {"path": "missing-fixture.txt"}},
            {"name": "not_a_tool", "arguments": {}},
        ]
        output = self.ask(
            env={"ACP_FIXTURE_MCP": "1", "ACP_FIXTURE_CALLS": json.dumps(calls)}
        )
        facts = self.state_json()
        self.assertEqual(
            (self.workspace / "fixture.txt").read_text(), "ACP file contents\n"
        )
        results = facts["tool_results"]
        self.assertEqual(len(results), len(calls))
        self.assertIn("ACP file contents", json.dumps(results[1]))
        self.assertIn("ACP-SHELL-OK", json.dumps(results[2]))
        # Lua executed successfully; nested tool failures remain result text.
        self.assertIn("error", json.dumps(results[3]).lower())
        self.assertIn("error", json.dumps(results[4]).lower())
        self.assertTrue(output.get("tool_calls"), output)
        for profile in ("all", "terminal"):
            if profile != "all":
                self.ask(env={"ACP_FIXTURE_MCP": "1", "TNY_TOOLS": profile})
                facts = self.state_json()
            bridge_tools = {tool["name"]: tool for tool in facts["tools"]}
            self.assertEqual(set(bridge_tools), {"run_code"})
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), NativeCapture)
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                result = subprocess.run(
                    [
                        str(TNY),
                        "--cwd",
                        str(self.workspace),
                        "--provider",
                        "openai",
                        "--wire-api",
                        "chat",
                        "ask",
                        "--json",
                        "fixture",
                    ],
                    env=dict(
                        self.env,
                        OPENAI_BASE_URL=f"http://127.0.0.1:{server.server_port}/v1",
                        OPENAI_API_KEY="fixture-not-real",
                        TNY_TOOLS=profile,
                    ),
                    capture_output=True,
                    text=True,
                    timeout=15,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                native_tools = {}
                for entry in server.captured["tools"]:
                    function = entry["function"]
                    native_tools[function["name"]] = {
                        "name": function["name"],
                        "description": function["description"],
                        "inputSchema": function["parameters"],
                    }
                self.assertEqual(bridge_tools, native_tools)
            finally:
                server.shutdown()
                thread.join(timeout=3)
                server.server_close()

    def test_bridge_half_close_drains_last_response(self):
        self.ask(mode="mcp-half-close", env={"ACP_FIXTURE_MCP": "1"})
        answer = next(
            answer
            for answer in self.state_json()["half_close"]
            if answer.get("id") == 900
        )
        self.assertEqual(answer["result"]["tools"], self.state_json()["tools"])

    def test_acp_catalog_provenance_newer_and_stale_adapter(self):
        # The same client must preserve advertised IDs/names, not guess a
        # newer Claude Code release from a stale adapter's generic alias.
        for advertised in ("Opus (1M context)", "Opus 5.5"):
            catalog = [
                {"value": "default", "name": "Default"},
                {"value": "opus[1m]", "name": advertised},
            ]
            result = subprocess.run(
                [
                    str(TNY),
                    "--cwd",
                    str(self.workspace),
                    "--provider",
                    "acp",
                    "--agent",
                    str(AGENT),
                    "models",
                    "--json",
                ],
                env={**self.env, "ACP_FIXTURE_CATALOG": json.dumps(catalog)},
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                json.loads(result.stdout)["models"],
                [
                    {"id": "default", "name": "Default"},
                    {"id": "opus[1m]", "name": advertised},
                ],
            )

    def test_models_catalog_and_negotiated_effort(self):
        result = subprocess.run(
            [
                str(TNY),
                "--cwd",
                str(self.workspace),
                "--provider",
                "acp",
                "--agent",
                str(AGENT),
                "models",
                "--json",
            ],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            [model["id"] for model in json.loads(result.stdout)["models"]],
            ["default-model", "selected-model"],
        )
        self.assertFalse(self.state_json().get("prompted"))
        self.ask(
            "--model",
            "selected-model",
            "--effort",
            "high",
            env={"ACP_FIXTURE_EFFORT": "1"},
        )
        self.assertEqual(self.state_json()["selected_effort"], "high")
        for effort, supported in (("high", "0"), ("max", "1")):
            self.state.unlink(missing_ok=True)
            failure = self.ask(
                "--effort", effort, success=False, env={"ACP_FIXTURE_EFFORT": supported}
            )
            self.assertTrue(
                "thought" in failure.stderr.lower()
                or "reasoning" in failure.stderr.lower(),
                failure.stderr,
            )
            self.assertFalse(self.state_json().get("prompted"))

    def test_usage_cumulative_currency_and_unreported_tokens(self):
        updates = [
            {"used": 10, "size": 1000, "cost": {"amount": 0.1, "currency": "USD"}},
            {"used": 20, "size": 1000, "cost": {"amount": 0.25, "currency": "EUR"}},
            {"used": 0, "size": 1000},
        ]
        for isolated in ("0", "1"):
            with self.subTest(isolated=isolated):
                output = self.ask(
                    env={
                        "TNY_ISOLATE": isolated,
                        "ACP_FIXTURE_USAGE": json.dumps(updates),
                    }
                )
                self.assertEqual(
                    output["usage"],
                    {
                        "input_tokens": None,
                        "output_tokens": None,
                        "context_used": 0,
                        "context_size": 1000,
                        "cost": 0.25,
                        "cost_currency": "EUR",
                        "cost_scope": "session",
                    },
                )

    def test_usage_invalid_or_legacy_cost_does_not_invent_currency(self):
        for cost in (
            {"amount": -1, "currency": "USD"},
            {"amount": 0.5},
            {"amount": 0.5, "currency": "x" * 17},
            {"amount": "0.5", "currency": "USD"},
            None,
            0.125,
        ):
            with self.subTest(cost=cost):
                output = self.ask(
                    env={"ACP_FIXTURE_USAGE": json.dumps([{"cost": cost}])}
                )
                usage = output["usage"]
                self.assertIsNone(usage["input_tokens"])
                self.assertIsNone(usage["output_tokens"])
                self.assertIsNone(usage["context_used"])
                self.assertIsNone(usage["context_size"])
                self.assertIsNone(usage["cost_currency"])
                self.assertEqual(usage["cost"], 0.125 if cost == 0.125 else None)

    def test_claude_tools_only_and_permission_mode(self):
        for policy in ("yolo", "ask", "auto"):
            self.ask(
                "--permission-mode",
                policy,
                env={
                    "ACP_FIXTURE_NAME": "@agentclientprotocol/claude-agent-acp",
                    "ACP_FIXTURE_VERSION": "0.75.1",
                },
            )
            facts = self.state_json()
            self.assertEqual(
                facts["session_meta"]["claudeCode"]["options"],
                {"tools": [], "settingSources": [], "strictMcpConfig": True},
            )
            self.assertTrue(facts["session_meta"]["disableBuiltInTools"])
            self.assertEqual(facts["selected_mode"], "bypassPermissions")

    def test_claude_bridge_events_are_authoritative_and_unknown_agents_rejected(self):
        (self.workspace / "read.txt").write_text("SAFE-READ")
        calls = [
            {"name": "read_file", "arguments": {"path": "read.txt"}},
            {
                "name": "write_file",
                "arguments": {"path": "written.txt", "content": "OK"},
            },
            {
                "name": "terminal",
                "arguments": {"command": "printf TOOL-OK", "timeout_s": 2},
            },
        ]
        for name, version, expected in (
            ("@agentclientprotocol/claude-agent-acp", "0.75.1", 3),
            ("@agentclientprotocol/claude-agent-acp", "unverified", 0),
            ("fixture", "1", 0),
        ):
            with self.subTest(name=name, version=version):
                self.state.unlink(missing_ok=True)
                output = self.ask(
                    success=bool(expected),
                    env={
                        "ACP_FIXTURE_NAME": name,
                        "ACP_FIXTURE_VERSION": version,
                        "ACP_FIXTURE_MCP": "1",
                        "ACP_FIXTURE_CALLS": json.dumps(calls),
                        "ACP_FIXTURE_TOOL_UPDATES": "1",
                    },
                )
                if not expected:
                    self.assertIn("verified Claude ACP", output.stderr)
                    self.assertFalse(self.state_json().get("prompted"))
                    continue
                nested = [
                    call for call in output["tool_calls"] if call["name"] != "run_code"
                ]
                self.assertEqual(len(nested), expected, output)
                results = self.state_json()["tool_results"]
                self.assertEqual(len(results), 3)
                self.assertTrue(
                    all(
                        "result" in reply and not reply["result"].get("isError")
                        for reply in results
                    ),
                    results,
                )
                self.assertEqual((self.workspace / "written.txt").read_text(), "OK")

    def test_claude_tny_permissions_allow_reads_and_deny_writes(self):
        (self.workspace / "read.txt").write_text("SAFE-READ")
        env = {
            "ACP_FIXTURE_NAME": "@agentclientprotocol/claude-agent-acp",
            "ACP_FIXTURE_VERSION": "0.75.1",
            "ACP_FIXTURE_MCP": "1",
        }
        for policy in ("ask", "auto"):
            (self.home / ".tny/settings.json").write_text(
                json.dumps({"permission": {"edit": "deny"}} if policy == "auto" else {})
            )
            with self.subTest(policy=policy):
                env["ACP_FIXTURE_CALLS"] = json.dumps(
                    [
                        {"name": "read_file", "arguments": {"path": "read.txt"}},
                    ]
                )
                self.ask("--permission-mode", policy, env=env)
                self.assertIn(
                    "SAFE-READ", json.dumps(self.state_json()["tool_results"])
                )
                self.assertEqual(
                    self.state_json()["selected_mode"], "bypassPermissions"
                )
                env["ACP_FIXTURE_CALLS"] = json.dumps(
                    [
                        {
                            "name": "write_file",
                            "arguments": {"path": "denied.txt", "content": "NO"},
                        },
                    ]
                )
                failure = self.ask("--permission-mode", policy, env=env, success=False)
                self.assertEqual(failure.returncode, 2, failure.stderr)
                self.assertFalse((self.workspace / "denied.txt").exists())

    def fake_ssh(self, remote):
        bins = self.root / "bin"
        bins.mkdir()
        ssh = bins / "ssh"
        ssh.write_text(
            f"#!{os.path.realpath(sys.executable)}\nimport os,subprocess,sys\nargs=sys.argv[1:]\nif args[-1] == 'true' or 'exit' in args: sys.exit(0)\nos.environ['HOME']={str(remote)!r}\nsys.exit(subprocess.call(['sh','-c',args[-1]]))\n"
        )
        ssh.chmod(0o755)
        return bins

    def test_claude_ssh_workspace_tools_and_remote_context(self):
        remote = self.root / "remote"
        remote.mkdir()
        (remote / "AGENTS.md").write_text("ACP-REMOTE-ONLY-RULES")
        (self.workspace / "AGENTS.md").write_text("ACP-LOCAL-EXCLUDED-RULES")
        bins = self.fake_ssh(remote)
        calls = [
            {
                "name": "write_file",
                "arguments": {"path": "remote.txt", "content": "REMOTE-ACP-WRITE"},
            },
            {"name": "terminal", "arguments": {"command": "pwd", "timeout_s": 2}},
        ]
        self.ask(
            "--ssh",
            "fixture@example.test",
            "--ssh-cwd",
            str(remote),
            env={
                "PATH": str(bins) + os.pathsep + self.env["PATH"],
                "ACP_FIXTURE_NAME": "@agentclientprotocol/claude-agent-acp",
                "ACP_FIXTURE_VERSION": "0.75.1",
                "ACP_FIXTURE_MCP": "1",
                "ACP_FIXTURE_CALLS": json.dumps(calls),
            },
        )
        facts = self.state_json()
        prompt = "\n".join(block.get("text", "") for block in facts["prompt"])
        self.assertIn("ACP-REMOTE-ONLY-RULES", prompt)
        self.assertNotIn("ACP-LOCAL-EXCLUDED-RULES", prompt)
        self.assertNotEqual(facts["new_cwd"], str(self.workspace))
        self.assertNotEqual(facts["process_cwd"], str(self.workspace))
        self.assertEqual((remote / "remote.txt").read_text(), "REMOTE-ACP-WRITE")
        self.assertFalse((self.workspace / "remote.txt").exists())
        self.assertIn(str(remote), json.dumps(facts["tool_results"][1]))

    def test_extensions_rewrite_and_replace_tool_result(self):
        directory = self.home / ".tny/extensions"
        directory.mkdir()
        (
            directory / "fixture.py"
        ).write_text("""from tny_ext import PreToolUseEvent, PostToolUseEvent, rewrite_tool, replace_tool_result

def setup(api):
    @api.on(PreToolUseEvent)
    def before(event):
        if event.tool_name == 'write_file':
            return rewrite_tool({'path': 'rewritten.txt', 'content': 'HOOK-WRITTEN'})
    @api.on(PostToolUseEvent)
    def after(event):
        if event.tool_name == 'write_file':
            return replace_tool_result('HOOK-RESULT')
""")
        calls = [
            {
                "name": "write_file",
                "arguments": {"path": "original.txt", "content": "ORIGINAL"},
            }
        ]
        self.ask(env={"ACP_FIXTURE_MCP": "1", "ACP_FIXTURE_CALLS": json.dumps(calls)})
        self.assertFalse((self.workspace / "original.txt").exists())
        self.assertEqual((self.workspace / "rewritten.txt").read_text(), "HOOK-WRITTEN")
        self.assertIn("HOOK-RESULT", json.dumps(self.state_json()["tool_results"]))

    def test_context_and_imported_mcp_roundtrip(self):
        from test_mcp_call import FAKE_SERVER

        (self.workspace / "AGENTS.md").write_text("ACP-PROJECT-INSTRUCTIONS")
        server = self.root / "upstream-mcp.sh"
        server.write_text(FAKE_SERVER)
        server.chmod(0o755)
        (self.home / ".tny/mcp.json").write_text(
            json.dumps({"servers": {"fixture": {"command": [str(server)]}}})
        )
        calls = [
            {
                "name": "mcp_select_tool",
                "arguments": {
                    "server": "fixture",
                    "tool": "echo",
                    "arguments": {"text": "ACP-UPSTREAM-OK", "times": 3},
                },
            }
        ]
        self.ask(env={"ACP_FIXTURE_MCP": "1", "ACP_FIXTURE_CALLS": json.dumps(calls)})
        facts = self.state_json()
        text = "\n".join(block.get("text", "") for block in facts["prompt"])
        self.assertIn("ACP-PROJECT-INSTRUCTIONS", text)
        self.assertIn("ACP-UPSTREAM-OK", json.dumps(facts["tool_results"]))
        self.assertFalse(facts["tool_results"][0].get("result", {}).get("isError"))

    def test_prompt_image_negotiation_and_exact_mcp_image_bytes(self):
        from test_image_input import PNG

        path = self.workspace / "tiny.png"
        path.write_bytes(PNG)
        command = self.command()[:-1] + ["--image", str(path), "fixture"]
        for supported in (False, True):
            self.state.unlink(missing_ok=True)
            result = subprocess.run(
                command,
                env=dict(self.env, ACP_FIXTURE_IMAGE="1" if supported else "0"),
                capture_output=True,
                text=True,
                timeout=15,
            )
            if supported:
                self.assertEqual(result.returncode, 0, result.stderr)
                image = next(
                    block
                    for block in self.state_json()["prompt"]
                    if block["type"] == "image"
                )
                self.assertEqual(base64.b64decode(image["data"]), PNG)
                self.assertEqual(image["mimeType"], "image/png")
            else:
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("image", result.stderr.lower())
                self.assertFalse(self.state_json().get("prompted"))
        calls = [{"name": "read_image", "arguments": {"path": "tiny.png"}}]
        self.ask(
            env={
                "ACP_FIXTURE_MCP": "1",
                "ACP_FIXTURE_IMAGE": "1",
                "ACP_FIXTURE_CALLS": json.dumps(calls),
            }
        )
        images = [
            block
            for block in self.state_json()["tool_results"][0]["result"]["content"]
            if block["type"] == "image"
        ]
        self.assertEqual(len(images), 1)
        self.assertEqual(base64.b64decode(images[0]["data"]), PNG)
        self.assertEqual(images[0]["mimeType"], "image/png")

    def test_oversized_image_tool_returns_explicit_failure(self):
        from test_image_input import PNG

        (self.workspace / "large.png").write_bytes(PNG + bytes(4 * 1024 * 1024))
        calls = [{"name": "read_image", "arguments": {"path": "large.png"}}]
        self.ask(
            env={
                "ACP_FIXTURE_MCP": "1",
                "ACP_FIXTURE_IMAGE": "1",
                "ACP_FIXTURE_CALLS": json.dumps(calls),
            }
        )
        result = self.state_json()["tool_results"][0]["result"]
        self.assertTrue(result["isError"])
        self.assertFalse(any(block["type"] == "image" for block in result["content"]))
        self.assertIn("4 mib", json.dumps(result).lower())
        self.assertIn("exceeds", json.dumps(result).lower())

    def test_cancel_pending_bridge_tool_is_bounded(self):
        calls = [
            {
                "name": "terminal",
                "arguments": {
                    "command": "sleep 20; printf late > cancelled.txt",
                    "timeout_s": 25,
                },
            }
        ]
        process = subprocess.Popen(
            self.command(),
            env=dict(
                self.env,
                TNY_ISOLATE="0",
                ACP_FIXTURE_MCP="1",
                ACP_FIXTURE_CALLS=json.dumps(calls),
            ),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and not self.state_json().get(
                "tool_call_started"
            ):
                time.sleep(0.02)
            self.assertEqual(self.state_json().get("tool_call_started"), "terminal")
            time.sleep(0.05)
            process.send_signal(signal.SIGINT)
            process.communicate(timeout=6)
            self.assertFalse((self.workspace / "cancelled.txt").exists())
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=3)

    def test_mcp_sensitive_tool_denied_without_side_effect(self):
        calls = [
            {
                "name": "write_file",
                "arguments": {"path": "denied.txt", "content": "must not write"},
            }
        ]
        failure = self.ask(
            "--permission-mode",
            "ask",
            env={"ACP_FIXTURE_MCP": "1", "ACP_FIXTURE_CALLS": json.dumps(calls)},
            success=False,
        )
        self.assertEqual(failure.returncode, 2)
        self.assertFalse((self.workspace / "denied.txt").exists())
        result = self.state_json()["tool_results"][0]
        self.assertTrue(result.get("result", {}).get("isError") or "error" in result)


if __name__ == "__main__":
    # run.sh may pass the binary positionally; unittest otherwise treats it as a test name.
    if len(sys.argv) == 2 and Path(sys.argv[1]).is_file():
        TNY = Path(sys.argv.pop()).resolve()
    if os.environ.get("TNY_TEST_EXPECT_WASM") == "1" or "/wasm/" in str(TNY):
        with tempfile.TemporaryDirectory(prefix="tny-acp-wasm-") as temporary:
            home = Path(temporary)
            env = clean_env(home)
            env["ACP_FIXTURE_STATE"] = str(home / "state.json")
            result = subprocess.run(
                [
                    str(TNY),
                    "--ephemeral",
                    "--provider",
                    "acp",
                    "--agent",
                    str(AGENT),
                    "ask",
                    "hello",
                ],
                env=env,
                capture_output=True,
                text=True,
                timeout=15,
            )
            assert result.returncode != 0, result
            assert "external agent processes are unsupported on WebAssembly" in (
                result.stdout + result.stderr
            ), result
            assert not (home / "state.json").exists(), "wasm spawned the agent"
        print("PASS ACP wasm clean unsupported diagnostic")
    else:
        unittest.main()
