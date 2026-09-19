import concurrent.futures
import copy
import io
import json
import multiprocessing
import os
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

from tnyboard import handle, render
from tnyboard.core import MAX_FILE
from tnyboard.http import make_server
from tnyboard.terminal import tui

REPO = Path(__file__).resolve().parents[2]


def race(root, number):
    return handle(
        root,
        "demo",
        {
            "op": "comment",
            "id": "one",
            "actor": "worker",
            "expected_revision": 1,
            "body": str(number),
        },
    )


class BoardFixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = str(Path(self.tmp.name).resolve())
        self.call("init")
        self.ticket = self.call("create", id="one", title="First")

    def tearDown(self):
        self.tmp.cleanup()

    def call(self, op, **kwargs):
        req = {"op": op, **kwargs}
        if op not in ("get", "list"):
            req.setdefault("actor", "user")
        result = handle(self.root, "demo", req)
        self.assertTrue(result["ok"], result)
        return result["result"]

    def reject(self, op, code=None, **kwargs):
        before = self.disk()
        result = handle(self.root, "demo", {"op": op, "actor": "user", **kwargs})
        self.assertFalse(result["ok"], result)
        if code:
            self.assertEqual(result["error"]["code"], code, result)
        self.assertEqual(before, self.disk())
        return result

    def disk(self):
        return {
            str(p): p.read_bytes() for p in Path(self.root).rglob("*.json") if not p.is_symlink()
        }


