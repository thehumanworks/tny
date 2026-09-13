#!/usr/bin/env python3
"""Hosted search request/persistence/split-SSE regressions; no live credentials."""

import json
import os
import shlex
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from test_tui import TNY, base_env

TNY = os.path.abspath(TNY)


def unique_object(pairs):
    """Reject duplicate members before provider-schema validation can hide them."""
    result = {}
    for key, value in pairs:
        assert key not in result, f"duplicate request JSON key: {key}"
        result[key] = value
    return result


def run_case(mode):
    errors = []
    requests = []
    search = {
        "type": "web_search_call",
        "id": "ws_1",
        "status": "completed",
        "action": {"type": "search", "query": "fixture"},
    }
    annotation = {
        "type": "url_citation",
        "start_index": 0,
        "end_index": 8,
        "url": "https://example.com/source",
        "title": "Fixture source",
    }
    message = {
        "type": "message",
        "id": "msg_1",
        "status": "completed",
        "role": "assistant",
        "content": [
            {
                "type": "output_text",
                "text": "ANSWER-SEARCH",
                "annotations": [annotation],
            }
        ],
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
                requests.append(body)
                native = [t for t in body["tools"] if t["type"] == "web_search"]
                functions = [t for t in body["tools"] if t.get("name") == "web_search"]
                if mode == "builtin":
                    assert native == [
                        {"type": "web_search", "external_web_access": True}
                    ], body["tools"]
                    assert not functions
                    assert self.headers["OpenAI-Beta"] == "responses=v1"
                else:
                    assert not native and len(functions) == 1, body["tools"]
                for tool in body["tools"]:
                    if tool.get("name") == "image_preview":
                        assert all(
                            v.get("type") == "object"
                            for v in tool["parameters"]["oneOf"]
                        ), tool
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()

                def event(value):
                    data = ("data: " + json.dumps(value) + "\n\n").encode()
                    # Split JSON, event lines and UTF-8 independently of reads.
                    for at in range(0, len(data), 7):
                        self.wfile.write(data[at : at + 7])
                        self.wfile.flush()

                if len(requests) == 1 and mode in ("builtin", "ordinary"):
                    if mode == "ordinary":
                        event(
                            {
                                "type": "response.output_text.delta",
                                "delta": "ANSWER-SEARCH",
                            }
                        )
                        event({"type": "response.output_item.done", "item": message})
                        event(
                            {
                                "type": "response.completed",
                                "response": {"output": [message]},
                            }
                        )
                        return
                    event(
                        {
                            "type": "response.output_item.added",
                            "output_index": 0,
                            "item": {**search, "status": "in_progress"},
                        }
                    )
                    event(
                        {
                            "type": "response.web_search_call.searching",
                            "item_id": "ws_1",
                            "output_index": 0,
                        }
                    )
                    event(
                        {
                            "type": "response.output_item.done",
                            "output_index": 0,
                            "item": search,
                        }
                    )
                    event(
                        {
                            "type": "response.output_item.done",
                            "output_index": 0,
                            "item": search,
                        }
                    )
                    event(
                        {"type": "response.output_text.delta", "delta": "ANSWER-SEARCH"}
                    )
                    event(
                        {
                            "type": "response.output_item.done",
                            "output_index": 1,
                            "item": message,
                        }
                    )
                    event(
                        {
                            "type": "response.completed",
                            "response": {"output": [search, message]},
                        }
                    )
                else:
                    if mode == "builtin":
                        assert body["input"].count(search) == 1, body["input"]
                        assert body["input"].count(message) == 1, body["input"]
                        assert not any(
                            v.get("type") == "function_call_output"
                            for v in body["input"]
                        )
                    if mode == "ordinary":
                        assistants = [
                            v for v in body["input"] if v.get("role") == "assistant"
                        ]
                        assert len(assistants) == 1 and "id" not in assistants[0], body[
                            "input"
                        ]
                        assert "annotations" not in json.dumps(assistants), assistants
                    event(
                        {"type": "response.output_text.delta", "delta": "FOLLOWUP-OK"}
                    )
                    event({"type": "response.completed", "response": {"output": []}})
            except Exception as exc:
                errors.append(repr(exc))

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="tny-hosted-search-") as home:
            ws = Path(home) / "ws"
            ws.mkdir()
            url = f"http://127.0.0.1:{server.server_port}/v1"
            env = base_env(
                home,
                {
                    "CHATGPT_ACCESS_TOKEN": "fixture-not-a-key",
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
                settings.parent.mkdir()
                settings.write_text(
                    json.dumps({"web_search_command": "printf 'configured:%s' {query}"})
                )
            if mode == "ordinary":
                env["OPENAI_BASE_URL"] = url
                env["OPENAI_API_KEY"] = "fixture-not-a-key"
                env["OPENAI_WIRE_API"] = "responses"
            argv = [
                TNY,
                "--no-extensions",
                "--provider",
                "openai" if mode == "ordinary" else "codex",
                "ask",
                "--json",
            ]
            result = subprocess.run(
                [*argv, "search fixture"],
                env=env,
                cwd=ws,
                capture_output=True,
                text=True,
                timeout=20,
            )
            assert result.returncode == 0, (result.stdout, result.stderr, errors)
            output = json.loads(result.stdout)
            if mode == "builtin":
                assert output["tool_calls"] == [], output
                assert (
                    output["output"].count(
                        "[Fixture source](https://example.com/source)"
                    )
                    == 1
                ), output
                session = next((Path(home) / ".tny/sessions").glob("*/*/session.json"))
                saved = json.loads(session.read_text())
                assert saved["messages"][-1]["responses_items"] == [search, message], (
                    saved
                )
                result = subprocess.run(
                    [*argv, "--resume", output["session_id"], "follow"],
                    env=env,
                    cwd=ws,
                    capture_output=True,
                    text=True,
                    timeout=20,
                )
                assert result.returncode == 0 and "FOLLOWUP-OK" in result.stdout, (
                    result.stdout,
                    result.stderr,
                    errors,
                )
            if mode == "ordinary":
                session = next((Path(home) / ".tny/sessions").glob("*/*/session.json"))
                assert "responses_items" not in session.read_text()
                result = subprocess.run(
                    [*argv, "--resume", output["session_id"], "follow"],
                    env=env,
                    cwd=ws,
                    capture_output=True,
                    text=True,
                    timeout=20,
                )
                assert result.returncode == 0 and "FOLLOWUP-OK" in result.stdout, (
                    result.stdout,
                    result.stderr,
                    errors,
                )
            assert not errors, errors
    finally:
        server.shutdown()
        server.server_close()
    print("PASS native search", mode)


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


def hosted_boundary():
    from test_background_agents import until
    from test_tui import BANNER, Term

    started, release, finished = threading.Event(), threading.Event(), threading.Event()
    requests, errors = [], []
    search = {
        "type": "web_search_call",
        "id": "hosted-boundary",
        "status": "completed",
        "action": {"type": "search", "query": "fixture"},
    }
    message = {
        "type": "message",
        "id": "cited",
        "role": "assistant",
        "status": "completed",
        "content": [
            {
                "type": "output_text",
                "text": "FOUND",
                "annotations": [
                    {
                        "type": "url_citation",
                        "url": "https://example.com/source",
                        "title": "Source",
                        "start_index": 0,
                        "end_index": 5,
                    }
                ],
            }
        ],
    }
    call = {
        "type": "function_call",
        "id": "fc_1",
        "call_id": "pending",
        "name": "terminal",
        "arguments": json.dumps(
            {
                "command": 'cat "${TNY_SESSION_SOCK%/sock}/pid" > execution-pid; printf once >> effects'
            }
        ),
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
                requests.append(body)
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()

                def event(value):
                    self.wfile.write(("data: " + json.dumps(value) + "\n\n").encode())
                    self.wfile.flush()

                if len(requests) == 1:
                    event(
                        {
                            "type": "response.output_item.added",
                            "output_index": 0,
                            "item": {**search, "status": "in_progress"},
                        }
                    )
                    started.set()
                    assert release.wait(20)
                    event(
                        {
                            "type": "response.output_item.done",
                            "output_index": 0,
                            "item": search,
                        }
                    )
                    event({"type": "response.output_text.delta", "delta": "FOUND"})
                    event(
                        {
                            "type": "response.output_item.done",
                            "output_index": 1,
                            "item": message,
                        }
                    )
                    event(
                        {
                            "type": "response.output_item.added",
                            "output_index": 2,
                            "item": {**call, "arguments": ""},
                        }
                    )
                    event(
                        {
                            "type": "response.function_call_arguments.delta",
                            "item_id": "fc_1",
                            "output_index": 2,
                            "delta": call["arguments"],
                        }
                    )
                    event(
                        {
                            "type": "response.output_item.done",
                            "output_index": 2,
                            "item": call,
                        }
                    )
                    event(
                        {
                            "type": "response.completed",
                            "response": {"output": [search, message, call]},
                        }
                    )
                else:
                    assert len(requests) == 2, requests
                    assert (
                        body["input"].count(search) == 1
                        and body["input"].count(message) == 1
                    ), body
                    results = [
                        v
                        for v in body["input"]
                        if v.get("type") == "function_call_output"
                    ]
                    assert len(results) == 1 and results[0]["call_id"] == "pending", (
                        body
                    )
                    event(
                        {
                            "type": "response.output_text.delta",
                            "delta": "HOSTED-CONTINUED",
                        }
                    )
                    event({"type": "response.completed", "response": {"output": []}})
                    finished.set()
            except Exception as exc:
                errors.append(repr(exc))

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="tny-hosted-boundary-") as home:
            ws = Path(home) / "ws"
            ws.mkdir()
            env = base_env(
                home,
                {
                    "CHATGPT_ACCESS_TOKEN": "fixture-secret",
                    "TNY_CODEX_BASE_URL": f"http://127.0.0.1:{server.server_port}/v1",
                    "TNY_PROVIDER_RETRIES": "0",
                },
            )
            term = Term([TNY, "--provider", "codex", "--no-extensions"], env, str(ws))
            try:
                term.expect(BANNER)
                term.send("search then tool\r")
                until(started.is_set, term)
                session = next((Path(home) / ".tny/sessions").glob("*/*/session.json"))
                old = int((session.parent / "pid").read_text())
                call["arguments"] = json.dumps(
                    {
                        "command": "cat "
                        + shlex.quote(str(session.parent / "pid"))
                        + " > execution-pid; printf once >> effects"
                    }
                )
                term.send("\x1b[D")
                term.expect("Background armed")
                release.set()
                term.expect("Background agents", timeout=20)
                until(finished.is_set, term)
                until(
                    lambda: json.loads(session.read_text()).get("status") == "done",
                    term,
                )
                successor = int((ws / "execution-pid").read_text())
                assert successor != old, (successor, old)
                assert (ws / "effects").read_text() == "once"
                assert len(requests) == 2 and not errors, errors
                saved = json.loads(session.read_text())
                assert saved["turns"] == 1 and saved["result"]["steps"] == 2, saved
                assert any(
                    m.get("responses_items") == [search, message]
                    for m in saved["messages"]
                ), saved
                term.send("q")
                assert term.wait() == 0
            finally:
                release.set()
                term.close()
                session = next(
                    (Path(home) / ".tny/sessions").glob("*/*/session.json"), None
                )
                if session:
                    subprocess.run(
                        [TNY, "session", "stop", session.parent.name, "--kill"],
                        env=env,
                        cwd=ws,
                        capture_output=True,
                        timeout=12,
                    )
    finally:
        server.shutdown()
        server.server_close()
    print("PASS hosted search checkpoints before first pending local call in successor")


if __name__ == "__main__":
    for case in ("builtin", "shadowed", "override", "ordinary"):
        run_case(case)

    fetch_compatibility()
    hosted_boundary()
