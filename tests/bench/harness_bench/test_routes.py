"""Offline route, credential boundary, and accounting negative controls."""

import io
import json
import tempfile
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from unittest.mock import patch

from adapters import invocation
from cost import request_cost
from proxy import RecordingProxy, _NoRedirect, record_chat_event, sections
from routes import ROUTES


class RoutesTest(unittest.TestCase):
    def test_fixed_origins_and_wire_rejection(self):
        for name, route in ROUTES.items():
            body = {
                "model": "mercury-3",
                "stream": True,
                "store": False,
                "messages" if route.wire == "chat/completions" else "input": [],
            }
            route.validate(body, "mercury-3")
            with self.assertRaises(ValueError):
                route.validate(body, "inception/mercury-3")
            with self.assertRaises(ValueError):
                route.validate({"model": "mercury-3", "stream": True}, "mercury-3")
            with self.assertRaises(ValueError):
                route.validate(dict(body, stream=False), "mercury-3")
            if name == "openai-siwc":
                with self.assertRaises(ValueError):
                    route.validate(dict(body, store=True))
            self.assertEqual(
                route.endpoint.split("/")[2],
                "chatgpt.com"
                if name == "legacy-codex"
                else "api.inceptionlabs.ai"
                if name == "inception-chat"
                else "api.openai.com",
            )

    def test_no_redirect_and_no_credential_type_fallback(self):
        handler = _NoRedirect()
        request = urllib.request.Request("https://api.openai.com/v1/responses")
        self.assertIsNone(
            handler.redirect_request(
                request, None, 307, "", {}, "https://elsewhere.test/"
            )
        )
        with tempfile.TemporaryDirectory() as temp:
            auth = Path(temp) / "auth.json"
            for record in (
                {"tokens": {"access_token": "secret"}},
                {"issuer": "https://wrong.test", "plan_usage": True},
            ):
                auth.write_text(json.dumps(record))
                with self.assertRaises(ValueError):
                    ROUTES["openai-siwc"].headers(auth)
            with patch.dict("os.environ", {}, clear=True):
                with self.assertRaises(ValueError):
                    ROUTES["inception-chat"].headers(auth)

    def test_siwc_never_copies_real_credentials_to_adapter(self):
        with (
            tempfile.TemporaryDirectory() as temp,
            patch.dict("os.environ", {"OPENAI_API_KEY": "real-secret"}),
        ):
            call = invocation(
                "tny",
                temp,
                "http://127.0.0.1:1234",
                "hi",
                "model",
                "low",
                "/bin/true",
                "openai-siwc",
            )
            self.assertNotIn("OPENAI_API_KEY", call.env)
            self.assertNotIn("codex", call.command)
            self.assertIn("openai", call.command)
            self.assertNotIn("real-secret", repr(call))
            for file in Path(temp).rglob("*.json"):
                self.assertNotIn("real-secret", file.read_text())
            with self.assertRaises(ValueError):
                invocation(
                    "codex",
                    temp,
                    "http://127.0.0.1:1234",
                    "hi",
                    "model",
                    "low",
                    "/bin/true",
                    "openai-siwc",
                )

    def test_chat_usage_and_tool_fragments_preserve_unknown(self):
        row, calls = {}, {}
        record_chat_event(
            {
                "choices": [
                    {
                        "delta": {
                            "tool_calls": [{"index": 0, "function": {"name": "run_"}}]
                        }
                    }
                ]
            },
            row,
            calls,
        )
        record_chat_event(
            {
                "choices": [
                    {
                        "delta": {
                            "tool_calls": [{"index": 0, "function": {"name": "code"}}]
                        },
                        "finish_reason": "tool_calls",
                    }
                ]
            },
            row,
            calls,
        )
        record_chat_event(
            {"choices": [], "usage": {"prompt_tokens": 10, "completion_tokens": 3}},
            row,
            calls,
        )
        self.assertIsNone(row["cached_input_tokens"])
        self.assertIsNone(row["cache_write_tokens"])
        self.assertEqual(
            row["output_items"], [{"type": "function_call", "name": "run_code"}]
        )
        self.assertEqual(request_cost(row, "mercury-3"), (None, None, None))
        row["cached_input_tokens"] = 0
        self.assertEqual(request_cost(row, "mercury-3"), (None, None, 10))
        data = sections(
            {
                "messages": [
                    {"role": "system", "content": "stable"},
                    {"role": "tool", "content": "result"},
                ]
            }
        )
        self.assertEqual(data["instructions_chars"], 6)
        self.assertEqual(data["tool_output_chars"], [6])

    def test_proxy_routes_body_unchanged_and_rejects_mismatch_before_auth(self):
        class Response(io.BytesIO):
            status = 200
            headers = {"Content-Type": "text/event-stream"}

        payload = b'data: {"choices":[],"usage":{"prompt_tokens":10,"completion_tokens":2}}\n\ndata: [DONE]\n\n'
        body = {
            "model": "mercury-3",
            "stream": True,
            "messages": [{"role": "user", "content": "fixture"}],
        }
        raw = json.dumps(body).encode()
        with (
            tempfile.TemporaryDirectory() as temp,
            patch.dict("os.environ", {"INCEPTION_API_KEY": "test-secret"}),
            patch("proxy.open_upstream", return_value=Response(payload)) as opened,
        ):
            with RecordingProxy(temp, None, "inception-chat", "mercury-3") as proxy:
                request = urllib.request.Request(
                    proxy.base_url + "/v1/chat/completions",
                    data=raw,
                    headers={"Authorization": "Bearer client-fixture"},
                )
                with urllib.request.urlopen(request) as response:
                    self.assertEqual(response.read(), payload)
                for path in ("/v1/responses", "/v1/chat/completions?escape=1"):
                    with self.assertRaises(urllib.error.HTTPError):
                        urllib.request.urlopen(
                            urllib.request.Request(proxy.base_url + path, data=raw)
                        )
                wrong = json.dumps(dict(body, model="inception/mercury-3")).encode()
                with self.assertRaises(urllib.error.HTTPError):
                    urllib.request.urlopen(
                        urllib.request.Request(
                            proxy.base_url + "/v1/chat/completions", data=wrong
                        )
                    )
            self.assertEqual(opened.call_count, 1)
            sent = opened.call_args.args[0]
            self.assertEqual(sent.full_url, ROUTES["inception-chat"].endpoint)
            self.assertEqual(sent.data, raw)
            self.assertEqual(sent.get_header("Authorization"), "Bearer test-secret")
            self.assertIsNone(sent.get_header("Chatgpt-account-id"))
            for file in Path(temp).glob("*.jsonl"):
                self.assertNotIn("test-secret", file.read_text())

    def test_expired_siwc_rejected_and_headers_are_origin_specific(self):
        with tempfile.TemporaryDirectory() as temp:
            auth = Path(temp) / "auth.json"
            record = {
                "issuer": "https://auth.openai.com",
                "plan_usage": True,
                "token_type": "Bearer",
                "scopes": ["chatgpt.tokens.use.direct"],
                "access_token": "fixture",
                "expires_at": "2000-01-01T00:00:00Z",
                "account_id": "not-forwarded",
            }
            auth.write_text(json.dumps(record))
            with self.assertRaises(ValueError):
                ROUTES["openai-siwc"].headers(auth)
            record["expires_at"] = "2999-01-01T00:00:00Z"
            auth.write_text(json.dumps(record))
            headers = ROUTES["openai-siwc"].headers(auth)
            self.assertEqual(headers["Authorization"], "Bearer fixture")
            self.assertNotIn("chatgpt-account-id", headers)

    def test_upstream_error_details_never_reach_recording_or_client(self):
        body = json.dumps(
            {"model": "mercury-3", "stream": True, "messages": []}
        ).encode()
        failure = urllib.error.HTTPError(
            "https://secret-in-url.test",
            401,
            "secret-in-message",
            {},
            io.BytesIO(b"secret-in-body"),
        )
        with (
            tempfile.TemporaryDirectory() as temp,
            patch.dict("os.environ", {"INCEPTION_API_KEY": "test-secret"}),
            patch("proxy.open_upstream", side_effect=failure),
        ):
            with RecordingProxy(temp, None, "inception-chat", "mercury-3") as proxy:
                self.assertEqual(proxy.server.server_address[0], "127.0.0.1")
                with self.assertRaises(urllib.error.HTTPError) as caught:
                    urllib.request.urlopen(
                        urllib.request.Request(
                            proxy.base_url + "/v1/chat/completions", data=body
                        )
                    )
                self.assertEqual(caught.exception.read(), b"")
            text = (Path(temp) / "requests.jsonl").read_text()
            self.assertNotIn("secret", text)
            row = json.loads(text)
            self.assertEqual(row["http_status"], 401)
            self.assertIsNone(row["input_tokens"])
            self.assertGreaterEqual(row["elapsed_ms"], 0)

    def test_malformed_siwc_stores_fail_with_value_error(self):
        with tempfile.TemporaryDirectory() as temp:
            auth = Path(temp) / "auth.json"
            good = {
                "issuer": "https://auth.openai.com",
                "token_type": "Bearer",
                "plan_usage": True,
                "scopes": ["chatgpt.tokens.use.direct"],
                "access_token": "fixture",
                "expires_at": "2999-01-01T00:00:00Z",
            }
            bad = [
                [],
                None,
                {},
                dict(good, scopes="chatgpt.tokens.use.direct"),
                dict(good, expires_at=None),
                dict(good, expires_at="2999-01-01"),
                dict(good, token_type="Other"),
                dict(good, access_token=[]),
            ]
            for record in bad:
                auth.write_text(json.dumps(record))
                with self.subTest(record=record), self.assertRaises(ValueError):
                    ROUTES["openai-siwc"].headers(auth)

    def test_chat_truncation_and_failure_are_not_completed(self):
        class Response(io.BytesIO):
            status = 200
            headers = {"Content-Type": "text/event-stream"}

        body = json.dumps(
            {"model": "mercury-3", "stream": True, "messages": []}
        ).encode()
        for finish, done, expected in (
            ("stop", True, True),
            ("stop", False, False),
            ("length", True, False),
            ("content_filter", True, False),
        ):
            payload = (
                b"data: "
                + json.dumps(
                    {
                        "choices": [{"index": 0, "delta": {}, "finish_reason": finish}],
                        "usage": {"prompt_tokens": 10, "completion_tokens": 1},
                    }
                ).encode()
                + b"\n\n"
            )
            # A repeated usage frame is a replacement, not an extra billed request.
            payload += b'data: {"choices":[],"usage":{"prompt_tokens":10,"completion_tokens":1}}\n\n'
            if done:
                payload += b"data: [DONE]\n\n"
            with (
                tempfile.TemporaryDirectory() as temp,
                patch.dict("os.environ", {"INCEPTION_API_KEY": "fixture"}),
                patch("proxy.open_upstream", return_value=Response(payload)),
            ):
                with RecordingProxy(temp, None, "inception-chat", "mercury-3") as proxy:
                    with urllib.request.urlopen(
                        urllib.request.Request(
                            proxy.base_url + "/v1/chat/completions", data=body
                        )
                    ) as response:
                        response.read()
                row = json.loads((Path(temp) / "requests.jsonl").read_text())
                self.assertEqual(row["stream_complete"], expected)
                self.assertEqual(row["input_tokens"], 10)

    def test_plan_only_never_loads_credentials_or_runs_agent(self):
        import sys
        from contextlib import redirect_stdout

        from run import main

        with (
            tempfile.TemporaryDirectory() as temp,
            patch.object(
                sys,
                "argv",
                [
                    "run.py",
                    "--plan-only",
                    "--route",
                    "inception-chat",
                    "--model",
                    "mercury-3",
                    "--effort",
                    "omit",
                    "--out",
                    temp,
                    "--tasks-dir",
                    str(Path(__file__).parent / "tasks-smoke"),
                    "--task",
                    "smoke-hello",
                ],
            ),
            patch.object(
                type(ROUTES["openai-siwc"]),
                "headers",
                side_effect=AssertionError("must not read auth"),
            ),
            patch("run.run_one", side_effect=AssertionError("must not run agent")),
            redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(main(), 0)
            manifest = json.loads((Path(temp) / "baseline/manifest.json").read_text())
            self.assertEqual(manifest["route"], "inception-chat")
            self.assertEqual(len(manifest["tasks_sha256"]["smoke-hello"]), 64)
