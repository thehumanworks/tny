#!/usr/bin/env python3
"""Strict remote MCP fixture: modern, legacy session, and rejected SSE."""

import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODERN = "2026-07-28"
LEGACY = "2025-06-18"
SESSION = "fixture-session-87"
STATE_PATH = os.environ.get("MCP_MOCK_STATE")
TOKEN = os.environ.get("MCP_MOCK_TOKEN", "fixture-secret-token")
LOCK = threading.Lock()
STATE = {
    "get": 0,
    "auth_ok": 0,
    "modern": {},
    "legacy": {},
    "sse": {},
    "legacy_sse": {},
    "errors": [],
}


def record(group, method):
    with LOCK:
        methods = STATE[group]
        methods[method] = methods.get(method, 0) + 1
        if STATE_PATH:
            tmp = STATE_PATH + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(STATE, f, sort_keys=True)
            os.replace(tmp, STATE_PATH)


def fail(message):
    with LOCK:
        STATE["errors"].append(message)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, _fmt, *_args):
        pass

    def do_GET(self):
        with LOCK:
            STATE["get"] += 1
        fail(f"unexpected GET {self.path}")
        self.send_response(405)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _json(self, status, obj, *, session=None, chunked=False):
        data = json.dumps(obj, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        if session:
            self.send_header("Mcp-Session-Id", session)
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if chunked:
            # One payload byte per HTTP chunk makes JSON and framing cross
            # arbitrary read boundaries even when the kernel coalesces writes.
            for byte in data:
                self.wfile.write(b"1\r\n" + bytes([byte]) + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.wfile.write(data)
        self.wfile.flush()

    def _empty(self, status):
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _validate_auth(self):
        ok = (
            self.headers.get("Authorization") == f"Bearer {TOKEN}"
            and self.headers.get("X-Tenant") == "fixture"
        )
        if ok:
            with LOCK:
                STATE["auth_ok"] += 1
        else:
            fail("missing or incorrect synthetic auth headers")
        return ok

    def _modern(self, req):
        method = req.get("method", "<missing>")
        record("modern", method)
        params = req.get("params") or {}
        meta = params.get("_meta") or {}
        checks = [
            self.headers.get("MCP-Protocol-Version") == MODERN,
            self.headers.get("Mcp-Method") == method,
            self.headers.get("Mcp-Session-Id") is None,
            meta.get("io.modelcontextprotocol/protocolVersion") == MODERN,
            isinstance(meta.get("io.modelcontextprotocol/clientInfo"), dict),
            isinstance(meta.get("io.modelcontextprotocol/clientCapabilities"), dict),
        ]
        if method == "tools/call":
            checks.append(self.headers.get("Mcp-Name") == params.get("name"))
        if not all(checks):
            fail(f"invalid stateless metadata for {method}")
            return self._json(
                400, {"jsonrpc": "2.0", "id": req.get("id"), "error": {"code": -32020}}
            )
        if method == "server/discover":
            result = {
                "resultType": "complete",
                "supportedVersions": [MODERN],
                "capabilities": {"tools": {}},
            }
        elif method == "tools/list":
            result = {
                "resultType": "complete",
                "tools": [
                    {
                        "name": "echo",
                        "description": "Echo through stateless HTTP",
                        "inputSchema": {"type": "object"},
                    }
                ],
            }
        elif method == "tools/call":
            result = {
                "resultType": "complete",
                "content": [{"type": "text", "text": "modern called ok"}],
            }
        else:
            return self._json(
                404, {"jsonrpc": "2.0", "id": req.get("id"), "error": {"code": -32601}}
            )
        self._json(
            200,
            {"jsonrpc": "2.0", "id": req.get("id"), "result": result},
            chunked=method == "tools/list",
        )

    def _legacy(self, req):
        method = req.get("method", "<missing>")
        record("legacy", method)
        if method == "server/discover":
            return self._empty(400)
        if method == "initialize":
            result = {"protocolVersion": LEGACY, "capabilities": {"tools": {}}}
            return self._json(
                200,
                {"jsonrpc": "2.0", "id": req.get("id"), "result": result},
                session=SESSION,
            )
        if self.headers.get("Mcp-Session-Id") != SESSION:
            fail(f"legacy request {method} omitted the negotiated session")
            return self._empty(400)
        if self.headers.get("MCP-Protocol-Version") != LEGACY:
            fail(f"legacy request {method} omitted the negotiated protocol version")
            return self._empty(400)
        if method == "notifications/initialized":
            return self._empty(202)
        if method == "tools/list":
            result = {
                "tools": [
                    {
                        "name": "echo",
                        "description": "Echo through session HTTP",
                        "inputSchema": {"type": "object"},
                    }
                ]
            }
            return self._json(
                200,
                {"jsonrpc": "2.0", "id": req.get("id"), "result": result},
                chunked=True,
            )
        if method == "tools/call":
            result = {"content": [{"type": "text", "text": "legacy called ok"}]}
            return self._json(
                200, {"jsonrpc": "2.0", "id": req.get("id"), "result": result}
            )
        self._empty(404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        try:
            req = json.loads(self.rfile.read(length))
        except (ValueError, json.JSONDecodeError):
            return self._empty(400)
        if not self._validate_auth():
            return self._empty(401)
        if self.path == "/modern":
            return self._modern(req)
        if self.path == "/legacy":
            return self._legacy(req)
        group = "sse" if self.path == "/sse" else "legacy_sse"
        record(group, req.get("method", "<missing>"))
        if self.path == "/sse":
            method = req.get("method", "<missing>")
            params = req.get("params") or {}
            if method == "server/discover":
                obj = {
                    "jsonrpc": "2.0",
                    "id": req.get("id"),
                    "result": {
                        "resultType": "complete",
                        "supportedVersions": [MODERN],
                        "capabilities": {"tools": {}},
                    },
                }
            elif method == "tools/list":
                obj = {
                    "jsonrpc": "2.0",
                    "id": req.get("id"),
                    "result": {
                        "resultType": "complete",
                        "tools": [
                            {
                                "name": "echo",
                                "description": "Echo through SSE HTTP",
                                "inputSchema": {"type": "object"},
                            }
                        ],
                    },
                }
            elif method == "tools/call":
                obj = {
                    "jsonrpc": "2.0",
                    "id": req.get("id"),
                    "result": {
                        "resultType": "complete",
                        "content": [{"type": "text", "text": "sse called ok"}],
                    },
                }
            else:
                return self._json(
                    404,
                    {"jsonrpc": "2.0", "id": req.get("id"), "error": {"code": -32601}},
                )
            if method == "tools/call" and self.headers.get("Mcp-Name") != params.get(
                "name"
            ):
                fail("invalid Mcp-Name on SSE tools/call")
                return self._json(
                    400,
                    {"jsonrpc": "2.0", "id": req.get("id"), "error": {"code": -32020}},
                )
            if method != "tools/call":
                return self._json(200, obj, chunked=method == "tools/list")
            payload = json.dumps(obj, separators=(",", ":")).encode()
            note = b'data: {"jsonrpc":"2.0","method":"notifications/progress"}\n\n'
            data = note + b"data: " + payload + b"\n\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            self.wfile.flush()
            return
        self.send_response(405)
        self.send_header("Allow", "GET")
        self.send_header("Content-Length", "0")
        self.end_headers()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    record("modern", "fixture/start")
    print(f"ready on {server.server_address[1]}", flush=True)
    server.serve_forever()
