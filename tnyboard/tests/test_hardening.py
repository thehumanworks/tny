"""Regression checks for failure atomicity, transport bounds and published schemas."""

import concurrent.futures
import json
import socket
import time
import unittest
from pathlib import Path
from unittest.mock import patch

from test_board import BoardFixture
from tnyboard import handle
from tnyboard.core import OPTIONAL, REQUIRED, TICKET_OPS
from tnyboard.http import make_server


class HardeningTest(BoardFixture):
    def test_atomic_replace_failure_and_lock_stability(self):
        lock = Path(self.root) / ".tnyboard/boards/demo/.lock"
        inode = lock.stat().st_ino
        with patch("tnyboard.core.os.replace", side_effect=OSError("simulated")):
            self.reject(
                "comment", id="one", expected_revision=1, body="not stored", code="storage_error"
            )
        self.assertFalse(list(Path(self.root).rglob(".tmp-*")))
        self.call("comment", id="one", expected_revision=1, body="stored")
        self.assertEqual(lock.stat().st_ino, inode)
        self.assertEqual(self.call("get", id="one")["comments"][0]["body"], "stored")
        self.assertIn(".lock", (lock.parent / ".gitignore").read_text())

    def test_document_limit_preserves_old_state(self):
        with patch("tnyboard.core.MAX_FILE", 5000):
            rev = 1
            while True:
                before = self.disk()
                result = handle(
                    self.root,
                    "demo",
                    {
                        "op": "comment",
                        "actor": "user",
                        "id": "one",
                        "expected_revision": rev,
                        "body": "x" * 1000,
                    },
                )
                if not result["ok"]:
                    self.assertEqual(result["error"]["code"], "too_large")
                    self.assertEqual(before, self.disk())
                    break
                rev += 1
                self.assertLess(rev, 10)

    def test_dispatch_board_unique_and_parallel_tickets(self):
        self.call("create", id="two", title="Other")
        self.call("dispatch-intent", id="one", expected_revision=1, intent_id="intent")
        self.reject(
            "dispatch-intent",
            id="two",
            expected_revision=1,
            intent_id="intent",
            code="dispatch_conflict",
        )
        ticket = self.call("dispatch-intent", id="two", expected_revision=1, intent_id="other")
        self.assertEqual(ticket["dispatch"]["state"], "pending")
        self.call(
            "move", id="one", status="done", reason="moved while unresolved", expected_revision=2
        )
        self.reject(
            "dispatch-intent",
            id="one",
            expected_revision=3,
            intent_id="third",
            code="dispatch_conflict",
        )
        self.reject("configure", expected_revision=1, lead=None, code="claim_conflict")

    def test_dispatch_claim_takeover(self):
        self.call("claim", id="one", actor="worker", expected_revision=1, token="old")
        ticket = self.call(
            "dispatch-intent", id="one", actor="board-lead", expected_revision=2, intent_id="intent"
        )
        self.assertIsNone(ticket["claim"])
        self.call("claim", id="one", actor="board-lead", expected_revision=3, token="lead-token")
        self.reject(
            "dispatch-result",
            id="one",
            actor="worker",
            token="old",
            expected_revision=4,
            intent_id="intent",
            outcome="failed",
            code="claim_conflict",
        )

    def test_parallel_claim_and_dispatch_reservations(self):
        for op in ("claim", "dispatch-intent"):
            ticket_id = op
            self.call("create", id=ticket_id, title=op)

            def attempt(n, op=op, ticket_id=ticket_id):
                request = {"op": op, "id": ticket_id, "actor": "worker", "expected_revision": 1}
                request.update({"token": str(n)} if op == "claim" else {"intent_id": "i-" + str(n)})
                return handle(self.root, "demo", request)

            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                results = list(pool.map(attempt, range(16)))
            self.assertEqual(sum(r["ok"] for r in results), 1)
            self.assertEqual(self.call("get", id=ticket_id)["revision"], 2)

    def test_stored_history_validation(self):
        path = Path(self.root) / ".tnyboard/boards/demo/tickets/one.json"
        ticket = self.ticket
        ticket["history"][0]["detail"] = {"ignored": "must not be accepted"}
        path.write_text(json.dumps(ticket))
        self.assertEqual(handle(self.root, "demo", {"op": "get"})["error"]["code"], "corrupt_state")

    def test_case_alias_does_not_overwrite(self):
        path = Path(self.root) / ".tnyboard/boards/demo/tickets/ONE.json"
        if not path.exists():
            self.skipTest("case-sensitive filesystem; no alias")
        self.reject("create", id="ONE", title="Collision", code="already_exists")

    def test_schema_contract_fields(self):
        schemas = Path(__file__).resolve().parents[1] / "schemas"
        request = json.loads((schemas / "request.schema.json").read_text())
        shapes = {shape["properties"]["op"]["const"]: shape for shape in request["oneOf"]}
        self.assertEqual(set(shapes), set(OPTIONAL))
        for op, shape in shapes.items():
            required = {"op"} | REQUIRED.get(op, set())
            if op not in ("get", "list"):
                required.add("actor")
            if op in TICKET_OPS | {"configure"}:
                required.add("expected_revision")
            if op in TICKET_OPS:
                required.add("id")
            self.assertEqual(set(shape["required"]), required)
            self.assertEqual(set(shape["properties"]), required | OPTIONAL[op])
            self.assertFalse(shape["additionalProperties"])
        board = json.loads((schemas / "board.schema.json").read_text())
        ticket = json.loads((schemas / "ticket.schema.json").read_text())
        self.assertEqual(set(board["required"]), set(self.call("get")["board"]))
        self.assertEqual(set(ticket["required"]), set(self.ticket))
        for path in schemas.glob("*.schema.json"):
            schema = json.loads(path.read_text())
            self.assertEqual(schema["$schema"], "https://json-schema.org/draft/2020-12/schema")

    def test_python_39_grammar(self):
        import ast

        for path in Path(__file__).resolve().parents[1].glob("*.py"):
            ast.parse(path.read_text(), feature_version=(3, 9))

    def test_http_deadline_and_split_body(self):
        import threading

        with patch("tnyboard.http.REQUEST_TIMEOUT", 0.3):
            server = make_server(self.root, "demo", "test-token-at-least-16")
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            headers = b"POST /v1/rpc HTTP/1.0\r\nAuthorization: Bearer test-token-at-least-16\r\nContent-Type: application/json\r\nContent-Length: 12\r\n\r\n"
            try:
                with socket.create_connection(server.server_address, timeout=3) as sock:
                    sock.sendall(headers)
                    start = time.monotonic()
                    response = sock.recv(4096)
                    self.assertIn(b"408", response)
                    self.assertLess(time.monotonic() - start, 2)
                with socket.create_connection(server.server_address, timeout=3) as sock:
                    sock.sendall(headers + b'{"op":')
                    time.sleep(0.02)
                    sock.sendall(b'"get"}')
                    response = b""
                    while True:
                        data = sock.recv(4096)
                        if not data:
                            break
                        response += data
                    self.assertIn(b"200 OK", response)
                    self.assertTrue(json.loads(response.split(b"\r\n\r\n", 1)[1])["ok"])
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=3)


if __name__ == "__main__":
    unittest.main()
