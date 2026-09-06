#!/usr/bin/env python3
"""Image CLI/tool contract against real HTTP fixtures; no live credentials."""

from __future__ import annotations

import base64
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TNY = str(Path(os.environ.get("TNY", ROOT / "build/tny")).resolve())
WASM = "wasm" in TNY
WINDOWS = sys.platform in ("win32", "cygwin", "msys")
PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a0xoAAAAASUVORK5CYII="
)
TOKEN = "fixture-image-token"
ACCOUNT = "fixture-image-account"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, status, ct, body):
        self.send_response(status)
        self.send_header("Content-Type", ct)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        s = self.server.state
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        s["requests"].append((self.path, dict(self.headers), body))
        if self.path == "/oauth/token":
            self.reply(
                200,
                "application/json",
                json.dumps(
                    {
                        "access_token": TOKEN,
                        "refresh_token": "rotated",
                        "expires_in": 3600,
                    }
                ).encode(),
            )
            return
        if self.path == "/v1/chat/completions":
            self.chat(body)
            return
        if self.path not in (
            "/backend-api/codex/images/generations",
            "/backend-api/codex/images/edits",
        ):
            self.reply(404, "text/plain", b"bad route")
            return
        if (
            self.headers.get("Authorization") != f"Bearer {TOKEN}"
            or self.headers.get("chatgpt-account-id") != ACCOUNT
        ):
            self.reply(401, "application/json", b"{}")
            return
        mode = s["mode"]
        data = json.dumps(
            {"created": 1, "data": [{"b64_json": base64.b64encode(PNG).decode()}]}
        ).encode()
        if isinstance(mode, int):
            self.reply(mode, "application/json", TOKEN.encode())
        elif mode == "html":
            self.reply(200, "text/html", b"<html>login</html>")
        elif mode in (
            "empty",
            "bad-json",
            "bad-base64",
            "non-image",
            "nul",
            "multiple",
            "padding",
            "oversize",
        ):
            value = {
                "empty": b"",
                "bad-json": b'{"data":[]}',
                "bad-base64": b'{"data":[{"b64_json":"!!!!"}]}',
                "non-image": b'{"data":[{"b64_json":"YWJjZGVmZ2hpamts"}]}',
                "nul": b'{"data":[{"b64_json":"iVBORw0KGgoAAAAN\\u0000"}]}',
                "multiple": b'{"data":[{"b64_json":"iVBORw0KGgoAAAAN"},{"b64_json":"iVBORw0KGgoAAAAN"}]}',
                "padding": b'{"data":[{"b64_json":"iVBORw0KGgoAAAAN===="}]}',
                "oversize": b" " * (45 * 1024 * 1024) if mode == "oversize" else b"",
            }[mode]
            self.reply(200, "application/json", value)
        elif mode in ("stall", "truncated"):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data) + 20))
            self.end_headers()
            if mode == "stall":
                s["ready"].set()
                s["release"].wait(10)
            else:
                self.wfile.write(data)
            self.close_connection = True
        elif mode == "chunked":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for byte in data:
                self.wfile.write(b"1\r\n" + bytes([byte]) + b"\r\n")
                self.wfile.flush()
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.reply(200, "application/json", data)

    def chat(self, body):
        s = self.server.state
        s["chat"].append(body)
        if any(m.get("role") == "tool" for m in body["messages"]):
            delta, reason = {"content": "done"}, "stop"
        else:
            op = s.get("operation", "generate")
            name = "image_" + op
            args = {"prompt": "A blue robot", "output_file": "agent.png"}
            if op == "edit":
                args["images"] = ["input.png"]
            if s.get("tool") == "terminal":
                name = "terminal"
                ref = " --image input.png" if op == "edit" else ""
                args = {
                    "command": f"printf 'A blue robot' | tny image {op}{ref} --output-file agent.png --json"
                }
            delta = {
                "tool_calls": [
                    {
                        "index": 0,
                        "id": "image-call",
                        "type": "function",
                        "function": {"name": name, "arguments": json.dumps(args)},
                    }
                ]
            }
            reason = "tool_calls"
        frames = [
            {"choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
            {"choices": [{"index": 0, "delta": {}, "finish_reason": reason}]},
        ]
        self.reply(
            200,
            "text/event-stream",
            (
                "".join(f"data: {json.dumps(f)}\n\n" for f in frames)
                + "data: [DONE]\n\n"
            ).encode(),
        )


class ImageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="tny-images-")
        self.home = Path(self.tmp.name)
        (self.home / "codex").mkdir()
        (self.home / "input.png").write_bytes(PNG)
        self.out = self.home / "result.png"
        self.state = {
            "mode": "ok",
            "requests": [],
            "chat": [],
            "ready": threading.Event(),
            "release": threading.Event(),
        }
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.state = self.state
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.server.server_port}"
        self.env = {
            "HOME": str(self.home),
            "CODEX_HOME": str(self.home / "codex"),
            "PATH": os.environ["PATH"],
            "LANG": "C.UTF-8",
            "CHATGPT_ACCESS_TOKEN": TOKEN,
            "CHATGPT_ACCOUNT_ID": ACCOUNT,
            "TNY_CODEX_BASE_URL": self.url + "/backend-api/codex",
            "TNY_ISOLATE": "0",
        }

    def tearDown(self):
        self.state["release"].set()
        self.server.shutdown()
        self.server.server_close()
        self.tmp.cleanup()

    def run_image(
        self, *args, prompt=b"A blue robot, \xe4\xb8\x96\xe7\x95\x8c\n", globals_=()
    ):
        return subprocess.run(
            [TNY, *globals_, "image", *args],
            input=prompt,
            cwd=self.home,
            env=self.env,
            capture_output=True,
            timeout=30,
        )

    def generate(self, *args, **kw):
        return self.run_image("generate", "--output-file", str(self.out), *args, **kw)

    def image_requests(self):
        return [r for r in self.state["requests"] if "/images/" in r[0]]

    def test_generate_defaults_chunk_boundaries_and_metadata(self):
        for mode in ("ok", "chunked"):
            self.state["mode"] = mode
            r = self.generate("--json")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(self.out.read_bytes(), PNG)
            self.assertEqual(
                json.loads(r.stdout),
                {
                    "kind": "image",
                    "ok": True,
                    "operation": "generate",
                    "provider": "codex",
                    "model": "gpt-image-2",
                    "path": str(self.out),
                    "mime_type": "image/png",
                    "bytes": len(PNG),
                },
            )
            path, headers, body = self.image_requests()[-1]
            self.assertEqual(path, "/backend-api/codex/images/generations")
            self.assertEqual(headers["originator"], "tny")
            self.assertEqual(
                body,
                {
                    "model": "gpt-image-2",
                    "prompt": "A blue robot, 世界\n",
                    "quality": "auto",
                    "size": "auto",
                    "background": "auto",
                },
            )
            if not WINDOWS:
                self.assertEqual(self.out.stat().st_mode & 0o777, 0o600)
        self.assertFalse(list(self.home.glob("result.png.*")))

    def test_edit_reference_payload_options_and_in_place(self):
        image = self.home / "input.png"
        r = self.run_image(
            "edit",
            "--image",
            str(image),
            "--image",
            str(image),
            "--output-file",
            str(image),
            "--quality",
            "low",
            "--size",
            "1024x1024",
            "--model",
            "fixture-image-model",
            "--json",
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(image.read_bytes(), PNG)
        path, _, body = self.image_requests()[0]
        self.assertEqual(path, "/backend-api/codex/images/edits")
        self.assertEqual(
            body["images"],
            [{"image_url": "data:image/png;base64," + base64.b64encode(PNG).decode()}]
            * 2,
        )
        self.assertEqual(body["quality"], "low")
        self.assertEqual(body["size"], "1024x1024")
        self.assertEqual(json.loads(r.stdout)["model"], "fixture-image-model")
        self.env["TNY_CODEX_BASE_URL"] += "/"
        args = ["--image", str(image)] * 5
        r = self.run_image("edit", *args, "--output-file", str(self.out))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(self.image_requests()[-1][2]["images"]), 5)

    def test_invalid_requests_and_files_do_not_spend_quota(self):
        for data in (b"", b" \t\n", b"\xff", b"x\0y", b"x" * 16385):
            self.assertEqual(self.generate(prompt=data).returncode, 1)
        for args in (
            ("--quality", "wrong"),
            ("--image-provider", "grok"),
            ("--image", "input.png"),
            ("--output-file", "twice"),
            ("--model", ""),
            ("positional-prompt",),
        ):
            self.assertEqual(self.generate(*args).returncode, 1, args)
        self.assertEqual(self.run_image("generate", prompt=b"x").returncode, 1)
        for args in (
            (),
            ("--image", "missing"),
            ("--image", str(self.home)),
            tuple(["--image", "input.png"] * 6),
        ):
            self.assertEqual(
                self.run_image(
                    "edit", *args, "--output-file", str(self.out)
                ).returncode,
                1,
            )
        huge = self.home / "huge.png"
        with huge.open("wb") as f:
            f.write(PNG)
            f.truncate(8 * 1024 * 1024 + 1)
        for path in (huge, self.home / "bad.png"):
            if not path.exists():
                path.write_bytes(b"plain text")
            self.assertEqual(
                self.run_image(
                    "edit", "--image", str(path), "--output-file", str(self.out)
                ).returncode,
                1,
            )
        self.assertEqual(
            self.run_image("generate", "--output-file", "missing/out.png").returncode, 1
        )
        self.assertEqual(
            self.run_image("generate", "--output-file", str(self.home)).returncode, 1
        )
        self.assertEqual(self.image_requests(), [])
        r = self.generate(prompt=b"x" * 16384)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_exact_reference_limit_is_accepted(self):
        image = self.home / "limit.png"
        with image.open("wb") as f:
            f.write(PNG)
            f.truncate(8 * 1024 * 1024)
        r = self.run_image(
            "edit", "--image", str(image), "--output-file", str(self.out)
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        encoded = self.image_requests()[0][2]["images"][0]["image_url"].split(",", 1)[1]
        self.assertEqual(len(base64.b64decode(encoded)), 8 * 1024 * 1024)

    def test_errors_never_replace_output_or_leak_bearer(self):
        self.out.write_bytes(b"keep me")
        for mode in (
            401,
            403,
            429,
            500,
            "html",
            "empty",
            "bad-json",
            "bad-base64",
            "non-image",
            "nul",
            "multiple",
            "padding",
            "truncated",
            "oversize",
        ):
            self.state["mode"] = mode
            r = self.generate("--json")
            self.assertEqual(
                r.returncode, 2 if isinstance(mode, int) else 1, (mode, r.stderr)
            )
            self.assertEqual(self.out.read_bytes(), b"keep me")
            self.assertNotIn(TOKEN.encode(), r.stdout + r.stderr)
            self.assertEqual(r.stdout, b"")
            self.assertFalse(list(self.home.glob("result.png.*")))
        self.assertEqual(len(self.image_requests()), 14)  # no automatic retries

    def test_check_auth_refresh_precedence_and_chat_independence(self):
        self.env.pop("CHATGPT_ACCESS_TOKEN")
        self.env.pop("CHATGPT_ACCOUNT_ID")
        store = self.home / "codex/auth.json"
        store.write_text(json.dumps({"OPENAI_API_KEY": "fixture-public-key"}))
        r = self.run_image("generate", "--check", "--json")
        self.assertEqual(r.returncode, 1)
        self.assertFalse(json.loads(r.stdout)["available"])
        store.write_text(
            json.dumps(
                {
                    "tokens": {
                        "access_token": "expired",
                        "account_id": ACCOUNT,
                        "refresh_token": "refresh-fixture",
                    },
                    "last_refresh": "2020-01-01T00:00:00Z",
                }
            )
        )
        self.env["CODEX_REFRESH_TOKEN_URL_OVERRIDE"] = self.url + "/oauth/token"
        self.assertEqual(self.run_image("edit", "--check").returncode, 0)
        self.assertEqual(self.state["requests"], [])
        r = self.generate()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(
            json.loads(store.read_text())["tokens"]["refresh_token"], "rotated"
        )
        self.env["CHATGPT_ACCESS_TOKEN"] = "wrong-env-token"
        r = self.generate(
            globals_=(
                "--provider",
                "openai",
                "--base-url",
                "http://127.0.0.1:1/never",
                "--chatgpt-token",
                TOKEN,
                "--chatgpt-account-id",
                ACCOUNT,
            )
        )
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_cwd_and_help_without_login(self):
        self.env.pop("CHATGPT_ACCESS_TOKEN")
        for args in (
            ("--help",),
            ("generate", "--help"),
            ("edit", "--help"),
            ("attach", "--help"),
        ):
            self.assertEqual(self.run_image(*args).returncode, 0)
        self.assertEqual(self.image_requests(), [])
        self.env["CHATGPT_ACCESS_TOKEN"] = TOKEN
        sub = self.home / "subdir"
        sub.mkdir()
        r = self.run_image(
            "generate", "--output-file", "out.png", globals_=("--cwd", str(sub))
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((sub / "out.png").read_bytes(), PNG)
        self.assertEqual(self.generate(globals_=("--ssh", "not-a-host")).returncode, 1)

    @unittest.skipIf(WINDOWS or WASM, "native FIFO/symlink semantics")
    def test_special_files_rejected_before_network(self):
        fifo = self.home / "pipe"
        os.mkfifo(fifo)
        r = self.run_image("edit", "--image", str(fifo), "--output-file", str(self.out))
        self.assertEqual(r.returncode, 1)
        self.out.symlink_to(self.home / "input.png")
        self.assertEqual(self.generate().returncode, 1)
        self.assertEqual(self.image_requests(), [])

    def agent_args(self, profile):
        self.env.update(
            {
                "OPENAI_API_KEY": "fixture-chat-key",
                "OPENAI_BASE_URL": self.url + "/v1",
                "OPENAI_WIRE_API": "chat",
                "TNY_TOOLS": profile,
            }
        )
        return [
            TNY,
            "--provider",
            "openai",
            "--cwd",
            str(self.home),
            "ask",
            "--ephemeral",
            "--yolo",
            "fixture image",
        ]

    def test_typed_and_shell_agents_discover_and_execute_both_operations(self):
        profiles = ("all",) if WASM else ("all", "terminal", "terminal+edit")
        for profile in profiles:
            for op in ("generate", "edit"):
                self.state["chat"].clear()
                self.state["requests"].clear()
                self.state.update(
                    {
                        "tool": "terminal" if profile != "all" else "typed",
                        "operation": op,
                    }
                )
                r = subprocess.run(
                    self.agent_args(profile),
                    env=self.env,
                    capture_output=True,
                    timeout=30,
                )
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual((self.home / "agent.png").read_bytes(), PNG)
                chats = self.state["chat"]
                names = [t["function"]["name"] for t in chats[0]["tools"]]
                self.assertEqual("image_generate" in names, profile == "all")
                self.assertEqual("image_edit" in names, profile == "all")
                self.assertIn("tny image generate", chats[0]["messages"][0]["content"])
                self.assertEqual(len(self.image_requests()), 1)
                result = next(
                    m["content"]
                    for m in chats[1]["messages"]
                    if m.get("role") == "tool"
                )
                self.assertIn('"ok":true', result)
                self.assertNotIn(base64.b64encode(PNG).decode(), result)
                for path, headers, _ in self.state["requests"]:
                    self.assertEqual(
                        headers["Authorization"],
                        f"Bearer {TOKEN}"
                        if "/images/" in path
                        else "Bearer fixture-chat-key",
                    )

    @unittest.skipIf(WASM or WINDOWS, "native signals")
    def test_cancellation_preserves_output_and_stops_agent_followup(self):
        self.state["mode"] = "stall"
        for agent in (False, True):
            self.state["ready"].clear()
            self.state["release"].clear()
            self.state["chat"].clear()
            self.out.write_bytes(b"keep me")
            args = (
                self.agent_args("all")
                if agent
                else [TNY, "image", "generate", "--output-file", str(self.out)]
            )
            p = subprocess.Popen(
                args,
                env=self.env,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                cwd=self.home,
            )
            try:
                p.stdin.write(b"A robot")
                p.stdin.close()
                self.assertTrue(self.state["ready"].wait(10))
                p.send_signal(signal.SIGINT)
                p.wait(timeout=5)
                self.assertEqual(p.returncode, 130, p.stderr.read())
                self.assertEqual(self.out.read_bytes(), b"keep me")
                self.assertFalse(list(self.home.glob("*.png.??????")))
                if agent:
                    self.assertEqual(len(self.state["chat"]), 1)
            finally:
                self.state["release"].set()
                if p.poll() is None:
                    p.kill()
                    p.wait()
                p.stdout.close()
                p.stderr.close()
            time.sleep(0.05)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
