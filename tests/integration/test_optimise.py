#!/usr/bin/env python3
"""Prompt optimisation through a real native tool loop and PTY composer."""

import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from test_tui import BANNER, TNY, Term, base_env

MODEL = "inception/mercury-2.5"
TEXT = "Fix the parser in src/nested/parser.c; preserve its UTF-8 contract."
READ_TOOLS = {
    "list_files",
    "glob_files",
    "grep_files",
    "read_file",
    "file_info",
    "read_tool_result",
}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        owner = self.server.owner
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        owner.requests.append((self.path, dict(self.headers), body))
        owner.arrived.set()
        messages = body.get("messages", [])
        optimise = any(
            "You optimise a draft prompt" in str(m.get("content")) for m in messages
        )
        if optimise and owner.mode == "error":
            self.send_error(400, "fixture rejection")
            return
        if optimise and owner.mode == "stall":
            owner.release.wait(10)
        results = [m["content"] for m in messages if m["role"] == "tool"]
        calls = [
            ("list_files", {"path": "src"}),
            ("glob_files", {"path": "src", "pattern": "**/*.c"}),
            ("grep_files", {"path": "src", "pattern": "UTF-8"}),
            ("read_file", {"path": "src/nested/parser.c"}),
        ]
        if owner.mode == "write":
            calls = [
                ("write_file", {"path": "src/nested/parser.c", "content": "changed"})
            ]
        elif owner.mode == "outside":
            calls = [("read_file", {"path": str(owner.outside)})]
        elif owner.mode == "loop":
            calls = [("list_files", {"path": "src"})] * 20
        elif owner.mode != "explore":
            calls = []
        delta = {"content": TEXT if optimise else "CHAT-OK"}
        if optimise and len(results) < len(calls):
            name, args = calls[len(results)]
            delta = {
                "content": "I will inspect relevant files first.",
                "tool_calls": [
                    {
                        "index": 0,
                        "id": f"call_{len(results)}",
                        "type": "function",
                        "function": {"name": name, "arguments": json.dumps(args)},
                    }
                ],
            }
        elif optimise and owner.mode == "invalid":
            delta = {"content": "bad\u001b[2Jprompt"}
        elif optimise and owner.mode == "empty":
            delta = {"content": " \n\t"}
        elif optimise and owner.mode == "oversized":
            delta = {"content": "x" * 65537}
        reason = "tool_calls" if "tool_calls" in delta else "stop"
        frames = [
            {"choices": [{"delta": delta, "finish_reason": None}]},
            {"choices": [{"delta": {}, "finish_reason": reason}]},
        ]
        wire = "".join("data: " + json.dumps(frame) + "\n\n" for frame in frames)
        wire += "data: [DONE]\n\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        try:
            # Fragment the framing and UTF-8/JSON boundaries across writes.
            data = wire.encode()
            for at in range(0, len(data), 97):
                self.wfile.write(data[at : at + 97])
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


class OptimiseTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="tny-optimise-test-")
        self.home = Path(self.tmp.name)
        self.ws = self.home / "workspace"
        (self.ws / "src/nested").mkdir(parents=True)
        self.source = self.ws / "src/nested/parser.c"
        self.source.write_text("// UTF-8 parser contract\n")
        (self.ws / "AGENTS.md").write_text("Project uses C11 and UTF-8.\n")
        self.outside = self.home / "outside.txt"
        self.outside.write_text("outside-workspace-marker")
        self.requests = []
        self.arrived = threading.Event()
        self.release = threading.Event()
        self.mode = "explore"
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.owner = self
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}/v1"
        self.env = base_env(
            str(self.home),
            {
                "OPENROUTER_BASE_URL": self.url,
                "OPENROUTER_API_KEY": "fixture-optimise-key",
                "OPENAI_BASE_URL": self.url,
                "OPENAI_API_KEY": "fixture-chat-key",
                "OPENAI_WIRE_API": "chat",
                "TNY_PREWARM": "0",
                "TNY_EXTENSIONS": "0",
            },
        )

    def tearDown(self):
        self.release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.tmp.cleanup()

    def run_cli(self, *args, env=None, input=None, globals=()):
        return subprocess.run(
            [TNY, "--cwd", str(self.ws), *globals, "optimise", *args],
            env=env or self.env,
            input=input,
            capture_output=True,
            text=True,
            timeout=20,
        )

    def settings(self, config):
        path = self.home / ".tny/settings.json"
        path.parent.mkdir(exist_ok=True)
        path.write_text(json.dumps(config))
        return path

    def test_explores_nested_files_and_returns_only_final_draft(self):
        config = {"provider": "cursor", "model": "conversation-model", "fast": "fast"}
        path = self.settings(config)
        result = self.run_cli("--json", "fix parser")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            json.loads(result.stdout),
            {
                "kind": "optimise",
                "provider": "openrouter",
                "model": MODEL,
                "text": TEXT,
            },
        )
        self.assertEqual(len(self.requests), 5)
        for endpoint, headers, body in self.requests:
            self.assertEqual(endpoint, "/v1/chat/completions")
            self.assertEqual(headers["Authorization"], "Bearer fixture-optimise-key")
            self.assertEqual(body["model"], MODEL)
            self.assertEqual({t["function"]["name"] for t in body["tools"]}, READ_TOOLS)
        messages = self.requests[-1][2]["messages"]
        self.assertIn("// UTF-8 parser contract", messages[-1]["content"])
        self.assertIn("nested/parser.c", str(messages))
        self.assertEqual(json.loads(path.read_text()), config)
        self.assertFalse((self.home / ".tny/sessions").exists())
        self.assertEqual(self.source.read_text(), "// UTF-8 parser contract\n")

    def test_override_precedence_and_named_provider_credentials(self):
        self.mode = "simple"
        self.settings(
            {
                "optimise": {"provider": "alt", "model": "settings-model"},
                "alt": {
                    "base_url": self.url,
                    "api_key_env": "ALT_KEY",
                    "wire_api": "chat",
                },
            }
        )
        env = {**self.env, "ALT_KEY": "fixture-alt-key"}
        result = self.run_cli("draft", env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.requests[-1][2]["model"], "settings-model")
        self.assertEqual(
            self.requests[-1][1]["Authorization"], "Bearer fixture-alt-key"
        )
        env.update(TNY_OPTIMISE_MODEL="env-model", TNY_OPTIMISE_PROVIDER="openrouter")
        self.assertEqual(self.run_cli("draft", env=env).returncode, 0)
        self.assertEqual(self.requests[-1][2]["model"], "env-model")
        result = self.run_cli(
            "--provider", "alt", "--model", "flag-model", "draft", env=env
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.requests[-1][2]["model"], "flag-model")
        self.assertEqual(
            self.requests[-1][1]["Authorization"], "Bearer fixture-alt-key"
        )

    def test_stdin_preserves_multiline_unicode_and_skill_mentions(self):
        self.mode = "simple"
        result = self.run_cli(
            "--stdin", input="Improve café support\nUse $deploy as a reference."
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.requests[0][2]["messages"][-1]["content"],
            "Improve café support\nUse $deploy as a reference.",
        )

    def test_default_missing_key_names_openrouter_without_chat_fallback(self):
        env = dict(self.env)
        env.pop("OPENROUTER_BASE_URL")
        env.pop("OPENROUTER_API_KEY")
        result = self.run_cli("draft", env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("OPENROUTER_API_KEY", result.stderr)
        self.assertEqual(self.requests, [])

    def test_ssh_exploration_reads_the_remote_project(self):
        if "wasm" in str(TNY):
            self.skipTest("SSH is native-only")
        remote = self.home / "remote"
        (remote / "src/nested").mkdir(parents=True)
        (remote / "src/nested/parser.c").write_text("// Remote UTF-8 contract\n")
        (remote / "AGENTS.md").write_text("REMOTE-PROJECT-RULES\n")
        bindir = self.home / "bin"
        bindir.mkdir()
        ssh = bindir / "ssh"
        ssh.write_text(
            f"#!{os.path.realpath(sys.executable)}\n"
            "import subprocess, sys\n"
            "args = sys.argv[1:]\n"
            "if args[-1] == 'true' or 'exit' in args: sys.exit(0)\n"
            "sys.exit(subprocess.call(['sh', '-c', args[-1]]))\n"
        )
        ssh.chmod(0o755)
        env = {**self.env, "PATH": str(bindir) + os.pathsep + self.env["PATH"]}
        result = self.run_cli(
            "draft", env=env, globals=("--ssh", "fixture", "--ssh-cwd", str(remote))
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        messages = self.requests[-1][2]["messages"]
        self.assertIn("Remote UTF-8 contract", messages[-1]["content"])
        self.assertIn("REMOTE-PROJECT-RULES", messages[0]["content"])
        self.assertNotIn("Project uses C11", messages[0]["content"])

    def test_cli_cancel_emits_no_partial_prompt(self):
        if "wasm" in str(TNY):
            self.skipTest("native signals")
        self.mode = "stall"
        p = subprocess.Popen(
            [TNY, "--cwd", str(self.ws), "optimise", "draft"],
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            self.assertTrue(self.arrived.wait(5))
            p.send_signal(signal.SIGINT)
            out, err = p.communicate(timeout=5)
            self.assertEqual(p.returncode, 130, err)
            self.assertEqual(out, "")
        finally:
            if p.poll() is None:
                p.kill()
            p.wait(timeout=5)

    def test_unavailable_write_tool_cannot_change_project(self):
        self.mode = "write"
        result = self.run_cli("rewrite my prompt")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("unavailable", str(self.requests[-1][2]["messages"]))
        self.assertEqual(self.source.read_text(), "// UTF-8 parser contract\n")

    def test_outside_workspace_read_is_denied(self):
        self.mode = "outside"
        result = self.run_cli("rewrite my prompt")
        self.assertNotIn("outside-workspace-marker", str(self.requests))
        self.assertNotEqual(result.returncode, 0)

    def test_failed_or_invalid_results_never_print_a_draft(self):
        for mode in ("error", "empty", "invalid", "oversized", "loop"):
            with self.subTest(mode=mode):
                self.mode = mode
                result = self.run_cli("draft")
                self.assertNotEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "")

    def test_invalid_options_and_host_provider_fail_without_requests(self):
        for args in (
            ("--model",),
            ("--provider", "cursor", "draft"),
            ("--provider", "missing", "draft"),
            ("--wat",),
            (" ",),
        ):
            with self.subTest(args=args):
                result = self.run_cli(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
        self.assertEqual(self.requests, [])

    def term(self):
        if "/wasm/" in str(TNY):
            self.skipTest("native PTY; wasm CLI uses the same service")
        t = Term(
            [
                TNY,
                "--cwd",
                str(self.ws),
                "--provider",
                "openai",
                "--model",
                "chat-model",
            ],
            self.env,
            str(self.ws),
        )
        self.addCleanup(lambda: t.proc.wait(timeout=5))
        self.addCleanup(t.close)
        t.expect(BANNER)
        return t

    def test_ctrl_o_replaces_draft_then_enter_uses_conversation_model(self):
        t = self.term()
        t.send("fix parser\x0f")
        t.expect("Prompt optimised")
        t.expect_on_screen(TEXT)
        self.assertEqual(len(self.requests), 5)
        t.send("\r")
        t.expect("CHAT-OK")
        self.assertEqual(self.requests[-1][2]["model"], "chat-model")
        self.assertEqual(
            self.requests[-1][1]["Authorization"], "Bearer fixture-chat-key"
        )
        self.assertEqual(self.requests[-1][2]["messages"][-1]["content"], TEXT)
        t.send("/quit\r")
        self.assertEqual(t.wait(), 0)
        self.assertTrue(t.restored())

    def test_slash_override_and_cancel_preserve_original_prompt(self):
        self.mode = "stall"
        t = self.term()
        t.send("/optimise --model override-model fix parser\r")
        self.assertTrue(self.arrived.wait(5))
        self.assertEqual(self.requests[0][2]["model"], "override-model")
        t.send("ignored\r\x1b[200~pasted\nprompt\x1b[201~")
        t.send("\x1b")
        t.expect("Optimisation cancelled")
        t.expect_on_screen("> fix parser")
        self.release.set()
        t.send("\r")
        t.expect("CHAT-OK")
        self.assertEqual(self.requests[-1][2]["messages"][-1]["content"], "fix parser")
        t.send("/quit\r")
        self.assertEqual(t.wait(), 0)

    def test_slash_preserves_multiline_continuation_before_optimising(self):
        self.mode = "simple"
        t = self.term()
        t.send("/optimise first line\\\r")
        t.expect_on_screen("first line")
        self.assertEqual(self.requests, [])
        t.send("second line\r")
        t.expect("Prompt optimised")
        self.assertEqual(
            self.requests[0][2]["messages"][-1]["content"], "first line\nsecond line"
        )
        self.assertEqual(len(self.requests), 1)
        t.send("\x03/quit\r")
        self.assertEqual(t.wait(), 0)

    def test_slash_error_preserves_draft_and_empty_key_does_not_send(self):
        self.mode = "error"
        t = self.term()
        t.send("\x0f")
        t.expect("type or dictate a prompt first")
        self.assertEqual(self.requests, [])
        t.send("/optimise fix parser\r")
        t.expect("Optimisation failed")
        t.expect_on_screen("> fix parser")
        self.assertEqual(len(self.requests), 1)
        t.send("\x03/quit\r")
        self.assertEqual(t.wait(), 0)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
