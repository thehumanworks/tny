#!/usr/bin/env python3
"""Exercise the fixture's actual TCP abort, independently of a tny parser."""

import importlib.util
import socket
import sys
import threading
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    "abort_mock", Path(__file__).with_name("mock_openai.py")
)
MOCK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOCK)
PAYLOAD = b"data: partial answer\n\n" * 40


class AbortHandler(MOCK.Handler):
    def do_GET(self):
        self._cut(PAYLOAD, "abort")


class AbortFixtureTests(unittest.TestCase):
    def test_abort_disconnects_without_waiting_for_the_client_stall_timer(self):
        server = MOCK.ThreadingHTTPServer(("127.0.0.1", 0), AbortHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            # Repeat real fresh sockets: no assertion on scheduler-sensitive
            # milliseconds, and no fake close/shutdown methods or parser mock.
            for iteration in range(6):
                with (
                    self.subTest(iteration=iteration),
                    socket.create_connection(
                        server.server_address, timeout=2
                    ) as client,
                ):
                    client.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
                    received = bytearray()
                    try:
                        while data := client.recv(65536):
                            received.extend(data)
                    except ConnectionResetError:
                        pass  # reset and truncated EOF both abort HTTP framing
                    self.assertIn(b"Transfer-Encoding: chunked", received)
                    self.assertIn(b"data: partial", received)
                    self.assertNotIn(b"\r\n0\r\n\r\n", received)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)
            self.assertFalse(thread.is_alive(), "fixture server failed to stop")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
