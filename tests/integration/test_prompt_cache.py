#!/usr/bin/env python3
"""Fixture-only cache routing, affinity lifetime, and per-request usage checks."""

import http.client
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

TNY = str(
    Path(
        sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", "build/tny")
    ).resolve()
)


class Provider(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_POST(self):
        wire = self.rfile.read(int(self.headers["Content-Length"]))
        body = json.loads(wire)
        chat = "messages" in body
        items = body.get("messages", body.get("input", []))
        users = [i for i, item in enumerate(items) if item.get("role") == "user"]
        step = sum(
            item.get("type") == "function_call_output" or item.get("role") == "tool"
            for item in items[users[-1] + 1 :]
        )
        server = self.server
        server.requests.append((body, dict(self.headers)))
        server.raw_requests.append(wire)
        if server.retry and step == 1 and not server.retried:
            server.retried = True
            error = b'{"error":{"message":"retry fixture","type":"server_error"}}'
            self.send_response(503)
            self.send_header("Content-Length", str(len(error)))
            self.end_headers()
            self.wfile.write(error)
            return
        details = {
            "cached_tokens": (0, 128, 256)[min(step, 2)],
            "cache_write_tokens": 0,
        }
        usage = {
            "prompt_tokens" if chat else "input_tokens": 100 * (step + 1),
            "completion_tokens" if chat else "output_tokens": 10 * (step + 1),
        }
        if not (server.missing and step == 1):
            usage["prompt_tokens_details" if chat else "input_tokens_details"] = details
        if server.empty_usage:
            usage.clear()
        call_id = f"call_{len(users)}_{step}"
        if step < server.max_tool_steps - 1:
            tool_name, tool_args = (
                server.tool_sequence[step]
                if server.tool_sequence
                else ("list_files", '{"path":"."}')
            )
            item = {
                "type": "function_call",
                "call_id": call_id,
                "name": tool_name,
                "arguments": tool_args,
            }
            delta = {
                "tool_calls": [
                    {
                        "index": 0,
                        "id": call_id,
                        "type": "function",
                        "function": {"name": tool_name, "arguments": tool_args},
                    }
                ]
            }
        else:
            item = {
                "type": "message",
                "role": "assistant",
                "content": [{"type": "output_text", "text": "CACHE-OK"}],
            }
            delta = {"content": "CACHE-OK"}
        response = {
            "status": "incomplete" if server.incomplete and step == 2 else "completed",
            "usage": usage,
        }
        if response["status"] == "incomplete":
            response["incomplete_details"] = {"reason": "max_output_tokens"}
        if chat:
            events = [
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": delta,
                            "finish_reason": "tool_calls"
                            if step < server.max_tool_steps - 1
                            else "stop",
                        }
                    ],
                    "usage": usage,
                }
            ]
        else:
            events = [
                {"type": "response.output_item.done", "output_index": 0, "item": item}
            ]
            if step == server.max_tool_steps - 1:
                events.append(
                    {"type": "response.output_text.delta", "delta": "CACHE-OK"}
                )
            events.append(
                {"type": "response." + response["status"], "response": response}
            )
            if server.duplicate:
                events.append(events[-1])
            if server.failed_usage and step == 1 and not server.retried:
                server.retried = True
                events = [
                    {
                        "type": "response.failed",
                        "response": {
                            "usage": usage,
                            "error": {
                                "type": "server_error",
                                "message": "transient fixture failure",
                            },
                        },
                    }
                ]
        self.send_response(200)
        state = (
            f"affinity-{len(users)}" if step == 0 else "must-not-replace-first-token"
        )
        self.send_header(
            "x-codex-turn-state", "x" * 4096 if server.oversized else state
        )
        if server.raw and not chat:
            response["output"] = [item]
            wire = json.dumps(response).encode()
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(wire)))
            self.end_headers()
            self.wfile.write(wire)
        else:
            wire = b"".join(
                b"data: " + json.dumps(event).encode() + b"\n\n" for event in events
            )
            if chat:
                wire += b"data: [DONE]\n\n"
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            # Every event crosses HTTP chunks, including the usage details.
            for pos in range(0, len(wire), 7):
                chunk = wire[pos : pos + 7]
                self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()


class CacheTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name)
        self.ws = self.home / "workspace"
        self.ws.mkdir()
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        self.server.requests = []
        self.server.raw_requests = []
        self.server.max_tool_steps = 3
        self.server.tool_sequence = None
        for flag in [
            "retry",
            "retried",
            "missing",
            "incomplete",
            "duplicate",
            "raw",
            "oversized",
            "empty_usage",
            "failed_usage",
        ]:
            setattr(self.server, flag, False)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.env = {
            k: v
            for k, v in os.environ.items()
            if not k.startswith(("TNY_", "CODEX_", "CHATGPT_", "CLAUDE_"))
            and not k.endswith(("_API_KEY", "_BASE_URL"))
        }
        self.env.update(
            HOME=str(self.home),
            CODEX_HOME=str(self.home / ".codex"),
            CHATGPT_ACCESS_TOKEN="fixture-token",
            CHATGPT_ACCOUNT_ID="fixture-account",
            TNY_CODEX_BASE_URL=f"http://127.0.0.1:{self.server.server_port}/v1",
            TNY_ISOLATE="0",
        )

    def ask(self, *flags, provider="codex", wire=None, expected=0, prompt="first"):
        result = subprocess.run(
            [
                TNY,
                "--cwd",
                str(self.ws),
                "--provider",
                provider,
                *(["--wire-api", wire] if wire else []),
                "ask",
                "--json",
                *flags,
                prompt,
            ],
            env=self.env,
            capture_output=True,
            timeout=20,
        )
        self.assertEqual(result.returncode, expected, result.stderr.decode())
        return json.loads(result.stdout)

    def check_routing(self, requests, enabled=True):
        keys = set()
        for body, original in requests:
            headers = {key.lower(): value for key, value in original.items()}
            if not enabled:
                self.assertNotIn("prompt_cache_key", body)
                self.assertTrue(
                    {"session-id", "thread-id", "x-codex-turn-state"}.isdisjoint(
                        headers
                    )
                )
                continue
            key = body["prompt_cache_key"]
            self.assertTrue(key)
            keys.add(key)
            self.assertEqual(headers.get("session-id"), key)
            self.assertTrue(headers.get("thread-id"))
            inputs = body["input"]
            last_user = max(
                i for i, item in enumerate(inputs) if item.get("role") == "user"
            )
            has_output = any(
                item.get("type") == "function_call_output"
                for item in inputs[last_user + 1 :]
            )
            expected = (
                f"affinity-{sum(item.get('role') == 'user' for item in inputs)}"
                if has_output
                else None
            )
            self.assertEqual(headers.get("x-codex-turn-state"), expected)
            self.assertFalse(body["store"])
            self.assertNotIn("previous_response_id", body)
            self.assertNotIn("prompt_cache_options", body)
        return keys

    def test_resume_retry_accounting_and_prefix(self):
        self.server.retry = self.server.duplicate = True
        first = self.ask()
        second = self.ask("--resume", first["session_id"], prompt="second")
        keys = self.check_routing(self.server.requests)
        self.assertEqual(len(keys), 1)
        self.assertTrue(next(iter(keys)).startswith("tny-ws-"))
        self.assertTrue(
            all(
                headers.get("thread-id") == first["session_id"]
                for _body, headers in self.server.requests
            )
        )
        for result in [first, second]:
            self.assertEqual(
                result["usage"],
                {
                    "input_tokens": 600,
                    "output_tokens": 60,
                    "requests": 3,
                    "cached_input_tokens": 384,
                    "uncached_input_tokens": 216,
                    "cache_write_tokens": 0,
                },
            )
        requests = [body for body, _headers in self.server.requests]
        self.assertEqual(len(requests), 7)
        self.assertTrue(
            all(
                body["instructions"] == requests[0]["instructions"] for body in requests
            )
        )
        self.assertTrue(all(body["tools"] == requests[0]["tools"] for body in requests))
        for left, right in zip(requests, requests[1:]):
            self.assertEqual(left["input"], right["input"][: len(left["input"])])
        session = next((self.home / ".tny" / "sessions").glob("*/*/session.json"))
        data = json.loads(session.read_text())
        self.assertEqual(data["usage"]["in"], 1200)
        self.assertEqual(data["usage"]["cached_in"], 768)
        self.assertNotIn("affinity-", session.read_text())

    def test_experimental_prefix_is_stable_for_five_requests(self):
        self.server.max_tool_steps = 5
        self.env["TNY_EXP_PREFIX"] = "1"
        self.ask()
        requests = [body for body, _headers in self.server.requests]
        self.assertEqual(len(requests), 5)
        first = requests[0]
        names = [tool.get("name") for tool in first["tools"]]
        self.assertIn("tool_search", names)
        self.assertNotIn("image_contact_sheet", names)
        self.assertEqual(first["input"][0]["role"], "developer")
        setup = first["input"][0]
        self.assertIn("Deferred built-in tools:", setup["content"])
        for request in requests[1:]:
            self.assertEqual(first["instructions"], request["instructions"])
            self.assertEqual(first["tools"], request["tools"])
            self.assertEqual(setup, request["input"][0])

    def test_experimental_first_request_keeps_workspace_routing(self):
        self.env["TNY_EXP_PREFIX"] = "1"
        self.server.max_tool_steps = 1
        self.ask()
        first = self.server.raw_requests[0]
        self.ws = self.home / "other-workspace"
        self.ws.mkdir()
        self.ask()
        second = self.server.raw_requests[1]
        self.assertNotEqual(first, second)
        first_body = self.server.requests[0][0]
        second_body = self.server.requests[1][0]
        self.assertNotEqual(
            first_body["prompt_cache_key"], second_body["prompt_cache_key"]
        )
        self.assertNotEqual(
            self.server.requests[0][1]["session-id"],
            self.server.requests[1][1]["session-id"],
        )
        shared = len(os.path.commonprefix([first, second]))
        self.assertLess(first.index(b'"tools":'), first.index(b'"instructions":'))
        self.assertGreater(shared, first.index(b'"instructions":'))
        self.assertGreater(shared, first.index(b'"prompt_cache_key":'))
        self.assertLess(shared, first.index(b'"input":'))

    def test_experimental_prefix_reaches_detached_runner(self):
        self.env.pop("TNY_ISOLATE")
        self.env["TNY_EXP_PREFIX"] = "1"
        self.server.max_tool_steps = 1
        self.ask()
        on = self.server.requests[0][0]
        self.assertIn("tool_search", [tool.get("name") for tool in on["tools"]])
        self.assertEqual(on["input"][0]["role"], "developer")
        self.assertIn("Deferred built-in tools:", on["input"][0]["content"])
        self.env.pop("TNY_EXP_PREFIX")
        self.ask()
        off = self.server.requests[1][0]
        self.assertNotIn("tool_search", [tool.get("name") for tool in off["tools"]])
        self.assertIn(str(self.ws), off["instructions"])

    def test_experimental_discovery_loads_schema_on_next_request(self):
        self.env["TNY_EXP_PREFIX"] = "1"
        self.server.tool_sequence = [
            ("tool_search", '{"query":"file_info"}'),
            ("file_info", '{"path":"."}'),
        ]
        first_turn = self.ask()
        bodies = [body for body, _headers in self.server.requests]
        self.assertEqual(len(bodies), 3)
        first_names = [tool.get("name") for tool in bodies[0]["tools"]]
        second_names = [tool.get("name") for tool in bodies[1]["tools"]]
        self.assertNotIn("file_info", first_names)
        self.assertIn("file_info", second_names)
        self.assertIn("Schema file_info loaded", json.dumps(bodies[1]["input"]))
        self.server.requests.clear()
        self.server.raw_requests.clear()
        self.server.tool_sequence = None
        self.ask("--resume", first_turn["session_id"], prompt="second")
        resumed_names = [
            tool.get("name") for tool in self.server.requests[0][0]["tools"]
        ]
        self.assertIn("file_info", resumed_names)

    def test_experimental_chat_wire_uses_frozen_setup(self):
        self.env["TNY_EXP_PREFIX"] = "1"
        self.env["OPENAI_BASE_URL"] = f"http://127.0.0.1:{self.server.server_port}/v1"
        self.env["OPENAI_API_KEY"] = "synthetic-prefix-fixture"
        self.ask(provider="openai", wire="chat")
        bodies = [body for body, _headers in self.server.requests]
        self.assertEqual(len(bodies), 3)
        first = bodies[0]
        self.assertEqual(first["messages"][1]["role"], "system")
        self.assertIn("Deferred built-in tools:", first["messages"][1]["content"])
        for body in bodies[1:]:
            self.assertEqual(first["tools"], body["tools"])
            self.assertEqual(first["messages"][:2], body["messages"][:2])
        self.assertLess(
            self.server.raw_requests[0].index(b'"tools":'),
            self.server.raw_requests[0].index(b'"messages":'),
        )

    def test_raw_missing_details_and_incomplete_usage(self):
        self.server.raw = self.server.missing = self.server.incomplete = True
        usage = self.ask(expected=2)["usage"]
        self.assertEqual(usage["input_tokens"], 600)
        self.assertEqual(usage["output_tokens"], 60)
        self.assertIsNone(usage["cached_input_tokens"])
        self.assertIsNone(usage["uncached_input_tokens"])
        self.assertIsNone(usage["cache_write_tokens"])

    def test_fresh_sessions_share_workspace_routing_not_thread_identity(self):
        first = self.ask()
        first_key = self.server.requests[0][0]["prompt_cache_key"]
        self.server.requests.clear()
        second = self.ask(prompt="independent")
        self.assertNotEqual(first["session_id"], second["session_id"])
        self.assertEqual(self.check_routing(self.server.requests), {first_key})
        self.assertTrue(
            all(
                headers.get("thread-id") == second["session_id"]
                for _body, headers in self.server.requests
            )
        )

    def test_routing_tracks_workspace_and_effective_tool_profile(self):
        self.ask()
        first_key = self.server.requests[0][0]["prompt_cache_key"]
        self.server.requests.clear()
        self.env["TNY_TOOLS"] = "terminal"
        self.ask()
        profile_key = self.server.requests[0][0]["prompt_cache_key"]
        if "/wasm/" in TNY:
            # config.c deliberately ignores TNY_TOOLS on wasm (ADR 0062),
            # so its effective profile and cache route both stay "all".
            self.assertEqual(first_key, profile_key)
        else:
            self.assertNotEqual(first_key, profile_key)
        self.env.pop("TNY_TOOLS")
        self.server.requests.clear()
        self.ws = self.home / "another-workspace"
        self.ws.mkdir()
        self.ask()
        self.assertNotEqual(first_key, self.server.requests[0][0]["prompt_cache_key"])

    def test_session_scope_preserves_legacy_routing(self):
        self.env["TNY_OPENAI_CACHE_SCOPE"] = "session"
        first = self.ask()
        self.assertEqual(
            self.check_routing(self.server.requests), {first["session_id"]}
        )
        self.server.requests.clear()
        second = self.ask()
        self.assertEqual(
            self.check_routing(self.server.requests), {second["session_id"]}
        )
        self.assertNotEqual(first["session_id"], second["session_id"])

    def test_ephemeral_and_kill_switch(self):
        result = self.ask("--ephemeral")
        self.assertEqual(result["session_id"], "")
        self.assertEqual(result["usage"]["requests"], 3)
        self.assertEqual(len(self.check_routing(self.server.requests)), 1)
        self.server.requests.clear()
        self.env["TNY_OPENAI_CACHE"] = "0"
        self.ask("--ephemeral")
        self.check_routing(self.server.requests, enabled=False)

    def test_compatible_provider_has_no_cache_extensions(self):
        self.env["OPENAI_API_KEY"] = "fixture"
        self.env["OPENAI_BASE_URL"] = self.env["TNY_CODEX_BASE_URL"]
        self.ask(provider="openai")
        self.check_routing(self.server.requests, enabled=False)

    def test_chat_usage_includes_tool_rounds(self):
        settings = self.home / ".tny" / "settings.json"
        settings.parent.mkdir()
        settings.write_text(
            json.dumps(
                {
                    "compat": {
                        "base_url": self.env["TNY_CODEX_BASE_URL"],
                        "api_key_env": "CHATGPT_ACCESS_TOKEN",
                        "wire_api": "chat",
                    }
                }
            )
        )
        usage = self.ask(provider="compat")["usage"]
        self.assertEqual(usage["input_tokens"], 600)
        self.assertEqual(usage["cached_input_tokens"], 384)
        self.check_routing(self.server.requests, enabled=False)

    def test_sse_incomplete_usage(self):
        self.server.incomplete = True
        usage = self.ask(expected=2)["usage"]
        self.assertEqual(usage["requests"], 3)
        self.assertEqual(usage["cached_input_tokens"], 384)

    def test_failed_response_usage_is_counted_once_before_retry(self):
        self.server.failed_usage = True
        usage = self.ask()["usage"]
        self.assertEqual(usage["requests"], 4)
        self.assertEqual(usage["input_tokens"], 800)
        self.assertEqual(usage["output_tokens"], 80)
        self.assertEqual(usage["cached_input_tokens"], 512)
        self.check_routing(self.server.requests)

    def test_empty_usage_is_unknown(self):
        self.server.empty_usage = True
        self.assertIsNone(self.ask()["usage"])

    def test_oversized_affinity_is_ignored(self):
        self.server.oversized = True
        self.ask()
        for _body, headers in self.server.requests:
            self.assertNotIn("x-codex-turn-state", {k.lower() for k in headers})

    def test_isolated_runner_result(self):
        if "/wasm/" in TNY:
            self.skipTest("wasm uses the in-process path")
        self.env["TNY_ISOLATE"] = "1"
        result = self.ask()
        self.assertEqual(result["usage"]["input_tokens"], 600)
        self.assertEqual(result["usage"]["cached_input_tokens"], 384)
        self.check_routing(self.server.requests)

    def test_affinity_resets_between_turns_in_one_process(self):
        if "/wasm/" in TNY or os.name == "nt":
            self.skipTest("native PTY; resume is checked on every target")
        from test_tui import BANNER, Term

        term = Term([TNY, "--provider", "codex"], self.env, str(self.ws))
        self.addCleanup(term.close)
        term.expect(BANNER)
        for prompt in ("first", "second"):
            before = len(self.server.requests)
            term.send(prompt + "\r")
            deadline = time.monotonic() + 20
            while (
                len(self.server.requests) < before + 3 and time.monotonic() < deadline
            ):
                term.pump(0.05)
            self.assertGreaterEqual(len(self.server.requests), before + 3)
            term.pump(0.5)
        self.check_routing(self.server.requests)
        term.send("/quit\r")
        self.assertEqual(term.wait(), 0)


