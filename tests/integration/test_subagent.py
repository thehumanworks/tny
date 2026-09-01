#!/usr/bin/env python3
"""Subagent child startup: provider forwarding, failure surfacing, quoting.

The `subagent` tool spawns a child `tny ask` process. The child must run the
parent's resolved provider — a `last_provider` remembered in settings (say,
codex from an earlier chat) must not re-route it to a host backend — and a
child that dies before its turn must surface its stderr in the tool result
instead of an empty "subagent failed:". Model-supplied strings (id, prompt)
reach a popen(3) shell, so an embedded quote must never escape its argument.
Stdlib only; fixture-only (no live keys, CLAUDE.md).
"""

import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TNY = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", "build/tny")

INJECTION_ID = "x; touch injected-canary; true"


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


class Handler(BaseHTTPRequestHandler):
    """Chat-wire fixture. The parent's first turn calls `subagent` with the
    action carried in the user prompt; the follow-up turn (after the tool
    result) captures that result for the assertions and finishes."""

    protocol_version = "HTTP/1.1"
    tool_results = {}

    def log_message(self, *_args):
        pass

    def _chunk(self, data):
        self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")

    def _stream(self, frames):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for frame in frames:
            self._chunk(f"data: {json.dumps(frame)}\n\n".encode())
        self._chunk(b"data: [DONE]\n\n")
        self._chunk(b"")

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        messages = body.get("messages", [])
        user_text = " ".join(
            m.get("content", "")
            for m in messages
            if m.get("role") == "user" and isinstance(m.get("content"), str)
        )
        tool_result = next(
            (m.get("content") for m in messages if m.get("role") == "tool"), None
        )
        if tool_result is not None:
            scenario = next(
                (k for k in ("create-forwards", "message-bad-id") if k in user_text),
                "unknown",
            )
            Handler.tool_results[scenario] = tool_result
            frames = [
                {"choices": [{"index": 0, "delta": {"content": "PARENT-OK"}}]},
                {
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                    "usage": {"prompt_tokens": 20, "completion_tokens": 2},
                },
            ]
        elif "child prompt" in user_text:
            frames = [
                {"choices": [{"index": 0, "delta": {"content": "CHILD-OK"}}]},
                {
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                    "usage": {"prompt_tokens": 5, "completion_tokens": 2},
                },
            ]
        elif "create-forwards" in user_text or "message-bad-id" in user_text:
            args = {"action": "create", "prompt": "child prompt"}
            if "message-bad-id" in user_text:
                args = {
                    "action": "message",
                    "id": INJECTION_ID,
                    "prompt": "child prompt",
                }
            frames = [
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "role": "assistant",
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "id": "subagent_call_1",
                                        "type": "function",
                                        "function": {
                                            "name": "subagent",
                                            "arguments": json.dumps(args),
                                        },
                                    }
                                ],
                            },
                        }
                    ]
                },
                {
                    "choices": [
                        {"index": 0, "delta": {}, "finish_reason": "tool_calls"}
                    ],
                    "usage": {"prompt_tokens": 10, "completion_tokens": 5},
                },
            ]
        else:
            frames = [
                {"choices": [{"index": 0, "delta": {"content": "UNEXPECTED"}}]},
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]},
            ]
        self._stream(frames)


def run_parent(workspace, env, scenario):
    run = subprocess.run(
        [TNY, "--provider", "openai", "--wire-api", "chat", "ask", "--json", scenario],
        cwd=workspace,
        env=env,
        text=True,
        capture_output=True,
        timeout=60,
    )
    check(
        run.returncode == 0,
        f"{scenario}: parent run failed ({run.returncode}): {run.stderr}",
    )
    try:
        payload = json.loads(run.stdout)
    except json.JSONDecodeError as exc:
        raise Fail(f"{scenario}: stdout was not JSON: {run.stdout!r}: {exc}") from exc
    check(payload.get("output") == "PARENT-OK", payload)
    return payload


def run():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_address[1]

    with tempfile.TemporaryDirectory(prefix="tny-subagent-integration-") as tmp:
        home = os.path.join(tmp, "home")
        workspace = os.path.join(tmp, "ws")
        os.makedirs(os.path.join(home, ".tny"))
        os.makedirs(workspace)
        # The regression: an earlier host-provider chat left last_provider
        # behind. The subagent child used to re-resolve it and try to spawn
        # codex (pointed at a dead binary here) instead of the parent's
        # native provider.
        with open(os.path.join(home, ".tny", "settings.json"), "w") as stream:
            json.dump({"last_provider": "codex"}, stream)
        env = dict(os.environ)
        for key in list(env):
            if key.endswith("_API_KEY") or key.endswith("_BASE_URL"):
                env.pop(key)
        env.update(
            {
                "HOME": home,
                "OPENAI_API_KEY": "integration-test",
                "OPENAI_BASE_URL": f"http://127.0.0.1:{port}/v1",
                "OPENAI_WIRE_API": "chat",
                "OPENAI_DEFAULT_MODEL": "mock-model",
                "TNY_CODEX_BIN": os.path.join(tmp, "no-such-codex"),
            }
        )

        payload = run_parent(workspace, env, "create-forwards")
        check(
            payload.get("tool_calls") == [{"name": "subagent", "status": "success"}],
            payload,
        )
        result = Handler.tool_results.get("create-forwards", "")
        check("CHILD-OK" in result, f"child answer missing from result: {result!r}")
        check(
            "use action=message" in result,
            f"persistent child lost its resumable id: {result!r}",
        )

        # A child that cannot start must say why (its stderr), and a
        # shell-metacharacter id must stay an argument, not become code.
        payload = run_parent(workspace, env, "message-bad-id")
        check(
            payload.get("tool_calls") == [{"name": "subagent", "status": "error"}],
            payload,
        )
        result = Handler.tool_results.get("message-bad-id", "")
        check("subagent failed" in result, f"failure not reported: {result!r}")
        check(
            "no session" in result,
            f"child stderr missing from failure result: {result!r}",
        )
        canary = os.path.join(workspace, "injected-canary")
        check(not os.path.exists(canary), f"shell injection executed: {canary}")

    print("ok  subagent: provider forwarded, startup failure surfaced, id quoted")


if __name__ == "__main__":
    try:
        run()
    except Fail as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        sys.exit(1)
