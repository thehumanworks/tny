#!/usr/bin/env python3
"""Exclusive native code surface and nested web search; synthetic local fixtures."""

import json
import os
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from test_tui import TNY, base_env

TNY = os.path.abspath(TNY)


def unique_object(pairs):
    """Reject duplicate request members before schema checks can hide them."""
    result = {}
    for key, value in pairs:
        assert key not in result, f"duplicate request JSON key: {key}"
        result[key] = value
    return result


def run_case(mode, unsolicited=False):
    errors, requests, searches = [], [], []
    search = {
        "type": "web_search_call",
        "id": "search-fixture",
        "status": "completed",
        "action": {"type": "search", "query": "fixture"},
    }
    message = {
        "type": "message",
        "id": "search-answer",
        "status": "completed",
        "role": "assistant",
        "content": [{"type": "output_text", "text": "NESTED-SEARCH-RESULT"}],
    }

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            try:
                body = json.loads(
                    self.rfile.read(int(self.headers["Content-Length"])),
                    object_pairs_hook=unique_object,
                )
                dedicated = body.get("tool_choice") == "required"
                if dedicated:
                    searches.append(body)
                    assert not unsolicited
                    assert body["tools"] == [
                        {"type": "web_search", "external_web_access": True}
                    ], body
                    assert "code-surface-conversation" not in json.dumps(body)
                else:
                    requests.append(body)
                    assert len(body["tools"]) == 1, body["tools"]
                    assert body["tools"][0]["type"] == "function", body["tools"]
                    assert body["tools"][0]["name"] == "run_code", body["tools"]
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()

                def event(value):
                    data = ("data: " + json.dumps(value) + "\n\n").encode()
                    for at in range(0, len(data), 7):
                        self.wfile.write(data[at : at + 7])
                        self.wfile.flush()

                if dedicated:
                    event({"type": "response.output_item.done", "item": search})
                    event(
                        {
                            "type": "response.output_text.delta",
                            "delta": "NESTED-SEARCH-RESULT",
                        }
                    )
                    event(
                        {
                            "type": "response.completed",
                            "response": {
                                "status": "completed",
                                "output": [search, message],
                            },
                        }
                    )
                elif unsolicited:
                    if mode == "builtin":
                        event({"type": "response.output_item.added", "item": search})
                    event(
                        {"type": "response.completed", "response": {"output": [search]}}
                    )
                elif len(requests) == 1:
                    code = (
                        'assert(string.find(tools.describe("web_search"), "web_search", 1, true)); '
                        'print(tools.call("web_search", json.encode({query="fixture"})))'
                    )
                    call = {
                        "type": "function_call",
                        "id": "function-fixture",
                        "call_id": "nested-search",
                        "name": "run_code",
                        "arguments": json.dumps({"code": code, "timeout_ms": 10000}),
                    }
                    event(
                        {
                            "type": "response.output_item.added",
                            "output_index": 0,
                            "item": call,
                        }
                    )
                    event(
                        {
                            "type": "response.output_item.done",
                            "output_index": 0,
                            "item": call,
                        }
                    )
                    event(
                        {"type": "response.completed", "response": {"output": [call]}}
                    )
                else:
                    assert len(requests) == 2, requests
                    results = [
                        v
                        for v in body["input"]
                        if v.get("type") == "function_call_output"
                    ]
                    assert (
                        len(results) == 1 and results[0]["call_id"] == "nested-search"
                    ), body
                    expected = (
                        "configured:fixture"
                        if mode == "override"
                        else "NESTED-SEARCH-RESULT"
                    )
                    assert expected in results[0]["output"], results
                    assert not any(
                        v.get("type") == "web_search_call" for v in body["input"]
                    ), body
                    event(
                        {"type": "response.output_text.delta", "delta": "FOLLOWUP-OK"}
                    )
                    event({"type": "response.completed", "response": {"output": []}})
            except (BrokenPipeError, ConnectionResetError):
                if not unsolicited:
                    errors.append("unexpected provider disconnect")
            except Exception as exc:
                errors.append(repr(exc))

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="tny-native-search-") as home:
            ws = Path(home) / "ws"
            ws.mkdir()
            url = f"http://127.0.0.1:{server.server_port}/v1"
            env = base_env(
                home,
                {
                    "CHATGPT_ACCESS_TOKEN": "fixture-not-a-key",
                    "CHATGPT_ACCOUNT_ID": "fixture-account",
                    "TNY_CODEX_BASE_URL": url,
                    "TNY_PROVIDER_RETRIES": "0",
                    "TNY_TOOLS": "all",
                },
            )
            if mode == "shadowed":
                env["CODEX_BASE_URL"] = url
                env["CODEX_API_KEY"] = "fixture-not-a-key"
            if mode == "override":
                settings = Path(home) / ".tny/settings.json"
                settings.parent.mkdir(exist_ok=True)
                settings.write_text(
                    json.dumps({"web_search_command": "printf 'configured:%s' {query}"})
                )
            if mode == "ordinary":
                env["OPENAI_BASE_URL"] = url
                env["OPENAI_API_KEY"] = "fixture-not-a-key"
                env["OPENAI_WIRE_API"] = "responses"
            result = subprocess.run(
                [
                    TNY,
                    "--no-extensions",
                    "--provider",
                    "openai" if mode == "ordinary" else "codex",
                    "ask",
                    "--json",
                    "code-surface-conversation",
                ],
                env=env,
                cwd=ws,
                capture_output=True,
                text=True,
                timeout=25,
            )
            assert not errors, errors
            if unsolicited:
                assert result.returncode != 0, (result.stdout, result.stderr)
                assert len(requests) == 1 and not searches
            else:
                assert result.returncode == 0, (result.stdout, result.stderr, errors)
                output = json.loads(result.stdout)
                assert "FOLLOWUP-OK" in output["output"], output
                assert len(requests) == 2
                assert len(searches) == (0 if mode == "override" else 1), searches
            session = next((Path(home) / ".tny/sessions").glob("*/*/session.json"))
            saved = json.loads(session.read_text())
            assert not any(
                item.get("type") == "web_search_call"
                for entry in saved["messages"]
                for item in entry.get("responses_items", [])
            ), saved
    finally:
        server.shutdown()
        server.server_close()
    print(
        "PASS native search",
        mode,
        "hosted rejection" if unsolicited else "nested execution",
    )


def fetch_compatibility():
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            data = (
                b"FETCH-NOT-FOUND" if self.path == "/missing" else b"A" * (1536 * 1024)
            )
            self.send_response(404 if self.path == "/missing" else 200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="tny-web-fetch-") as home:
            env = base_env(home, {})
            for path, expected in (
                ("missing", "FETCH-NOT-FOUND"),
                ("large", "HTTP 200"),
            ):
                result = subprocess.run(
                    [
                        TNY,
                        "web",
                        "fetch",
                        f"http://127.0.0.1:{server.server_port}/{path}",
                        "--json",
                    ],
                    env=env,
                    cwd=home,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                assert result.returncode == 0, (result.stdout, result.stderr)
                value = json.loads(result.stdout)
                assert value["ok"] and expected in value["result"], value
                assert len(value["result"]) < 40000, len(value["result"])
    finally:
        server.shutdown()
        server.server_close()
    print("PASS web_fetch preserves non-2xx bodies and bounded oversized pages")


if __name__ == "__main__":
    for case in ("builtin", "shadowed", "override", "ordinary"):
        run_case(case)
    for case in ("builtin", "ordinary"):
        run_case(case, unsolicited=True)
    fetch_compatibility()