class BoardTest(BoardFixture):
    def test_layout_and_persistence(self):
        state = self.call("get")
        self.assertEqual(state["board"]["lead"], "board-lead")
        self.assertEqual(state["tickets"], [self.ticket])
        self.assertEqual(self.call("list"), state)
        self.assertEqual(len(self.disk()), 2)
        self.assertEqual(self.call("get", id="one"), self.ticket)

    def test_revision_and_types(self):
        for rev in (0, True, "1", 2, None):
            self.reject("move", id="one", status="done", reason="review", expected_revision=rev)
        self.reject("move", id="one", status="done", reason="review")
        self.reject("create", id="one", title="Duplicate", code="already_exists")
        self.reject("configure", expected_revision=2, lead=None, code="revision_conflict")

    def test_owner_lead_and_reverse_moves(self):
        self.reject(
            "move",
            id="one",
            status="doing",
            reason="start",
            actor="other",
            expected_revision=1,
            code="forbidden",
        )
        ticket = self.call(
            "move", id="one", status="done", reason="skip", actor="worker", expected_revision=1
        )
        self.assertEqual(ticket["status"], "done")
        ticket = self.call(
            "move", id="one", status="todo", reason="redo", actor="board-lead", expected_revision=2
        )
        self.assertEqual(ticket["history"][-1]["detail"]["reason"], "redo")
        self.reject("move", id="one", status="todo", reason=" ", expected_revision=3)

    def test_claim_fencing_and_comments(self):
        ticket = self.call("claim", id="one", actor="worker", expected_revision=1)
        token = ticket["claim"]["token"]
        for op, extra in (
            ("claim", {}),
            ("release", {}),
            ("assign", {"owner": "other"}),
            ("move", {"status": "done", "reason": "finish"}),
            ("dispatch-intent", {"intent_id": "run1"}),
        ):
            self.reject(
                op, id="one", actor="worker", expected_revision=2, code="claim_conflict", **extra
            )
        ticket = self.call(
            "comment", id="one", actor="observer", expected_revision=2, body="Review this"
        )
        self.assertEqual(ticket["comments"][0]["actor"], "observer")
        ticket = self.call(
            "move",
            id="one",
            actor="worker",
            expected_revision=3,
            token=token,
            status="doing",
            reason="start",
        )
        self.assertIsNone(ticket["claim"])
        self.assertEqual(ticket["revision"], 4)

    def test_claim_takeover_and_override(self):
        self.call("claim", id="one", actor="worker", expected_revision=1, token="old")
        self.call("claim", id="one", actor="board-lead", expected_revision=2, token="new")
        self.reject(
            "release",
            id="one",
            actor="worker",
            token="old",
            expected_revision=3,
            code="claim_conflict",
        )
        ticket = self.call(
            "assign", id="one", actor="board-lead", expected_revision=3, owner="other"
        )
        self.assertIsNone(ticket["claim"])
        self.reject("claim", id="one", actor="worker", expected_revision=4, code="forbidden")
        self.call(
            "move", id="one", actor="other", expected_revision=4, status="doing", reason="mine"
        )
        ticket = self.call("assign", id="one", actor="other", expected_revision=5, owner=None)
        self.assertIsNone(ticket["owner"])
        self.call("claim", id="one", actor="worker", expected_revision=6)
        ticket = self.call("release", id="one", expected_revision=7)
        self.assertIsNone(ticket["claim"])

    def test_configure(self):
        self.reject("configure", actor="worker", expected_revision=1, lead=None, code="forbidden")
        self.call("configure", expected_revision=1, lead=None)
        self.reject(
            "configure", actor="board-lead", expected_revision=2, lead="lead", code="forbidden"
        )
        self.reject(
            "configure", expected_revision=2, columns=[{"id": "new", "title": "New", "owner": None}]
        )
        self.call("claim", id="one", actor="worker", expected_revision=1)
        self.reject("configure", expected_revision=2, lead="lead", code="claim_conflict")

    def test_dispatch_intent_results_and_no_retry(self):
        ticket = self.call(
            "dispatch-intent", id="one", actor="worker", expected_revision=1, intent_id="run1"
        )
        self.assertEqual(ticket["dispatch"]["state"], "pending")
        self.reject(
            "dispatch-intent",
            id="one",
            expected_revision=2,
            intent_id="run2",
            code="dispatch_conflict",
        )
        self.reject(
            "dispatch-result",
            id="one",
            expected_revision=2,
            intent_id="wrong",
            outcome="failed",
            code="dispatch_conflict",
        )
        self.reject(
            "dispatch-result", id="one", expected_revision=2, intent_id="run1", outcome="confirmed"
        )
        self.call(
            "dispatch-result",
            id="one",
            expected_revision=2,
            intent_id="run1",
            outcome="uncertain",
            detail="lost ack",
        )
        self.reject(
            "dispatch-intent",
            id="one",
            expected_revision=3,
            intent_id="run2",
            code="dispatch_conflict",
        )
        ticket = self.call(
            "dispatch-result",
            id="one",
            expected_revision=3,
            intent_id="run1",
            outcome="confirmed",
            job_id="job-123",
        )
        self.assertEqual(ticket["dispatch"]["job_id"], "job-123")
        self.reject(
            "dispatch-result",
            id="one",
            expected_revision=4,
            intent_id="run1",
            outcome="failed",
            code="dispatch_conflict",
        )
        self.reject(
            "dispatch-intent",
            id="one",
            expected_revision=4,
            intent_id="run1",
            code="dispatch_conflict",
        )
        self.call("dispatch-intent", id="one", expected_revision=4, intent_id="run2")
        ticket = self.call(
            "dispatch-result",
            id="one",
            expected_revision=5,
            intent_id="run2",
            outcome="failed",
            detail="launcher exited 1",
        )
        self.assertEqual(ticket["dispatch"]["state"], "failed")
        self.assertEqual(self.call("get", id="one"), ticket)

    def test_input_validation(self):
        for bad in ("../escape", ".", "a/b", "a.json", "", "a" * 65):
            self.reject("create", id=bad, title="Bad")
            result = handle(self.root, bad, {"op": "get"})
            self.assertFalse(result["ok"])
        for title in (None, "", " " * 3, "x" * 1025, "\ud800"):
            self.reject("create", title=title)
        self.reject("create", title="Bad", body="x" * 65537)
        self.reject("create", title="Bad", owner="bad\nactor")
        self.reject("create", title="Bad", surprise=True)
        self.reject("comment", id="one", expected_revision=1, actor="", body="test")
        self.reject(
            "configure",
            expected_revision=1,
            columns=[{"id": "same", "title": "A", "owner": None}] * 2,
        )
        for req in ([], None, {"op": []}, {"op": "unknown"}):
            self.assertFalse(handle(self.root, "demo", req)["ok"])

    def test_stored_corruption(self):
        path = Path(self.root) / ".tnyboard/boards/demo/tickets/one.json"
        original = path.read_bytes()
        bad_ticket = copy.deepcopy(self.ticket)
        bad_ticket["claim"] = {"actor": "worker"}
        for data in (
            b'{"id":"one","id":"one"}',
            b"not json",
            b"x" * (MAX_FILE + 1),
            json.dumps(bad_ticket).encode(),
        ):
            path.write_bytes(data)
            result = handle(self.root, "demo", {"op": "get"})
            self.assertFalse(result["ok"])
            self.assertEqual(result["error"]["code"], "corrupt_state")
            self.assertEqual(path.read_bytes(), data)
        path.write_bytes(original)
        self.assertEqual(self.call("get", id="one"), self.ticket)

    def test_symlinks_and_hardlinks(self):
        base = Path(self.root) / ".tnyboard/boards/demo"
        outside = Path(self.root) / "outside.json"
        outside.write_text("outside")
        path = base / "tickets/one.json"
        path.unlink()
        path.symlink_to(outside)
        result = handle(self.root, "demo", {"op": "get"})
        self.assertEqual(result["error"]["code"], "unsafe_path")
        path.unlink()
        os.link(outside, path)
        self.assertEqual(handle(self.root, "demo", {"op": "get"})["error"]["code"], "unsafe_path")
        path.unlink()
        (base / ".lock").unlink()
        (base / ".lock").symlink_to(outside)
        self.assertEqual(handle(self.root, "demo", {"op": "get"})["error"]["code"], "unsafe_path")
        self.assertEqual(outside.read_text(), "outside")
        linked = Path(self.root) / "linked"
        linked.symlink_to(Path(self.root), target_is_directory=True)
        self.assertEqual(
            handle(linked, "other", {"op": "init", "actor": "user"})["error"]["code"], "unsafe_path"
        )

    def test_threads(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda n: race(self.root, n), range(16)))
        self.assertEqual(sum(r["ok"] for r in results), 1)
        self.assertEqual(self.call("get", id="one")["revision"], 2)

    def test_processes(self):
        with concurrent.futures.ProcessPoolExecutor(
            max_workers=4, mp_context=multiprocessing.get_context("spawn")
        ) as pool:
            results = list(pool.map(race, [self.root] * 12, range(12)))
        self.assertEqual(sum(r["ok"] for r in results), 1)
        ticket = self.call("get", id="one")
        self.assertEqual(len(ticket["comments"]), 1)
        self.assertEqual(ticket["revision"], 2)

    def cli(self, data, *args):
        return subprocess.run(
            [
                sys.executable,
                "-m",
                "tnyboard",
                "--root",
                self.root,
                "--board",
                "demo",
                *(args or ("rpc",)),
            ],
            input=data,
            capture_output=True,
            check=False,
            cwd=REPO,
            timeout=10,
        )

    def test_rpc_one_request_one_result(self):
        for data in (
            b'{"op":"get","op":"list"}',
            b"{} {}",
            b"[]",
            b"\xff",
            b'{"op":NaN}',
            b"x" * (MAX_FILE + 1),
        ):
            result = self.cli(data)
            self.assertEqual(result.returncode, 1, result)
            self.assertEqual(len(result.stdout.splitlines()), 1)
            self.assertFalse(json.loads(result.stdout)["ok"])
        result = self.cli(b'{"op":"get"}')
        self.assertEqual(result.returncode, 0)
        self.assertTrue(json.loads(result.stdout)["ok"])

    def test_ergonomic_cli(self):
        result = self.cli(b"", "move", "one", "done", "reviewed", "--expected-revision", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["result"]["status"], "done")

    def test_render_controls_and_width(self):
        self.call("create", id="evil", title="\x1b]52;c;secret\x07\r\n\u202e\u009b日本")
        state = self.call("get")
        for width in (1, 10, 40, 100, 200):
            output = render(state, width)
            self.assertTrue(all(c == "\n" or 32 <= ord(c) < 127 for c in output))
            self.assertTrue(all(len(line) <= width for line in output.splitlines()))
        self.assertIn(r"\x1b", render(state, 200))
        result = self.cli(b"", "tui")
        self.assertEqual(result.returncode, 0)
        self.assertNotIn(b"board>", result.stdout)

    def test_interactive_tui(self):
        class TTY(io.StringIO):
            def isatty(self):
                return True

        incoming = TTY(
            'inspect one\nmove one doing "start work"\ncomment one "hello"\nrefresh\nbogus\nquit\n'
        )
        outgoing = TTY()
        self.assertEqual(tui(self.root, "demo", incoming, outgoing), 0)
        ticket = self.call("get", id="one")
        self.assertEqual(ticket["status"], "doing")
        self.assertEqual(ticket["comments"][0]["body"], "hello")
        self.assertIn("Unknown command", outgoing.getvalue())


