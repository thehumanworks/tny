#!/usr/bin/env python3
"""Codex-authenticated search from other providers; offline HTTP/PTY fixtures."""

import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from test_tui import TNY, Term, base_env

TNY = str(Path(TNY).resolve())
WASM = os.environ.get("TNY_TEST_EXPECT_WASM") == "1" or "wasm" in Path(TNY).parts
TOKEN = "fixture-codex-token-not-a-secret"
ACCOUNT = "fixture-codex-account"
ACTIVE_KEY = "fixture-active-key-not-a-secret"
QUERY = "fixture query — café"
SOURCE = "https://example.com/search-source?a=1&b=2"


def attach_dashboard_session(term, session_id):
    """Select the current run even when earlier fixtures left saved rows."""
    term.expect_on_screen("Agents — all saved sessions")
    for _ in range(64):
        selected = next(
            (
                line
                for line in term.screen().splitlines()
                if re.match(r"^>\s+[0-9a-f]{16}\b", line)
            ),
            "",
        )
        if session_id in selected:
            term.send("\r")
            term.expect(f"Attached {session_id}", 5)
            return
        term.send("\x1b[B")
        term.pump(0.05)
    raise AssertionError(
        f"current session {session_id} was not selectable: {term.screen()}"
    )


def strict_object(pairs):
    result = {}
    for key, value in pairs:
        assert key not in result, f"duplicate JSON member {key}"
        result[key] = value
    return result


def event(value):
    return ("data: " + json.dumps(value, ensure_ascii=False) + "\r\n\r\n").encode()


def search_output(mode="ok"):
    output = [
        {
            "id": "ws-fixture",
            "type": "web_search_call",
            "status": "completed" if mode != "failed-tool" else "failed",
            "action": {"type": "search", "query": QUERY},
        },
        {
            "id": "msg-fixture",
            "type": "message",
            "role": "assistant",
            "status": "completed",
            "content": [
                {
                    "type": "output_text",
                    "text": "CODEX-SEARCH-RESULT café",
                    "annotations": [
                        {"type": "url_citation", "url": SOURCE, "title": "Fixture"},
                        {
                            "type": "url_citation",
                            "url": "javascript:alert(1)",
                            "title": "Unsafe",
                        },
                    ],
                }
            ],
        },
    ]
    if mode == "no-search":
        output.pop(0)
    elif mode == "unexpected-tool":
        output.append(
            {"type": "function_call", "name": "terminal", "arguments": "touch unsafe"}
        )
    if mode == "many-sources":
        annotations = output[-1]["content"][0]["annotations"]
        annotations.append(annotations[0].copy())
        annotations.extend(
            {
                "type": "url_citation",
                "url": "https://example.com/" + str(i) + "x" * 3900,
            }
            for i in range(32)
        )
    return output


