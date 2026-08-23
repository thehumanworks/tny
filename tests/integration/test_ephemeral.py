#!/usr/bin/env python3
"""End-to-end ephemeral CLI, TUI-adjacent tool, and ACP checks. Stdlib only."""
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TNY = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", "build/tny")


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def base_cmd():
    # A loopback URL satisfies ACP's credential preflight without making a
    # network request in the initialize/load-only scenario below.
    return [TNY, "--provider", "openai", "--base-url",
            "http://127.0.0.1:9/v1"]


def request(proc, mid, method, params):
    proc.stdin.write(json.dumps({
        "jsonrpc": "2.0", "id": mid, "method": method, "params": params,
    }) + "\n")
    proc.stdin.flush()
    deadline = time.time() + 10
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            raise Fail(f"ACP server closed stdout while waiting for {method}")
        msg = json.loads(line)
        if msg.get("id") == mid:
            return msg
    raise Fail(f"timed out waiting for {method}")


def sse(obj):
    return f"data: {json.dumps(obj)}\n\n".encode()


class SubagentHandler(BaseHTTPRequestHandler):
    """Chat-wire fixture: parent calls subagent; child and parent then finish."""
    protocol_version = "HTTP/1.1"

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
            self._chunk(sse(frame))
        self._chunk(b"data: [DONE]\n\n")
        self._chunk(b"")

    def do_POST(self):
        if not self.path.endswith("/chat/completions"):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n))
        messages = req.get("messages") or []
        user_text = [m.get("content") for m in messages if m.get("role") == "user"]
        has_tool_result = any(m.get("role") == "tool" for m in messages)

        if "child prompt" in user_text:
            frames = [
                {"choices": [{"index": 0, "delta": {"content": "CHILD-OK"}}]},
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                 "usage": {"prompt_tokens": 10, "completion_tokens": 2}},
            ]
        elif has_tool_result:
            frames = [
                {"choices": [{"index": 0, "delta": {"content": "PARENT-OK"}}]},
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                 "usage": {"prompt_tokens": 20, "completion_tokens": 2}},
            ]
        elif "exercise subagent" in user_text:
            frames = [
                {"choices": [{"index": 0, "delta": {
                    "role": "assistant",
                    "tool_calls": [{
                        "index": 0,
                        "id": "subagent_call_1",
                        "type": "function",
                        "function": {
                            "name": "subagent",
                            "arguments": json.dumps({
                                "action": "create", "prompt": "child prompt"
                            }),
                        },
                    }],
                }}]},
                {"choices": [{"index": 0, "delta": {},
                              "finish_reason": "tool_calls"}],
                 "usage": {"prompt_tokens": 10, "completion_tokens": 4}},
            ]
        else:
            frames = [
                {"choices": [{"index": 0, "delta": {"content": "UNEXPECTED"}}]},
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]},
            ]
        self._stream(frames)


def exercise_ephemeral_subagent(home, workspace, env):
    server = ThreadingHTTPServer(("127.0.0.1", 0), SubagentHandler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        port = server.server_address[1]
        child_env = dict(env)
        child_env.update({
            "OPENAI_API_KEY": "integration-test",
            "OPENAI_BASE_URL": f"http://127.0.0.1:{port}/v1",
            "OPENAI_WIRE_API": "chat",
            "OPENAI_DEFAULT_MODEL": "mock-model",
        })
        run = subprocess.run(
            [TNY, "--provider", "openai", "--wire-api", "chat",
             "--ephemeral", "ask", "--json", "exercise subagent"],
            cwd=workspace, env=child_env, text=True, capture_output=True,
            timeout=30,
        )
        check(run.returncode == 0,
              f"ephemeral subagent run failed ({run.returncode}): {run.stderr}")
        try:
            payload = json.loads(run.stdout)
        except json.JSONDecodeError as exc:
            raise Fail(f"subagent stdout was not JSON: {run.stdout!r}: {exc}") from exc
        check(payload.get("output") == "PARENT-OK", payload)
        check(payload.get("ephemeral") is True, payload)
        check(payload.get("session_id") == "", payload)
        sessions = os.path.join(home, ".tny", "sessions")
        check(not os.path.exists(sessions),
              f"ephemeral child created a session store: {sessions}")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def run():
    with tempfile.TemporaryDirectory(prefix="tny-ephemeral-integration-") as tmp:
        home = os.path.join(tmp, "home")
        workspace = os.path.join(tmp, "workspace")
        os.makedirs(home)
        os.makedirs(workspace)
        env = dict(os.environ)
        env["HOME"] = home
        for key in list(env):
            if key.endswith("_API_KEY") or key.endswith("_BASE_URL"):
                env.pop(key)

        # Leading global mode reaches the ACP surface. The server must not
        # claim saved-session support and must reject an attempted load.
        proc = subprocess.Popen(
            base_cmd() + ["--ephemeral", "acp"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1,
            cwd=workspace, env=env,
        )
        try:
            init = request(proc, 1, "initialize", {
                "protocolVersion": 1,
                "clientCapabilities": {},
                "clientInfo": {"name": "ephemeral-test", "version": "0"},
            })
            check("result" in init, f"initialize failed: {init}")
            caps = init["result"]["agentCapabilities"]
            check(caps.get("loadSession") is False,
                  f"ephemeral ACP advertised loadSession: {init}")

            load = request(proc, 2, "session/load", {"sessionId": "saved-id"})
            check("error" in load, f"ephemeral session/load succeeded: {load}")
            check("ephemeral" in load["error"].get("message", "").lower(),
                  f"session/load error did not explain the mode: {load}")
        finally:
            try:
                proc.stdin.close()
            except OSError:
                pass
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        check(proc.returncode == 0, proc.stderr.read())

        # Saved-state entry points fail before any backend connection.
        resumed = subprocess.run(
            base_cmd() + ["--ephemeral", "resume", "last"],
            cwd=workspace, env=env, text=True, capture_output=True,
        )
        check(resumed.returncode == 1,
              f"ephemeral resume exit {resumed.returncode}: {resumed.stderr}")
        check("cannot resume" in resumed.stderr.lower(), resumed.stderr)

        ask = subprocess.run(
            base_cmd() + ["ask", "--ephemeral", "--resume", "last", "hello"],
            cwd=workspace, env=env, text=True, capture_output=True,
        )
        check(ask.returncode == 1,
              f"ephemeral ask/resume exit {ask.returncode}: {ask.stderr}")
        check("incompatible" in ask.stderr.lower(), ask.stderr)

        status = subprocess.run(
            base_cmd() + ["--ephemeral", "--json", "status"],
            cwd=workspace, env=env, text=True, capture_output=True,
        )
        check(status.returncode == 0,
              f"ephemeral status failed ({status.returncode}): {status.stderr}")
        try:
            status_payload = json.loads(status.stdout)
        except json.JSONDecodeError as exc:
            raise Fail(f"status stdout was not JSON: {status.stdout!r}: {exc}") from exc
        check(status_payload.get("ephemeral") is True, status_payload)

        # A native-loop child agent is another conversational surface. It
        # must inherit the mode rather than materialize a child session.
        exercise_ephemeral_subagent(home, workspace, env)

        # None of these surfaces may materialize a conversation store.
        sessions = os.path.join(home, ".tny", "sessions")
        history = os.path.join(home, ".tny", "history")
        check(not os.path.exists(sessions), f"created session store: {sessions}")
        check(not os.path.exists(history), f"created prompt history: {history}")

    print("ok  ephemeral: ACP, status, resume guards, and child-agent propagation")


if __name__ == "__main__":
    try:
        run()
    except (Fail, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
