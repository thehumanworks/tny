#!/usr/bin/env python3
"""Mock OpenAI-compatible provider for tny integration tests.

Turn 1: streams a tool_call (list_files), split across SSE chunks.
Turn 2: streams a text answer mentioning what the tool returned.
Also serves GET /v1/models. SSE is chunked-encoded like real providers.

Usage: mock_openai.py [port] [certfile keyfile]
With certfile/keyfile the mock serves HTTPS (used by test_https.py).
"""
import json
import os
import ssl
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

# MOCK_EXPECT_EFFORT: every chat request must carry exactly this
# `reasoning_effort`; unset means the field must be absent (tny sends it only
# when --effort / TNY_REASONING_EFFORT is set).
EXPECT_EFFORT = os.environ.get("MOCK_EXPECT_EFFORT")
# MOCK_SLOW_MS: delay before the first (tool-call) response so a TUI test can
# steer while the turn runs. MOCK_EXPECT_STEER: the follow-up request (the one
# carrying the tool result) must END with a user message of exactly this text
# — the steered input rides after the tool result, never inside it.
SLOW_MS = int(os.environ.get("MOCK_SLOW_MS", "0"))
EXPECT_STEER = os.environ.get("MOCK_EXPECT_STEER")
# MOCK_PARALLEL: turn 1 streams THREE parallel tool calls in the gateway
# shape from the field: the third call reuses the second call's "index" but
# carries its own fresh "id" (index-keyed assembly used to merge the two and
# drop an id, unpairing the transcript).
PARALLEL = os.environ.get("MOCK_PARALLEL") == "1"


def sse(obj):
    return f"data: {json.dumps(obj)}\n\n".encode()


