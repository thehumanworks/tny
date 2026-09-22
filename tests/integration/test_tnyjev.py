#!/usr/bin/env python3
"""tnyjev CLI/wire contract. Loopback HTTP and synthetic credentials only."""

from __future__ import annotations

import json
import os
import signal
import subprocess
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TNY = str(Path(os.environ.get("TNY", ROOT / "build/tny")).resolve())
WASM = "wasm" in TNY
TOKEN = "fixture-jev-secret"
SCORE = {
    "model": "jev-1.13.0",
    "answers": {"decision": {"type": "noul", "noul": 0.75}},
    "usage": {"input_tokens": 12, "output_tokens": 3},
}
CHOICE = {
    **SCORE,
    "answers": {
        "decision": {
            "type": "choice",
            "choice": "billing",
            "probabilities": {"support": 0.25, "billing": 0.75},
            "confidence": 0.8,
        }
    },
}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        s = self.server.state
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        s["requests"].append((self.path, dict(self.headers), request))
        s.setdefault("user_agents", []).append(self.headers.get_all("User-Agent", []))
        s["ready"].set()
        if s["stall"]:
            s["release"].wait(5)
            return
        body = s["body"]
        self.send_response(s["status"])
        self.send_header("Content-Type", s["content_type"])
        if s["chunked"]:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(len(body) + s["truncate"]))
        self.end_headers()
        try:
            if s["chunked"]:
                for b in body:
                    self.wfile.write(b"1\r\n" + bytes([b]) + b"\r\n")
                    self.wfile.flush()
                self.wfile.write(b"0\r\n\r\n")
            else:
                split = s["split"]
                if split is not None:
                    self.wfile.write(body[:split])
                    self.wfile.flush()
                    time.sleep(0.001)
                    self.wfile.write(body[split:])
                else:
                    self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True