class HTTPTest(BoardFixture):
    def setUp(self):
        super().setUp()
        self.server = make_server(self.root, "demo", "test-token-at-least-16")
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)
        super().tearDown()

    def raw(self, request):
        with socket.create_connection(self.server.server_address, timeout=8) as sock:
            sock.sendall(request)
            sock.shutdown(socket.SHUT_WR)
            response = b""
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    return response
                response += chunk

    def post(self, data, extra=b"", auth=True):
        return self.raw(
            b"POST /v1/rpc HTTP/1.0\r\nContent-Type: application/json\r\nContent-Length: "
            + str(len(data)).encode()
            + b"\r\n"
            + (b"Authorization: Bearer test-token-at-least-16\r\n" if auth else b"")
            + extra
            + b"\r\n"
            + data
        )

    def test_http_auth_rpc(self):
        response = self.post(b'{"op":"get"}')
        head, body = response.split(b"\r\n\r\n", 1)
        self.assertIn(b"200 OK", head)
        self.assertNotIn(b"Access-Control", head)
        self.assertTrue(json.loads(body)["ok"])
        self.assertIn(b"401", self.post(b'{"op":"get"}', auth=False).splitlines()[0])
        self.assertIn(
            b"403", self.post(b'{"op":"get"}', extra=b"Origin: http://evil\r\n").splitlines()[0]
        )
        self.assertEqual(self.server.server_address[0], "127.0.0.1")

    def test_http_malformed_and_durable_errors(self):
        before = self.disk()
        for data, extra in (
            (b'{"op":"get","op":"list"}', b""),
            (b"\xff", b""),
            (b"{} {}", b""),
            (b"{}", b"Content-Length: 2\r\n"),
            (b"{}", b"Transfer-Encoding: chunked\r\n"),
        ):
            response = self.post(data, extra)
            self.assertIn(b"400", response.splitlines()[0], response)
            self.assertFalse(json.loads(response.split(b"\r\n\r\n", 1)[1])["ok"])
        self.assertEqual(self.disk(), before)
        response = self.post(
            json.dumps(
                {
                    "op": "move",
                    "actor": "worker",
                    "id": "one",
                    "status": "done",
                    "reason": "test",
                    "expected_revision": 99,
                }
            ).encode()
        )
        self.assertIn(b"409", response.splitlines()[0])
        self.assertEqual(self.disk(), before)
        response = self.raw(
            b"POST /v1/rpc HTTP/1.0\r\nAuthorization: Bearer test-token-at-least-16\r\nContent-Length: 9999999999\r\n\r\n"
        )
        self.assertIn(b"413", response.splitlines()[0])
        response = self.raw(
            b"POST /v1/rpc HTTP/1.0\r\nAuthorization: Bearer test-token-at-least-16\r\nContent-Type: application/json\r\nContent-Length: 99\r\n\r\n{}"
        )
        self.assertIn(b"400", response.splitlines()[0])

    def test_http_mutation_uses_shared_api(self):
        request = {
            "op": "claim",
            "id": "one",
            "actor": "worker",
            "expected_revision": 1,
            "token": "http-claim",
        }
        response = self.post(json.dumps(request).encode())
        self.assertIn(b"200 OK", response.splitlines()[0])
        ticket = json.loads(response.split(b"\r\n\r\n", 1)[1])["result"]
        self.assertEqual(self.call("get", id="one"), ticket)
        before = self.disk()
        request.update(op="release", expected_revision=2, token="wrong")
        response = self.post(json.dumps(request).encode())
        self.assertIn(b"409", response.splitlines()[0])
        self.assertEqual(self.disk(), before)
        request["token"] = "http-claim"
        response = self.post(json.dumps(request).encode())
        self.assertIn(b"200 OK", response.splitlines()[0])
        self.assertIsNone(self.call("get", id="one")["claim"])

    def test_token_validation(self):
        for token in (None, "short", "a" * 16 + "\n", "é" * 16):
            with self.assertRaises(ValueError):
                make_server(self.root, "demo", token)


if __name__ == "__main__":
    unittest.main()
