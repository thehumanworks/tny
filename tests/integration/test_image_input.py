#!/usr/bin/env python3
"""Conversation image-input gating (docs/adr/0089) against HTTP fixtures.

Runs the real binary with a throwaway HOME, actual ~/.tny/settings.json
parsing, fake credentials and a loopback provider that counts requests. No
live provider, no real credential is read. `image attach`/preview behavior
beyond this prerequisite is not covered here.
"""

from __future__ import annotations

import base64
import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TNY = str(Path(os.environ.get("TNY", ROOT / "build/tny")).resolve())
WASM = "wasm" in TNY
PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a0xoAAAAASUVORK5CYII="
)
REFUSAL = "image input is disabled for this provider by settings.json image_input"
TOKEN = "fixture-image-input-token"
ACCOUNT = "fixture-image-input-account"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, status, ct, body):
        self.send_response(status)
        self.send_header("Content-Type", ct)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        s = self.server.state
        raw = self.rfile.read(int(self.headers["Content-Length"]))
        if self.path in (
            "/backend-api/codex/images/generations",
            "/backend-api/codex/images/edits",
        ):
            s["images"].append(json.loads(raw))
            self.reply(
                200,
                "application/json",
                json.dumps(
                    {
                        "created": 1,
                        "data": [{"b64_json": base64.b64encode(PNG).decode()}],
                    }
                ).encode(),
            )
            return
        if self.path != "/v1/chat/completions":
            self.reply(404, "text/plain", b"bad route")
            return
        body = json.loads(raw)
        s["chat"].append(body)
        call = s.get("tool_call")
        if call and not any(m.get("role") == "tool" for m in body["messages"]):
            delta = {
                "tool_calls": [
                    {
                        "index": 0,
                        "id": "image-input-call",
                        "type": "function",
                        "function": {
                            "name": call["name"],
                            "arguments": json.dumps(call["arguments"]),
                        },
                    }
                ]
            }
            reason = "tool_calls"
        else:
            delta, reason = {"content": "MOCK-OK"}, "stop"
        frames = [
            {"choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
            {"choices": [{"index": 0, "delta": {}, "finish_reason": reason}]},
        ]
        self.reply(
            200,
            "text/event-stream",
            (
                "".join(f"data: {json.dumps(f)}\n\n" for f in frames)
                + "data: [DONE]\n\n"
            ).encode(),
        )


class ImageInputTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="tny-image-input-")
        self.home = Path(self.tmp.name)
        self.ws = self.home / "ws"
        self.ws.mkdir()
        (self.home / "codex").mkdir()
        self.png = self.ws / "shot.png"
        self.png.write_bytes(PNG)
        self.state = {"chat": [], "images": [], "tool_call": None}
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.state = self.state
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.server.server_port}"
        self.env = {
            "HOME": str(self.home),
            "CODEX_HOME": str(self.home / "codex"),
            "PATH": os.environ["PATH"],
            "LANG": "C.UTF-8",
            "OPENAI_BASE_URL": self.url + "/v1",
            "OPENAI_API_KEY": "fixture-openai-key",
            "OPENAI_WIRE_API": "chat",
            "TNY_ISOLATE": "0",
        }

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.tmp.cleanup()

    # ---- helpers ----

    def settings(self, obj):
        (self.home / ".tny").mkdir(exist_ok=True)
        (self.home / ".tny/settings.json").write_text(json.dumps(obj))

    def run_tny(self, *args, env=None, timeout=60):
        return subprocess.run(
            [TNY, "--cwd", str(self.ws), *args],
            cwd=self.ws,
            env=dict(self.env, **(env or {})),
            capture_output=True,
            text=True,
            timeout=timeout,
        )

    def state_files(self):
        base = self.home / ".tny"
        if not base.exists():
            return set()
        return {str(p.relative_to(base)) for p in base.rglob("*")}

    def image_parts(self, body):
        parts = []
        for message in body["messages"]:
            content = message.get("content")
            if isinstance(content, list):
                parts += [p for p in content if p.get("type") == "image_url"]
        return parts

    def tool_names(self, body):
        return [t["function"]["name"] for t in body.get("tools", [])]

    # ---- gates ----

    def test_configured_false_refuses_cli_image_without_request_or_session(self):
        self.settings({"image_input": {"openai": False}})
        before = self.state_files()
        r = self.run_tny("ask", "--image", str(self.png), "describe this")
        # the CLI gate's own oracle: no provider request and no session state,
        # asserted before the exit code so a removed pre-session check is
        # distinguishable from the engine-level refusal behind it
        self.assertEqual(self.state["chat"], [])
        self.assertEqual(self.state_files(), before)
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn(REFUSAL, r.stderr)
        self.assertEqual(r.stdout, "")

    def test_configured_false_refuses_resume_image_before_touching_the_session(self):
        # a saved session exists and is resumable; the refused --image run
        # must not open it, take its writer lock or contact the provider
        self.settings({})
        first = self.run_tny("ask", "hello")
        self.assertEqual(first.returncode, 0, first.stderr)
        before_files = self.state_files()
        before_requests = len(self.state["chat"])

        self.settings({"image_input": {"openai": False}})
        r = self.run_tny(
            "ask", "--resume", "last", "--image", str(self.png), "describe this"
        )
        self.assertEqual(len(self.state["chat"]), before_requests)
        self.assertEqual(self.state_files() - before_files, set())
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn(REFUSAL, r.stderr)

    def test_unknown_and_configured_true_still_send_image_parts(self):
        for label, config in (
            ("unknown", {}),
            ("true", {"image_input": {"openai": True}}),
        ):
            with self.subTest(capability=label):
                self.state["chat"].clear()
                self.settings(config)
                r = self.run_tny(
                    "ask", "--no-save", "--image", str(self.png), "describe this"
                )
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn("MOCK-OK", r.stdout)
                parts = self.image_parts(self.state["chat"][0])
                self.assertEqual(len(parts), 1)
                self.assertTrue(
                    parts[0]["image_url"]["url"].startswith("data:image/png;base64,")
                )

    def test_read_image_schema_and_direct_call_agree(self):
        self.state["tool_call"] = {
            "name": "read_image",
            "arguments": {"path": str(self.png)},
        }
        self.settings({"image_input": {"openai": False}})
        r = self.run_tny("ask", "--no-save", "inspect the screenshot")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("read_image", self.tool_names(self.state["chat"][0]))
        results = [
            m for m in self.state["chat"][1]["messages"] if m.get("role") == "tool"
        ]
        self.assertEqual(len(results), 1)
        self.assertTrue(
            results[0]["content"].startswith("error:"), results[0]["content"]
        )
        self.assertIn("read_image", results[0]["content"])

        self.state["chat"].clear()
        self.settings({"image_input": {"openai": True}})
        r = self.run_tny("ask", "--no-save", "inspect the screenshot")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("read_image", self.tool_names(self.state["chat"][0]))
        results = [
            m for m in self.state["chat"][1]["messages"] if m.get("role") == "tool"
        ]
        self.assertFalse(
            results[0]["content"].startswith("error:"), results[0]["content"]
        )
        self.assertIn("image/png", results[0]["content"])
        self.assertEqual(len(self.image_parts(self.state["chat"][1])), 1)

    def test_generation_is_independent_of_chat_image_input(self):
        self.settings({"image_input": {"openai": False}})
        out = self.ws / "generated.png"
        env = {
            "CHATGPT_ACCESS_TOKEN": TOKEN,
            "CHATGPT_ACCOUNT_ID": ACCOUNT,
            "TNY_CODEX_BASE_URL": self.url + "/backend-api/codex",
        }
        r = subprocess.run(
            [
                TNY,
                "--cwd",
                str(self.ws),
                "image",
                "generate",
                "--output-file",
                str(out),
                "--json",
            ],
            input=b"An orange robot reading under a tree",
            cwd=self.ws,
            env=dict(self.env, **env),
            capture_output=True,
            timeout=60,
        )
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertEqual(out.read_bytes(), PNG)
        self.assertEqual(len(self.state["images"]), 1)
        self.assertEqual(self.state["chat"], [])

    # ---- actual runtime parsing, independent of the editor schema ----

    def test_malformed_maps_fail_configuration_before_any_request(self):
        cases = [
            ('{"image_input":[]}', "must be an object"),
            ('{"image_input":"openai"}', "must be an object"),
            ('{"image_input":{"openai":"yes"}}', "must be true or false"),
            ('{"image_input":{"openai":1}}', "must be true or false"),
            ('{"image_input":{"openai":null}}', "must be true or false"),
            ('{"image_input":{"":true}}', "keys must be provider names"),
            ('{"image_input":{"open ai":true}}', "keys must be provider names"),
            ('{"image_input":{"acp:agent":true}}', "keys must be provider names"),
            ('{"image_input":{"open\\u0000ai":true}}', "keys must be provider names"),
            (
                '{"image_input":{"' + "a" * 257 + '":true}}',
                "keys must be provider names",
            ),
            (
                '{"image_input":{"openai":true,"openai":false}}',
                "defined more than once",
            ),
        ]
        for raw, expected in cases:
            with self.subTest(settings=raw[:60]):
                (self.home / ".tny").mkdir(exist_ok=True)
                (self.home / ".tny/settings.json").write_text(raw)
                r = self.run_tny("ask", "--no-save", "hello")
                self.assertNotEqual(r.returncode, 0)
                self.assertIn("settings.json image_input", r.stderr)
                self.assertIn(expected, r.stderr)
                self.assertEqual(self.state["chat"], [])

    def test_acp_alias_cannot_bypass_false_and_transport_still_refuses(self):
        if WASM:
            self.skipTest("wasm cannot spawn the ACP agent process")
        agent = str(ROOT / "tests/integration/fake_acp_agent.py")
        for selector in ("acp@agent", "acp:agent"):
            with self.subTest(selector=selector):
                self.settings(
                    {
                        "acp": {"agent": {"command": sys.executable, "args": [agent]}},
                        "image_input": {"acp@agent": False},
                    }
                )
                r = self.run_tny(
                    "--provider",
                    selector,
                    "ask",
                    "--image",
                    str(self.png),
                    "describe this",
                )
                self.assertEqual(r.returncode, 1, r.stderr)
                self.assertIn(REFUSAL, r.stderr)

        # configured true never overrides the ACP client's actual rejection
        self.settings(
            {
                "acp": {"agent": {"command": sys.executable, "args": [agent]}},
                "image_input": {"acp@agent": True},
            }
        )
        r = self.run_tny(
            "--provider", "acp@agent", "ask", "--image", str(self.png), "describe this"
        )
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("image prompts are not supported", r.stderr)
        self.assertNotIn(REFUSAL, r.stderr)

    def test_switching_providers_recomputes_the_capability(self):
        self.settings(
            {
                "gateway": {
                    "base_url": self.url + "/v1",
                    "api_key": "fixture-gateway-key",
                },
                "image_input": {"openai": False},
            }
        )
        r = self.run_tny("ask", "--image", str(self.png), "describe this")
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn(REFUSAL, r.stderr)
        self.assertEqual(self.state["chat"], [])

        r = self.run_tny(
            "--provider",
            "gateway",
            "ask",
            "--no-save",
            "--image",
            str(self.png),
            "describe this",
            env={"GATEWAY_WIRE_API": "chat"},
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(self.image_parts(self.state["chat"][0])), 1)


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        TNY = str(Path(sys.argv.pop(1)).resolve())
        WASM = "wasm" in TNY
    unittest.main(verbosity=2)