def unpaired_tool_calls(messages):
    """What a strict provider validates: every id in an assistant message's
    tool_calls needs a role:tool message with that tool_call_id before the
    next assistant message, ids must be unique, arguments must be JSON."""
    problems = []
    for i, m in enumerate(messages):
        if m.get("role") != "assistant" or not m.get("tool_calls"):
            continue
        ids = [tc["id"] for tc in m["tool_calls"]]
        if len(ids) != len(set(ids)):
            problems.append(f"duplicate tool_call ids {ids}")
        for tc in m["tool_calls"]:
            try:
                json.loads(tc["function"]["arguments"] or "{}")
            except ValueError:
                problems.append("arguments of %s are not JSON: %r"
                                % (tc["id"], tc["function"]["arguments"]))
        seen = set()
        for mm in messages[i + 1:]:
            if mm.get("role") == "assistant":
                break
            if mm.get("role") == "tool":
                seen.add(mm.get("tool_call_id"))
        for cid in ids:
            if cid not in seen:
                problems.append(f"no tool output found for function call {cid}")
    return problems


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _chunk(self, data: bytes):
        self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")

    def do_GET(self):
        if self.path.endswith("/models"):
            body = json.dumps({"data": [{"id": "mock-model-1"}, {"id": "mock-model-2"}]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n))
        if req.get("reasoning_effort") != EXPECT_EFFORT:
            body = json.dumps({"error": {"message":
                f"reasoning_effort is {req.get('reasoning_effort')!r}, "
                f"want {EXPECT_EFFORT!r}"}}).encode()
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        problems = unpaired_tool_calls(req["messages"])
        if problems:
            body = json.dumps({"error": {"message": "; ".join(problems)}}).encode()
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        has_tool_result = any(m.get("role") == "tool" for m in req["messages"])
        if not has_tool_result and SLOW_MS:
            import time
            time.sleep(SLOW_MS / 1000.0)
        if has_tool_result and EXPECT_STEER:
            last = req["messages"][-1]
            if last.get("role") != "user" or last.get("content") != EXPECT_STEER:
                body = json.dumps({"error": {"message":
                    f"steer: last message is {last!r}, want user {EXPECT_STEER!r}"}}).encode()
                self.send_response(400)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return

        # Structured outputs: validate the wrapper tny sends, echo JSON back.
        structured = req.get("response_format")
        if structured is not None:
            assert structured["type"] == "json_schema", structured
            assert "schema" in structured["json_schema"], structured
            assert "name" in structured["json_schema"], structured

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        if PARALLEL and not has_tool_result:
            frames = [
                # call 1: id+name first, arguments fragmented, keyed by index
                {"choices": [{"index": 0, "delta": {"role": "assistant",
                    "tool_calls": [{"index": 0, "id": "par_A", "type": "function",
                                    "function": {"name": "read_file", "arguments": ""}}]}}]},
                {"choices": [{"index": 0, "delta": {
                    "tool_calls": [{"index": 0, "function": {"arguments": "{\"path\":\"a.txt\"}"}}]}}]},
                # call 2: complete in one chunk
                {"choices": [{"index": 0, "delta": {
                    "tool_calls": [{"index": 1, "id": "par_B", "type": "function",
                                    "function": {"name": "read_file",
                                                 "arguments": "{\"path\":\"b.txt\"}"}}]}}]},
                # call 3: REUSES index 1 with a fresh id (the gateway bug)
                {"choices": [{"index": 0, "delta": {
                    "tool_calls": [{"index": 1, "id": "par_C", "type": "function",
                                    "function": {"name": "list_files",
                                                 "arguments": "{\"path\":\".\"}"}}]}}]},
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}],
                 "usage": {"prompt_tokens": 100, "completion_tokens": 30}},
            ]
        elif PARALLEL:
            by_id = {m.get("tool_call_id"): m["content"]
                     for m in req["messages"] if m.get("role") == "tool"}
            ok = ("aaa" in by_id.get("par_A", "") and
                  "bbb" in by_id.get("par_B", "") and
                  "a.txt" in by_id.get("par_C", ""))
            text = "PARALLEL-OK" if ok else f"PARALLEL-BAD {by_id!r}"
            frames = [
                {"choices": [{"index": 0, "delta": {"content": text}}]},
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                 "usage": {"prompt_tokens": 200, "completion_tokens": 20}},
            ]
        elif not has_tool_result:
            frames = [
                {"choices": [{"index": 0, "delta": {"role": "assistant",
                    "tool_calls": [{"index": 0, "id": "call_1", "type": "function",
                                    "function": {"name": "list_files", "arguments": ""}}]}}]},
                {"choices": [{"index": 0, "delta": {
                    "tool_calls": [{"index": 0, "function": {"arguments": "{\"pa"}}]}}]},
                {"choices": [{"index": 0, "delta": {
                    "tool_calls": [{"index": 0, "function": {"arguments": "th\": \".\"}"}}]}}]},
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}],
                 "usage": {"prompt_tokens": 100, "completion_tokens": 10}},
            ]
        else:
            tool_msg = next(m for m in req["messages"] if m.get("role") == "tool")
            nfiles = len([l for l in tool_msg["content"].splitlines() if l.strip()])
            tier = req.get("service_tier", "unset")
            if structured is not None:
                text = json.dumps({"count": nfiles, "note": "MOCK-OK"})
            else:
                text = f"The workspace contains {nfiles} entries. MOCK-OK. tier={tier}"
                if EXPECT_STEER:
                    text += " STEER-OK"
            frames = []
            for i in range(0, len(text), 7):
                frames.append({"choices": [{"index": 0, "delta": {"content": text[i:i+7]}}]})
            frames.append({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                           "usage": {"prompt_tokens": 200, "completion_tokens": 20}})

        for f in frames:
            self._chunk(sse(f))
        self._chunk(b"data: [DONE]\n\n")
        self._chunk(b"")  # final chunk


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    srv = HTTPServer(("127.0.0.1", port), Handler)
    if len(sys.argv) > 3:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=sys.argv[2], keyfile=sys.argv[3])
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print(f"ready on {port}", flush=True)
    srv.serve_forever()
