#!/usr/bin/env python3
"""Exercise truncated TCP EOF and reset independently of a tny parser."""

import importlib.util
import socket
import sys
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "abort_mock", Path(__file__).with_name("mock_openai.py")
)
MOCK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOCK)
PAYLOAD = b"data: partial answer\n\n" * 40


class AbortHandler(MOCK.Handler):
    def do_GET(self):
        self._cut(PAYLOAD, self.path.strip("/"))
        self.server.cut_complete.set()


class AbortFixtureTests(unittest.TestCase):
    def test_abort_disconnects_without_waiting_for_the_client_stall_timer(self):
        server = MOCK.ThreadingHTTPServer(("127.0.0.1", 0), AbortHandler)
        server.cut_complete = threading.Event()
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            # Repeat real fresh sockets: no assertion on scheduler-sensitive
            # milliseconds, and no fake close/shutdown methods or parser mock.
            for mode in ("abort", "reset"):
                for iteration in range(6):
                    self.check_disconnect(server, mode, iteration)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)
            self.assertFalse(thread.is_alive(), "fixture server failed to stop")

    def check_disconnect(self, server, mode, iteration):
        server.cut_complete.clear()
        with (
            self.subTest(mode=mode, iteration=iteration),
            socket.create_connection(server.server_address, timeout=2) as client,
        ):
            client.sendall(f"GET /{mode} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
            self.assertTrue(
                server.cut_complete.wait(2), "fixture did not close the socket"
            )
            received = bytearray()
            try:
                while data := client.recv(65536):
                    received.extend(data)
            except ConnectionResetError:
                pass  # reset and truncated EOF both abort HTTP framing
            if mode == "abort":
                self.assertIn(b"Transfer-Encoding: chunked", received)
                # FIN preserves every sent chunk even with a delayed reader.
                body = received.split(b"\r\n\r\n", 1)[1]
                expected = b"".join(
                    f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n"
                    for offset in range(0, len(PAYLOAD), MOCK.CHUNK_WIDTH)
                    for chunk in [PAYLOAD[offset : offset + MOCK.CHUNK_WIDTH]]
                )
                self.assertEqual(expected, body)
            self.assertNotIn(b"\r\n0\r\n\r\n", received)

    def test_reset_accepts_only_exact_replay_or_continuation(self):
        handler = object.__new__(MOCK.Handler)
        original = [{"role": "user", "content": "fixture"}]
        text = "The workspace contains 3 entries. MOCK-OK"
        with (
            patch.object(MOCK, "CUT_ANSWER_ONCE", "reset"),
            patch.dict(MOCK._cut_state, {"answer": True, "messages": original}),
        ):
            self.assertEqual(text, handler._continuation(original, text))
            for length in range(1, MOCK.CUT_PARTIAL + 1):
                continuation = original + [
                    {"role": "assistant", "content": text[:length]},
                    {"role": "user", "content": MOCK.CONTINUE_MARK},
                ]
                self.assertEqual(
                    text[length:], handler._continuation(continuation, text)
                )
            invalid = [
                [],
                [{"role": "user", "content": "changed"}],
                original
                + [
                    {"role": "assistant", "content": "wrong"},
                    {"role": "user", "content": MOCK.CONTINUE_MARK},
                ],
                original
                + [
                    {"role": "assistant", "content": text[: MOCK.CUT_PARTIAL]},
                    {"role": "user", "content": "wrong"},
                ],
                [{"role": "user", "content": "changed"}] + continuation[-2:],
            ]
            for messages in invalid:
                with (
                    self.subTest(messages=messages),
                    self.assertRaises(MOCK.BadRequest),
                ):
                    handler._continuation(messages, text)
            with patch.object(MOCK, "CUT_ANSWER_ONCE", "abort"):
                with self.assertRaises(MOCK.BadRequest):
                    handler._continuation(original, text)
                self.assertEqual(
                    text[MOCK.CUT_PARTIAL :], handler._continuation(continuation, text)
                )


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