class JevTests(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.TemporaryDirectory(prefix="tnyjev-")
        self.addCleanup(self.home.cleanup)
        self.state = {
            "requests": [],
            "body": json.dumps(SCORE).encode(),
            "status": 200,
            "content_type": "application/json; charset=utf-8",
            "chunked": False,
            "truncate": 0,
            "split": None,
            "stall": False,
            "ready": threading.Event(),
            "release": threading.Event(),
        }
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.server.state = self.state
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop_server)
        self.env = {
            **os.environ,
            "HOME": self.home.name,
            "TYPESAFE_API_KEY": TOKEN,
            "TNY_JEV_URL": f"http://127.0.0.1:{self.server.server_port}/v1/systemone",
            "TNY_JEV_MODEL": "jev-latest",
        }

    def stop_server(self):
        self.state["release"].set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(5)

    def run_tny(self, *args, state="Production down", env=None):
        return subprocess.run(
            [TNY, *args],
            input=state,
            text=True,
            capture_output=True,
            env=self.env if env is None else env,
            timeout=15,
        )

    def assert_failed(self, p):
        self.assertNotEqual(p.returncode, 0, p.stdout)
        self.assertEqual(p.stdout, "")
        self.assertTrue(p.stderr)
        self.assertNotIn(TOKEN, p.stderr)

    def test_score_plain_and_json_and_wire_contract(self):
        p = self.run_tny("score", "Is this urgent?")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, "0.75\n")
        path, headers, body = self.state["requests"][-1]
        self.assertEqual(path, "/v1/systemone")
        self.assertEqual(headers["Authorization"], f"Bearer {TOKEN}")
        self.assertLessEqual(len(self.state["user_agents"][-1]), 1)
        self.assertEqual(
            body,
            {
                "model": "jev-latest",
                "state": "Production down",
                "questions": {
                    "decision": {"type": "noul", "instructions": "Is this urgent?"}
                },
            },
        )
        p = self.run_tny(
            "--json", "score", "Is this urgent?", "--state", 'A "quote"\n世界'
        )
        self.assertEqual(p.returncode, 0, p.stderr)
        result = json.loads(p.stdout)
        self.assertEqual(result["score"], 0.75)
        self.assertEqual(result["primitive"], "noul")
        self.assertEqual(result["model"], SCORE["model"])
        self.assertEqual(result["usage"], SCORE["usage"])
        self.assertEqual(self.state["requests"][-1][2]["state"], 'A "quote"\n世界')

    def test_choose_is_router_with_structured_criteria(self):
        self.state["body"] = json.dumps(CHOICE).encode()
        choices = {
            "billing": {"description": "Payments"},
            "support": ["Bugs", "Outages"],
        }
        p = self.run_tny("choose", "--choices", json.dumps(choices))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, "billing\n")
        q = self.state["requests"][-1][2]["questions"]["decision"]
        self.assertEqual(q["type"], "choice")
        self.assertEqual(q["criteria"], choices)
        p = self.run_tny(
            "choose", "Which team?", "--choices", json.dumps(choices), "--json"
        )
        self.assertEqual(p.returncode, 0, p.stderr)
        result = json.loads(p.stdout)
        self.assertEqual(result["choice"], "billing")
        self.assertEqual(result["probabilities"], {"billing": 0.75, "support": 0.25})
        self.assertEqual(result["confidence"], 0.8)
        self.assertEqual(result["kind"], "choose")

    def test_state_json_and_model_precedence(self):
        for flag, state in [
            ("--state-json", {"text": "help"}),
            ("--stdin-json", ["help"]),
        ]:
            with self.subTest(flag=flag):
                args = ["score", "Urgent?", flag]
                if flag == "--state-json":
                    args.append(json.dumps(state))
                args += ["--model", "jev-1.13.0"]
                p = self.run_tny(*args, state=json.dumps(state))
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertEqual(self.state["requests"][-1][2]["state"], state)
                self.assertEqual(self.state["requests"][-1][2]["model"], "jev-1.13.0")
        self.env["TNY_JEV_MODEL"] = "jev-env-model"
        p = self.run_tny("score", "Urgent?")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(self.state["requests"][-1][2]["model"], "jev-env-model")

    def test_missing_key_and_help_do_not_open_a_session_or_network(self):
        self.env.pop("TYPESAFE_API_KEY")
        for cmd in ("score", "choose"):
            p = self.run_tny(cmd, "--help")
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn("TYPESAFE_API_KEY", p.stdout)
        p = self.run_tny("score", "Urgent?")
        self.assert_failed(p)
        self.assertIn("TYPESAFE_API_KEY", p.stderr)
        self.assertEqual(self.state["requests"], [])
        self.assertFalse((Path(self.home.name) / ".tny").exists())

    def test_independent_of_broken_chat_settings(self):
        home = Path(self.home.name) / ".tny"
        home.mkdir()
        settings = home / "settings.json"
        settings.write_text('{"provider":"missing-chat-profile"}')
        p = self.run_tny("score", "Urgent?")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertFalse((home / "sessions").exists())
        self.assertEqual(settings.read_text(), '{"provider":"missing-chat-profile"}')

    def test_invalid_inputs_fail_before_http(self):
        cases = [
            ["score"],
            ["score", "Urgent?", "--state", "x", "--stdin"],
            ["score", "Urgent?", "--timeout", "0"],
            ["score", "Urgent?", "--timeout", "301"],
            ["score", "Urgent?", "--timeout", "1s"],
            ["score", "Urgent?", "--state-json", "true"],
            ["score", "Urgent?", "--state-json", "{} garbage"],
            ["score", "Urgent?", "--choices", "{}"],
            ["choose", "--choices", "[]"],
            ["choose", "--choices", "{}"],
            ["choose", "--choices", '{"same":null,"same":null}'],
            ["choose", "--choices", '{"bad":123}'],
            ["choose", "--choices", '{"bad\\u0000key":null}'],
            ["choose", "--choices", json.dumps({str(i): None for i in range(256)})],
        ]
        for args in cases:
            with self.subTest(args=args):
                self.assert_failed(self.run_tny(*args))
        self.assertEqual(self.state["requests"], [])

    def test_bad_or_oversized_stdin_fails_before_http(self):
        for state in ("", "a\x00b", "x" * (1024 * 1024 + 1)):
            with self.subTest(size=len(state)):
                self.assert_failed(self.run_tny("score", "Urgent?", state=state))
        self.assertEqual(self.state["requests"], [])

    def test_http_failures_are_not_retried_or_echoed(self):
        self.state["body"] = TOKEN.encode()
        for status in (301, 401, 403, 422, 429, 500, 529):
            with self.subTest(status=status):
                self.state["status"] = status
                before = len(self.state["requests"])
                p = self.run_tny("score", "Urgent?")
                self.assert_failed(p)
                self.assertIn(str(status), p.stderr)
                self.assertEqual(len(self.state["requests"]), before + 1)

    def test_invalid_responses_fail_closed(self):
        for body in (b"{}", b"not-json", b"null", b"x" * (1024 * 1024 + 1)):
            self.state["body"] = body
            self.assert_failed(self.run_tny("score", "Urgent?"))
        self.state["body"] = json.dumps(SCORE).encode()
        self.state["content_type"] = "text/html"
        self.assert_failed(self.run_tny("score", "Urgent?"))
        self.state["content_type"] = "application/json"
        self.state["truncate"] = 5
        self.assert_failed(self.run_tny("score", "Urgent?"))

    def test_chunked_one_byte_chunks(self):
        self.state["chunked"] = True
        p = self.run_tny("score", "Urgent?")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, "0.75\n")

    def test_every_body_split_boundary(self):
        # Actual HTTP reads may coalesce, so combine this with one-byte chunks
        # and the shared parser's deterministic every-split unit suite.
        for split in range(1, len(self.state["body"])):
            with self.subTest(split=split):
                self.state["split"] = split
                p = self.run_tny("score", "Urgent?")
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertEqual(p.stdout, "0.75\n")

    def test_timeout(self):
        self.state["stall"] = True
        p = self.run_tny("score", "Urgent?", "--timeout", "1")
        self.assert_failed(p)
        self.assertIn("timed out", p.stderr)

    @unittest.skipIf(WASM or os.name == "nt", "native POSIX signal contract")
    def test_cancel(self):
        self.state["stall"] = True
        with subprocess.Popen(
            [TNY, "score", "Urgent?", "--state", "help"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=self.env,
        ) as p:
            try:
                self.assertTrue(self.state["ready"].wait(5))
                p.send_signal(signal.SIGINT)
                out, err = p.communicate(timeout=5)
                self.assertEqual(p.returncode, 130, err)
                self.assertEqual(out, "")
                self.assertIn("interrupted", err)
            finally:
                if p.poll() is None:
                    p.kill()
                    p.wait()


if __name__ == "__main__":
    unittest.main(argv=[__file__])