class Fixture:
    def __init__(self, mode="ok", terminal=False):
        self.mode = mode
        self.terminal = terminal
        self.errors = []
        self.search_requests = []
        self.chat_requests = []
        self.oauth_requests = []
        self.get_requests = []
        self.search_started = threading.Event()
        self.release_search = threading.Event()
        self.followup_started = threading.Event()
        self.release_followup = threading.Event()
        self.block_followup = False
        self.token = TOKEN
        self.model = "gpt-5.6-sol"
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def send(self, data, status=200, content_type="text/event-stream"):
                self.send_response(status)
                if fixture.mode != "missing-content-type":
                    self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                if (
                    fixture.mode in ("split", "bom-stream")
                    and self.path == "/codex/responses"
                ):
                    step = 1 if fixture.mode == "bom-stream" else 7
                    for at in range(0, len(data), step):
                        self.wfile.write(data[at : at + step])
                        self.wfile.flush()
                else:
                    self.wfile.write(data)
                    self.wfile.flush()

            def do_GET(self):
                fixture.get_requests.append(self.path)
                self.send(b"OVERRIDE-RESULT", content_type="text/plain")

            def do_POST(self):
                try:
                    body = json.loads(
                        self.rfile.read(int(self.headers["Content-Length"])),
                        object_pairs_hook=strict_object,
                    )
                    if self.path == "/oauth/token":
                        fixture.oauth_requests.append(body)
                        assert body["grant_type"] == "refresh_token"
                        if fixture.mode == "slow-refresh":
                            fixture.search_started.set()
                            fixture.release_search.wait(15)
                        fixture.token = "fixture-refreshed-codex-token"
                        self.send(
                            json.dumps(
                                {"access_token": fixture.token, "expires_in": 3600}
                            ).encode(),
                            content_type="application/json",
                        )
                        return
                    if self.path == "/codex/responses":
                        self.codex(body)
                    elif self.path.startswith("/active/"):
                        self.chat(body)
                    else:
                        raise AssertionError(f"unexpected endpoint {self.path}")
                except (BrokenPipeError, ConnectionResetError):
                    pass  # explicit cancellation/early failure closes the owned request
                except Exception as exc:
                    fixture.errors.append(repr(exc))
                    self.send(b"fixture rejected request", status=500)

            def codex(self, body):
                fixture.search_requests.append(body)
                assert self.headers["Authorization"] == "Bearer " + fixture.token
                assert self.headers["chatgpt-account-id"] == ACCOUNT
                assert self.headers["OpenAI-Beta"] == "responses=v1"
                assert self.headers["originator"] == "tny"
                assert self.headers.get("X-Active-Only") is None
                assert body["model"] == fixture.model, body
                assert body["tools"] == [
                    {"type": "web_search", "external_web_access": True}
                ]
                assert body["tool_choice"] == "required"
                assert body["stream"] is True and body["store"] is False
                assert body["reasoning"] == {"effort": "low"}
                assert body["input"] == [
                    {
                        "role": "user",
                        "content": [{"type": "input_text", "text": QUERY}],
                    }
                ], body
                assert set(body) == {
                    "model",
                    "tools",
                    "tool_choice",
                    "stream",
                    "store",
                    "reasoning",
                    "instructions",
                    "input",
                }
                assert "read-only" in body["instructions"]
                assert "PROJECT-INSTRUCTION-SENTINEL" not in json.dumps(body)
                assert ACTIVE_KEY not in json.dumps(body)
                fixture.search_started.set()
                mode = fixture.mode
                if mode == "slow-headers":
                    fixture.release_search.wait(15)
                if mode.startswith("http-"):
                    self.send(
                        json.dumps({"error": {"message": TOKEN}}).encode(),
                        status=int(mode[5:]),
                        content_type="application/json",
                    )
                    return
                if mode == "wrong-type":
                    self.send(b"{}", content_type="application/json")
                    return
                if mode == "json-error":
                    self.send(
                        json.dumps({"error": {"message": TOKEN}}).encode(),
                        content_type="application/json",
                    )
                    return
                if mode == "malformed":
                    self.send(b"data: not-json\n\n")
                    return
                if mode == "oversized":
                    self.send(b"data: " + b"x" * (2 * 1024 * 1024 + 1))
                    return
                if mode in ("failed", "incomplete"):
                    self.send(
                        event(
                            {
                                "type": "response." + mode,
                                "response": {"error": {"message": TOKEN}},
                            }
                        )
                    )
                    return
                if mode == "partial":
                    self.send(
                        event(
                            {
                                "type": "response.output_text.delta",
                                "delta": "UNTRUSTED-PARTIAL",
                            }
                        )
                    )
                    return
                if mode == "slow-body":
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.end_headers()
                    self.wfile.write(b": waiting\n\n")
                    self.wfile.flush()
                    fixture.release_search.wait(15)
                    return
                output = search_output(mode)
                if mode == "json-response":
                    self.send(
                        json.dumps({"status": "completed", "output": output}).encode(),
                        content_type="application/json",
                    )
                    return
                data = event({"type": "response.output_item.done", "item": output[0]})
                if mode == "items-before-empty-terminal":
                    for item in output:
                        data += event(
                            {"type": "response.output_item.done", "item": item}
                        )
                        data += event(
                            {"type": "response.output_item.done", "item": item}
                        )
                    output = []
                data += event(
                    {
                        "type": "response.completed",
                        "response": {"status": "completed", "output": output},
                    }
                )
                data += event(
                    {
                        "type": "response.completed",
                        "response": {"status": "completed", "output": output},
                    }
                )
                if mode == "bom-stream":
                    data = b"\xef\xbb\xbf" + data
                elif mode == "unusual-field":
                    data = b"custom-field: harmless\n\n" + data
                self.send(data)

            def chat(self, body):
                if fixture.terminal:
                    assert "Codex login independently" in json.dumps(body), (
                        "terminal prompt omits provider-independent Codex login"
                    )
                fixture.chat_requests.append(body)
                assert self.headers["Authorization"] == "Bearer " + ACTIVE_KEY
                assert body["model"] == "active-unrelated-model"
                assert not any(t.get("type") == "web_search" for t in body["tools"])
                responses = self.path.endswith("/responses")
                if len(fixture.chat_requests) == 1:
                    name = "terminal" if fixture.terminal else "web_search"
                    args = (
                        {"command": "tny web search '" + QUERY + "'"}
                        if fixture.terminal
                        else {"query": QUERY}
                    )
                    if responses:
                        call = {
                            "type": "function_call",
                            "call_id": "call-search",
                            "name": name,
                            "arguments": json.dumps(args),
                        }
                        self.send(
                            event(
                                {
                                    "type": "response.output_item.done",
                                    "output_index": 0,
                                    "item": call,
                                }
                            )
                            + event(
                                {
                                    "type": "response.completed",
                                    "response": {"output": []},
                                }
                            )
                        )
                    else:
                        delta = {
                            "tool_calls": [
                                {
                                    "index": 0,
                                    "id": "call-search",
                                    "type": "function",
                                    "function": {
                                        "name": name,
                                        "arguments": json.dumps(args),
                                    },
                                }
                            ]
                        }
                        self.send(
                            event(
                                {
                                    "choices": [
                                        {"delta": delta, "finish_reason": "tool_calls"}
                                    ]
                                }
                            )
                            + b"data: [DONE]\n\n"
                        )
                else:
                    assert "Codex web search results" in json.dumps(body)
                    assert "CODEX-SEARCH-RESULT" in json.dumps(body)
                    assert SOURCE in json.dumps(body)
                    fixture.followup_started.set()
                    if fixture.block_followup:
                        fixture.release_followup.wait(15)
                    if responses:
                        self.send(
                            event(
                                {
                                    "type": "response.output_text.delta",
                                    "delta": "CHAT-PROVIDER-UNCHANGED",
                                }
                            )
                            + event(
                                {
                                    "type": "response.completed",
                                    "response": {"output": []},
                                }
                            )
                        )
                    else:
                        self.send(
                            event(
                                {
                                    "choices": [
                                        {
                                            "delta": {
                                                "content": "CHAT-PROVIDER-UNCHANGED"
                                            },
                                            "finish_reason": "stop",
                                        }
                                    ]
                                }
                            )
                            + b"data: [DONE]\n\n"
                        )

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def base(self):
        return f"http://127.0.0.1:{self.server.server_port}"

    def close(self):
        self.release_search.set()
        self.release_followup.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)


class SearchServiceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tny-shared-search-")
        self.home = Path(self.temp.name)
        self.work = self.home / "workspace"
        self.work.mkdir()
        (self.work / "AGENTS.md").write_text("PROJECT-INSTRUCTION-SENTINEL\n")
        self.fixture = Fixture()
        self.addCleanup(self.temp.cleanup)
        self.addCleanup(self.fixture.close)

    def env(self, extra=None):
        return base_env(
            str(self.home),
            {
                "CHATGPT_ACCESS_TOKEN": TOKEN,
                "CHATGPT_ACCOUNT_ID": ACCOUNT,
                "TNY_CODEX_BASE_URL": self.fixture.base + "/codex",
                "TNY_PROVIDER_RETRIES": "0",
                "TNY_EXTENSION_HOST": str(
                    Path(__file__).resolve().parents[2] / "python/tny_extension_host.py"
                ),
                **(extra or {}),
            },
        )

    def settings(self, value):
        p = self.home / ".tny/settings.json"
        p.parent.mkdir(exist_ok=True)
        p.write_text(json.dumps(value))

    def cli(self, *prefix, env=None):
        return subprocess.run(
            [TNY, *prefix, "web", "search", QUERY, "--json"],
            cwd=self.work,
            env=env or self.env(),
            capture_output=True,
            text=True,
            timeout=20,
        )

    def assert_ok(self, result):
        self.assertEqual(
            result.returncode, 0, (result.stdout, result.stderr, self.fixture.errors)
        )
        self.assertFalse(self.fixture.errors)
        value = json.loads(result.stdout)
        self.assertTrue(value["ok"])
        self.assertIn("Codex web search results", value["result"])
        self.assertIn("CODEX-SEARCH-RESULT café", value["result"])
        self.assertIn("<" + SOURCE + ">", value["result"])
        self.assertEqual(value["result"].count("CODEX-SEARCH-RESULT"), 1)
        self.assertNotIn("javascript:", value["result"])
        self.assertFalse(self.fixture.get_requests)

    def test_cli_independent_of_provider_model_and_broken_active_profile(self):
        self.settings({"grok": {"base_url": 42}})
        for provider in (
            "openai",
            "grok",
            "claude",
            "codex",
            "custom",
            "cursor",
            "acp",
        ):
            with self.subTest(provider=provider):
                self.assert_ok(
                    self.cli("--provider", provider, "--model", "unrelated-model")
                )
        self.assertEqual(len(self.fixture.search_requests), 7)
        self.assertFalse(list((self.home / ".tny").glob("sessions/*/*/session.json")))

    def test_split_sse_and_duplicate_terminal_event(self):
        self.fixture.mode = "split"
        self.assert_ok(self.cli())

    def test_missing_content_type_and_whole_json_match_native_backend(self):
        for mode in (
            "missing-content-type",
            "json-response",
            "items-before-empty-terminal",
            "bom-stream",
            "unusual-field",
        ):
            with self.subTest(mode=mode):
                self.fixture.mode = mode
                self.assert_ok(self.cli())

    def test_model_setting_is_independent(self):
        self.fixture.model = "fixture-codex-search-model"
        self.settings({"web_search_model": self.fixture.model})
        self.assert_ok(self.cli("--model", "ignored-active-model"))

    def test_explicit_overrides_do_not_contact_codex(self):
        self.settings({"web_search_url": self.fixture.base + "/configured?q={query}"})
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OVERRIDE-RESULT", result.stdout)
        self.assertFalse(self.fixture.search_requests)
        self.settings(
            {
                "web_search_command": "printf configured-command",
                "web_search_url": self.fixture.base + "/unused",
            }
        )
        result = self.cli()
        if WASM:
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn("not available in wasm", result.stdout)
        else:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("configured-command", result.stdout)
        self.assertEqual(len(self.fixture.get_requests), 1)
        self.assertFalse(self.fixture.search_requests)

    def test_auth_stores_and_flag_precedence(self):
        env = self.env()
        env.pop("CHATGPT_ACCESS_TOKEN")
        env.pop("CHATGPT_ACCOUNT_ID")
        auth = {"tokens": {"access_token": TOKEN, "account_id": ACCOUNT}}
        cli = self.home / ".codex/auth.json"
        cli.parent.mkdir()
        cli.write_text(json.dumps(auth))
        self.assert_ok(self.cli(env=env))
        store = self.home / ".tny/codex-auth.json"
        store.parent.mkdir(exist_ok=True)
        self.fixture.token = "fixture-preferred-tny-token"
        auth["tokens"]["access_token"] = self.fixture.token
        store.write_text(json.dumps(auth))
        self.assert_ok(self.cli(env=env))
        self.fixture.token = TOKEN
        self.assert_ok(self.cli(env=self.env()))
        self.fixture.token = "fixture-explicit-flag-token"
        self.assert_ok(
            self.cli(
                "--chatgpt-token",
                self.fixture.token,
                "--chatgpt-account-id",
                ACCOUNT,
                env=self.env(),
            )
        )

    def test_failed_requests_do_not_fallback_or_echo_secrets(self):
        for mode in (
            "http-401",
            "http-403",
            "http-429",
            "http-500",
            "wrong-type",
            "json-error",
            "failed",
            "incomplete",
            "partial",
            "malformed",
            "no-search",
            "failed-tool",
            "unexpected-tool",
            "oversized",
        ):
            with self.subTest(mode=mode):
                self.fixture.mode = mode
                result = self.cli()
                self.assertEqual(
                    result.returncode, 2, (mode, result.stdout, result.stderr)
                )
                value = json.loads(result.stdout)
                self.assertFalse(value["ok"])
                self.assertTrue(value["result"].startswith("error:"))
                self.assertNotIn(TOKEN, result.stdout + result.stderr)
                self.assertNotIn("UNTRUSTED-PARTIAL", result.stdout)
                self.assertNotIn("DuckDuckGo", result.stdout)
                self.assertFalse(self.fixture.errors)
        self.assertEqual(len(self.fixture.search_requests), 14)
        self.assertFalse(self.fixture.get_requests)
        self.assertFalse((self.work / "unsafe").exists())

    def test_large_repeated_source_list_keeps_success_and_deduplicates(self):
        self.fixture.mode = "many-sources"
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        value = json.loads(result.stdout)
        self.assertTrue(value["ok"])
        self.assertEqual(value["result"].count("<" + SOURCE + ">"), 1)
        # The shared parent result limit may itself truncate; the service
        # must not turn source-list bounds into a failed paid search.
        self.assertIn("CODEX-SEARCH-RESULT", value["result"])

    @unittest.skipIf(WASM, "SSH/command override needs a native process")
    def test_standalone_ssh_command_override_and_local_codex_service(self):
        remote = self.home / "remote"
        remote.mkdir()
        binpath = self.home / "bin"
        binpath.mkdir()
        log = self.home / "ssh-calls.jsonl"
        fake = binpath / "ssh"
        fake.write_text(f"""#!{Path(sys.executable).resolve()}
import json, subprocess, sys
args = sys.argv[1:]
with open({str(log)!r}, "a") as f:
    f.write(json.dumps(args) + "\\n")
if args[-1] == "true" or "exit" in args:
    raise SystemExit(0)
raise SystemExit(subprocess.call(["sh", "-c", args[-1]]))
""")
        fake.chmod(0o755)
        env = self.env({"PATH": str(binpath) + os.pathsep + os.environ["PATH"]})
        self.settings({"web_search_command": "printf remote-search > marker.txt; pwd"})
        prefix = [
            "--provider",
            "cursor",
            "--ssh",
            "user@fixture.test",
            "--ssh-cwd",
            str(remote),
        ]
        result = self.cli(*prefix, env=env)
        self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))
        self.assertTrue((remote / "marker.txt").exists())
        self.assertFalse((self.work / "marker.txt").exists())
        self.assertIn(str(remote), result.stdout)
        self.assertFalse(self.fixture.search_requests)
        self.settings({})
        self.assert_ok(self.cli(*prefix, env=env))
        calls = log.read_text()
        self.assertNotIn(TOKEN, calls)
        self.assertNotIn(ACCOUNT, calls)
        self.assertNotIn("/codex/responses", calls)

    def test_invalid_settings_and_missing_account_fail_before_http(self):
        for options in (
            {"web_search_model": ""},
            {"web_search_model": 42},
            {"web_search_timeout_seconds": 0},
            {"web_search_timeout_seconds": 301},
            {"web_search_timeout_seconds": "1"},
        ):
            self.settings(options)
            result = self.cli()
            self.assertEqual(result.returncode, 2, result.stdout)
        self.settings({})
        env = self.env()
        env.pop("CHATGPT_ACCOUNT_ID")
        self.assertEqual(self.cli(env=env).returncode, 2)
        self.assertFalse(self.fixture.search_requests)

    def test_timeout_headers_and_body(self):
        self.settings({"web_search_timeout_seconds": 1})
        for mode in ("slow-headers", "slow-body"):
            self.fixture.mode = mode
            before = time.monotonic()
            result = self.cli()
            self.assertEqual(result.returncode, 2)
            self.assertIn("timed out", result.stdout)
            self.assertLess(time.monotonic() - before, 3)

    @unittest.skipIf(
        WASM, "native POSIX signal/runner semantics; shared HTTP paths run on wasm"
    )
    def test_cancel_headers_body_and_refresh(self):
        for mode in ("slow-headers", "slow-body", "slow-refresh"):
            with self.subTest(mode=mode):
                self.fixture.mode = mode
                self.fixture.search_started.clear()
                env = self.env()
                if mode == "slow-refresh":
                    env.pop("CHATGPT_ACCESS_TOKEN")
                    auth = self.home / ".tny/codex-auth.json"
                    auth.parent.mkdir(exist_ok=True)
                    auth.write_text(
                        json.dumps(
                            {
                                "tokens": {
                                    "access_token": TOKEN,
                                    "account_id": ACCOUNT,
                                    "refresh_token": "fixture-refresh-token",
                                },
                                "expires_at": "2020-01-01T00:00:00Z",
                            }
                        )
                    )
                    env["CODEX_REFRESH_TOKEN_URL_OVERRIDE"] = (
                        self.fixture.base + "/oauth/token"
                    )
                p = subprocess.Popen(
                    [TNY, "web", "search", QUERY, "--json"],
                    cwd=self.work,
                    env=env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                try:
                    self.assertTrue(self.fixture.search_started.wait(5))
                    before = time.monotonic()
                    p.send_signal(signal.SIGINT)
                    stdout, stderr = p.communicate(timeout=3)
                    self.assertEqual(p.returncode, 130, (stdout, stderr))
                    self.assertIn("interrupted", stdout)
                    self.assertLess(time.monotonic() - before, 2)
                finally:
                    if p.poll() is None:
                        p.kill()
                        p.communicate()

    def test_expired_store_refresh_is_bounded_and_updates_only_auth(self):
        auth = self.home / ".tny/codex-auth.json"
        auth.parent.mkdir()
        auth.write_text(
            json.dumps(
                {
                    "tokens": {
                        "access_token": TOKEN,
                        "account_id": ACCOUNT,
                        "refresh_token": "fixture-refresh-token",
                    },
                    "expires_at": "2020-01-01T00:00:00Z",
                }
            )
        )
        env = self.env()
        env.pop("CHATGPT_ACCESS_TOKEN")
        env["CODEX_REFRESH_TOKEN_URL_OVERRIDE"] = self.fixture.base + "/oauth/token"
        self.assert_ok(self.cli(env=env))
        self.assertEqual(len(self.fixture.oauth_requests), 1)
        self.assertEqual(
            json.loads(auth.read_text())["tokens"]["access_token"], self.fixture.token
        )
        self.assertEqual(auth.stat().st_mode & 0o777, 0o600)
        self.assertFalse(list((self.home / ".tny").glob("sessions/*/*/session.json")))

    def active_env(self, provider, wire="chat"):
        return self.env(
            {
                provider.upper() + "_BASE_URL": self.fixture.base + "/active/v1",
                provider.upper() + "_API_KEY": ACTIVE_KEY,
                "TNY_TOOLS": "terminal" if self.fixture.terminal else "all",
                "OPENAI_WIRE_API": wire,
            }
        )

    @unittest.skipIf(
        WASM,
        "wasm has no tool profiles: `terminal` is ignored and the prompt is `all` (ADR 0017)",
    )
    def test_terminal_prompt_advertises_provider_independent_login(self):
        self.fixture.terminal = True
        result = subprocess.run(
            [
                TNY,
                "--provider",
                "openai",
                "--model",
                "active-unrelated-model",
                "--wire-api",
                "chat",
                "--no-extensions",
                "ask",
                "--json",
                "Search now",
            ],
            cwd=self.work,
            env=self.active_env("openai"),
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(
            result.returncode, 0, (result.stdout, result.stderr, self.fixture.errors)
        )
        self.assertEqual(json.loads(result.stdout)["output"], "CHAT-PROVIDER-UNCHANGED")
        self.assertFalse(self.fixture.errors)

    def test_other_provider_roundtrip_keeps_model_and_cited_tool_result(self):
        for wire in ("chat", "responses"):
            for provider in ("openai", "grok", "claude", "codex", "custom"):
                with self.subTest(provider=provider, wire=wire):
                    self.fixture.chat_requests.clear()
                    p = subprocess.run(
                        [
                            TNY,
                            "--provider",
                            provider,
                            "--model",
                            "active-unrelated-model",
                            "--wire-api",
                            wire,
                            "--no-extensions",
                            "ask",
                            "--json",
                            "Search using the web_search tool",
                        ],
                        cwd=self.work,
                        env=self.active_env(provider, wire),
                        capture_output=True,
                        text=True,
                        timeout=15,
                    )
                    self.assertEqual(
                        p.returncode, 0, (p.stdout, p.stderr, self.fixture.errors)
                    )
                    answer = json.loads(p.stdout)
                    self.assertEqual(answer["provider"], provider)
                    self.assertEqual(answer["model"], "active-unrelated-model")
                    self.assertEqual(answer["output"], "CHAT-PROVIDER-UNCHANGED")
                    self.assertEqual(len(self.fixture.chat_requests), 2)
                    self.assertFalse(self.fixture.errors)
        self.assertEqual(len(self.fixture.search_requests), 10)

    @unittest.skipIf(
        WASM, "native POSIX signal/runner semantics; shared HTTP paths run on wasm"
    )
    def test_runner_cancellation_during_search_and_refresh_in_both_profiles(self):
        for terminal in (False, True):
            for mode in ("slow-headers", "slow-refresh"):
                with self.subTest(terminal=terminal, mode=mode):
                    self.fixture.terminal = terminal
                    self.fixture.mode = mode
                    self.fixture.search_started.clear()
                    self.fixture.release_search.clear()
                    self.fixture.chat_requests.clear()
                    self.fixture.search_requests.clear()
                    self.fixture.token = TOKEN
                    env = self.active_env("openai")
                    if mode == "slow-refresh":
                        env.pop("CHATGPT_ACCESS_TOKEN")
                        auth = self.home / ".tny/codex-auth.json"
                        auth.parent.mkdir(exist_ok=True)
                        auth.write_text(
                            json.dumps(
                                {
                                    "tokens": {
                                        "access_token": TOKEN,
                                        "account_id": ACCOUNT,
                                        "refresh_token": "fixture-refresh-token",
                                    },
                                    "expires_at": "2020-01-01T00:00:00Z",
                                }
                            )
                        )
                        env["CODEX_REFRESH_TOKEN_URL_OVERRIDE"] = (
                            self.fixture.base + "/oauth/token"
                        )
                    term = Term(
                        [
                            TNY,
                            "--provider",
                            "openai",
                            "--model",
                            "active-unrelated-model",
                            "--wire-api",
                            "chat",
                            "--no-extensions",
                        ],
                        env,
                        str(self.work),
                    )
                    state = None
                    try:
                        term.expect("/help", 10)
                        term.send("Search using the available tool\r")
                        until = time.monotonic() + 10
                        while (
                            not self.fixture.search_started.is_set()
                            and time.monotonic() < until
                        ):
                            term.pump(0.05)
                        self.assertTrue(
                            self.fixture.search_started.is_set(), term.screen()
                        )
                        state = max(
                            (self.home / ".tny").glob("sessions/*/*/session.json"),
                            key=lambda p: p.stat().st_mtime_ns,
                        )
                        old_pid = (state.parent / "pid").read_text()
                        term.send("\x1b[D")
                        term.expect("Agents — all saved sessions", 5)
                        self.assertEqual((state.parent / "pid").read_text(), old_pid)
                        self.assertTrue(json.loads(state.read_text()).get("background"))
                        attach_dashboard_session(term, state.parent.name)
                        started = time.monotonic()
                        term.send("\x03")
                        term.expect("interrupted", 5)
                        self.assertLess(time.monotonic() - started, 3)
                        term.pump(0.2)
                        self.assertEqual((state.parent / "pid").read_text(), old_pid)
                        self.assertEqual(
                            json.loads(state.read_text())["status"], "interrupted"
                        )
                        self.assertTrue(json.loads(state.read_text()).get("background"))
                        self.assertEqual(len(self.fixture.chat_requests), 1)
                        self.assertEqual(
                            len(self.fixture.search_requests),
                            0 if mode == "slow-refresh" else 1,
                        )
                        self.assertFalse(self.fixture.errors)
                        term.send("/quit\r")
                        self.assertEqual(term.wait(), 0)
                    finally:
                        self.fixture.release_search.set()
                        term.close()
                        if state:
                            subprocess.run(
                                [TNY, "session", "stop", state.parent.name],
                                cwd=self.work,
                                env=env,
                                capture_output=True,
                                timeout=10,
                            )

    @unittest.skipIf(
        WASM, "native POSIX signal/runner semantics; shared HTTP paths run on wasm"
    )
    def test_search_is_saved_after_immediate_background_in_both_tool_profiles(self):
        for terminal in (False, True):
            with self.subTest(terminal=terminal):
                self.fixture.terminal = terminal
                self.fixture.mode = "slow-headers"
                self.fixture.block_followup = True
                self.fixture.search_started.clear()
                self.fixture.followup_started.clear()
                self.fixture.release_search.clear()
                self.fixture.release_followup.clear()
                self.fixture.chat_requests.clear()
                self.fixture.search_requests.clear()
                env = self.active_env("openai")
                term = Term(
                    [
                        TNY,
                        "--provider",
                        "openai",
                        "--model",
                        "active-unrelated-model",
                        "--wire-api",
                        "chat",
                        "--no-extensions",
                    ],
                    env,
                    str(self.work),
                )
                state = None
                try:
                    term.expect("/help", 10)
                    term.send("Search using the available tool\r")
                    until = time.monotonic() + 10
                    while (
                        not self.fixture.search_started.is_set()
                        and time.monotonic() < until
                    ):
                        term.pump(0.05)
                    self.assertTrue(self.fixture.search_started.is_set(), term.screen())
                    candidates = list(
                        (self.home / ".tny").glob("sessions/*/*/session.json")
                    )
                    state = max(candidates, key=lambda p: p.stat().st_mtime_ns)
                    old_pid = (state.parent / "pid").read_text()
                    term.send("\x1b[D")
                    term.expect("Agents — all saved sessions", 5)
                    self.assertEqual((state.parent / "pid").read_text(), old_pid)
                    self.assertTrue(json.loads(state.read_text()).get("background"))
                    self.fixture.release_search.set()
                    self.assertTrue(self.fixture.followup_started.wait(5))
                    self.assertEqual((state.parent / "pid").read_text(), old_pid)
                    self.assertEqual(len(self.fixture.search_requests), 1)
                    attach_dashboard_session(term, state.parent.name)
                    self.fixture.release_followup.set()
                    term.expect("CHAT-PROVIDER-UNCHANGED", 10)
                    term.pump(0.3)
                    saved = json.loads(state.read_text())
                    self.assertEqual(saved["turns"], 1)
                    self.assertEqual(
                        sum(m.get("role") == "user" for m in saved["messages"]), 1
                    )
                    results = [m for m in saved["messages"] if m.get("role") == "tool"]
                    self.assertEqual(len(results), 1)
                    self.assertIn("Codex web search results", results[0]["content"])
                    self.assertFalse(self.fixture.errors)
                    term.send("/quit\r")
                    self.assertEqual(term.wait(), 0)
                finally:
                    self.fixture.release_search.set()
                    self.fixture.release_followup.set()
                    term.close()
                    if state:
                        subprocess.run(
                            [TNY, "session", "stop", state.parent.name],
                            cwd=self.work,
                            env=env,
                            capture_output=True,
                            timeout=10,
                        )


if __name__ == "__main__":
    unittest.main(argv=[__file__], verbosity=2)