class ReportingTests(unittest.TestCase):
    def test_forwarder_requires_the_random_local_credential(self):
        path = Path(__file__).parents[1] / "bench" / "bench_prompt_cache.py"
        spec = importlib.util.spec_from_file_location("bench_prompt_cache", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        server = ThreadingHTTPServer(("127.0.0.1", 0), module.Forwarder)
        # No upstream credentials: rejection must happen before forwarding.
        server.state = {"expected_auth": "Bearer local-fixture-only", "rows": []}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            client = http.client.HTTPConnection(
                "127.0.0.1", server.server_port, timeout=5
            )
            client.request(
                "POST", "/v1/responses", b"{}", {"Authorization": "Bearer wrong"}
            )
            self.assertEqual(client.getresponse().status, 401)
            self.assertEqual(server.state["rows"], [])
            client.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_weighted_rates_and_unknown_usage(self):
        path = Path(__file__).parents[1] / "bench" / "bench_prompt_cache.py"
        spec = importlib.util.spec_from_file_location("bench_prompt_cache", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        rows = [
            {
                "input_tokens": 100,
                "output_tokens": 1,
                "request_bytes": 1000,
                "cached_input_tokens": 0,
                "first_event_ms": 100,
            },
            {
                "input_tokens": 900,
                "output_tokens": 2,
                "request_bytes": 2000,
                "cached_input_tokens": 900,
                "first_event_ms": 200,
            },
        ]
        result = module.summarize(rows)
        self.assertEqual(result["cache_hit_rate"], 0.9)
        self.assertEqual(result["uncached_input_tokens"], 100)
        self.assertEqual(result["median_first_event_ms"], 150)
        rows[1]["cached_input_tokens"] = None
        result = module.summarize(rows)
        self.assertIsNone(result["cache_hit_rate"])
        self.assertIsNone(result["uncached_input_tokens"])
        self.assertIsNone(module.summarize([]))

    def test_benchmark_requires_explicit_live_opt_in(self):
        path = Path(__file__).parents[1] / "bench" / "bench_prompt_cache.py"
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "metrics.json"
            result = subprocess.run(
                [
                    sys.executable,
                    str(path),
                    "--auth-file",
                    str(output),
                    "--output",
                    str(output),
                ],
                capture_output=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn(b"require --live", result.stderr)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
