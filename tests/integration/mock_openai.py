#!/usr/bin/env python3
"""Mock OpenAI-compatible provider for tny integration tests.

Turn 1: streams a tool_call (list_files), split across SSE chunks.
Turn 2: streams a text answer mentioning what the tool returned.
Also serves GET /v1/models. SSE is chunked-encoded like real providers.

Usage: mock_openai.py [port] [certfile keyfile]
With certfile/keyfile the mock serves HTTPS (used by test_https.py).
"""
import json
import ssl
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


def sse(obj):
    return f"data: {json.dumps(obj)}\n\n".encode()


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
        has_tool_result = any(m.get("role") == "tool" for m in req["messages"])

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

        if not has_tool_result:
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
            if structured is not None:
                text = json.dumps({"count": nfiles, "note": "MOCK-OK"})
            else:
                text = f"The workspace contains {nfiles} entries. MOCK-OK."
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
