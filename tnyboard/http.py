"""Bounded local HTTP transport. No browser CORS and no remote bind option."""

import hmac
import json
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

from .core import MAX_FILE, BoardError, handle, parse_json

REQUEST_TIMEOUT = 5


def make_server(root, board, token, port=0):
    """Construct a loopback-only server. Caller owns serve_forever/server_close."""
    if (
        not isinstance(token, str)
        or not 16 <= len(token) <= 256
        or not all(33 <= ord(c) <= 126 for c in token)
    ):
        raise ValueError("TNYBOARD_TOKEN must be 16..256 printable ASCII characters without spaces")

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"
        server_version = "tnyboard/1"
        sys_version = ""

        def setup(self):
            super().setup()
            self.connection.settimeout(REQUEST_TIMEOUT)

        def handle(self):
            # A total deadline also stops slow-drip request headers. A socket read
            # timeout alone restarts after each received byte.
            def expire():
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

            self.deadline = time.monotonic() + REQUEST_TIMEOUT
            timer = threading.Timer(REQUEST_TIMEOUT + 0.1, expire)
            timer.daemon = True
            timer.start()
            try:
                super().handle()
            finally:
                timer.cancel()

        def log_message(self, *_args):
            pass

        def reply(self, status, result):
            data = (json.dumps(result, ensure_ascii=True) + "\n").encode("ascii")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            try:
                self.wfile.write(data)
            except OSError:
                pass

        def error(self, status, code, message):
            self.reply(status, {"ok": False, "error": {"code": code, "message": message}})

        def send_error(self, code, message=None, explain=None):
            self.error(code, "http_error", "Malformed or unsupported HTTP request")

        def authorized(self):
            auth = self.headers.get_all("Authorization", [])
            if len(auth) != 1 or not hmac.compare_digest(
                auth[0].encode("utf-8"), ("Bearer " + token).encode("ascii")
            ):
                self.error(401, "unauthorized", "Bearer token required")
                return False
            if self.headers.get("Origin") is not None:
                self.error(403, "forbidden", "Browser origins are not supported")
                return False
            return True

        def do_GET(self):
            if not self.authorized():
                return
            if self.path != "/v1/health":
                self.error(404, "not_found", "Unknown endpoint")
                return
            self.reply(200, {"ok": True, "result": {"schema_version": 1}})

        def do_POST(self):
            if not self.authorized():
                return
            if self.path != "/v1/rpc":
                self.error(404, "not_found", "Unknown endpoint")
                return
            lengths = self.headers.get_all("Content-Length", [])
            if (
                self.headers.get("Transfer-Encoding") is not None
                or len(lengths) != 1
                or not lengths[0].isascii()
                or not lengths[0].isdigit()
            ):
                self.error(
                    400, "invalid_request", "One Content-Length and no Transfer-Encoding required"
                )
                return
            # Bound decimal conversion before int() on Python versions without its limit.
            if len(lengths[0]) > 8 or int(lengths[0]) > MAX_FILE:
                self.error(413, "too_large", "Request body too large")
                return
            if self.headers.get_all("Content-Type", []) != ["application/json"]:
                self.error(415, "invalid_request", "Content-Type must be application/json")
                return
            length = int(lengths[0])
            try:
                chunks = []
                remaining = length
                while remaining:
                    budget = self.deadline - time.monotonic()
                    if budget <= 0:
                        raise TimeoutError()
                    self.connection.settimeout(budget)
                    chunk = self.rfile.read1(remaining)
                    if not chunk:
                        break
                    chunks.append(chunk)
                    remaining -= len(chunk)
                data = b"".join(chunks)
                if len(data) != length:
                    self.error(400, "invalid_request", "Incomplete request body")
                    return
                request = parse_json(data.decode("utf-8"))
            except (socket.timeout, TimeoutError):
                self.error(408, "timeout", "Request body timed out")
                return
            except (BoardError, UnicodeError) as exc:
                self.error(400, "invalid_request", str(exc))
                return
            response = handle(root, board, request)
            code = response.get("error", {}).get("code")
            status = (
                200
                if response["ok"]
                else {
                    "not_found": 404,
                    "forbidden": 403,
                    "revision_conflict": 409,
                    "claim_conflict": 409,
                    "dispatch_conflict": 409,
                    "already_exists": 409,
                    "too_large": 413,
                    "corrupt_state": 500,
                    "storage_error": 500,
                    "unsafe_path": 400,
                }.get(code, 400)
            )
            self.reply(status, response)

    # Serial HTTP admission bounds resource use; board process lock also covers CLI.
    server = HTTPServer(("127.0.0.1", port), Handler)
    server.timeout = REQUEST_TIMEOUT
    return server
