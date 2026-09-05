#!/usr/bin/env python3
"""Speech fixtures: real HTTP parser/auth/tool path, fake local player, no live keys."""

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
AUDIO = b"ID3\x04\x00\x00\x00\x00\x00\x00fixture-mp3\x00\xff\xfa"
TOKEN = "fixture-speech-token"
ACCOUNT = "fixture-speech-account"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        state = self.server.state
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        state["requests"].append((self.path, dict(self.headers), body))
        if self.path == "/v1/chat/completions":
            self.chat(body)
            return
        if self.path == "/oauth/token":
            state["refresh"] = body
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
        if self.path != "/backend-api/pronunciation/synthesize?format=mp3":
            self.reply(404, "text/plain", b"bad path")
            return
        if (
            self.headers.get("Authorization") != f"Bearer {TOKEN}"
            or self.headers.get("chatgpt-account-id") != ACCOUNT
        ):
            self.reply(401, "application/json", b"{}")
            return
        mode = state["mode"]
        if mode == "error":
            self.reply(403, "application/json", TOKEN.encode())
        elif mode == "json":
            self.reply(
                200,
                "application/json",
                json.dumps({"base64": base64.b64encode(AUDIO).decode()}).encode(),
            )
        elif mode == "bad-json":
            self.reply(200, "application/json", b'{"error":"no audio"}')
        elif mode == "bad-base64":
            self.reply(200, "application/json", b'{"base64":"!!!!"}')
        elif mode == "html":
            self.reply(200, "text/html", b"<html>sign in</html>")
        elif mode == "empty":
            self.reply(200, "audio/mpeg", b"")
        elif mode == "truncated":
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("Content-Length", str(len(AUDIO) + 10))
            self.end_headers()
            self.wfile.write(AUDIO)
            self.close_connection = True
        elif mode == "stall":
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("Content-Length", "100")
            self.end_headers()
            state["ready"].set()
            state["release"].wait(10)
        elif mode == "oversize":
            self.reply(200, "audio/mpeg", b"ID3" + b"a" * (16 * 1024 * 1024))
        elif mode == "chunked":
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            try:
                for byte in AUDIO:
                    self.wfile.write(b"1\r\n" + bytes([byte]) + b"\r\n")
                    self.wfile.flush()
                self.wfile.write(b"0\r\n\r\n")
            except (BrokenPipeError, ConnectionResetError):
                pass
        else:
            self.reply(200, "audio/mpeg", AUDIO)

    def reply(self, status, content_type, body):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def chat(self, body):
        state = self.server.state
        state["chat"].append(body)
        if any(m.get("role") == "tool" for m in body["messages"]):
            delta = {"content": "done"}
            reason = "stop"
        else:
            name = state["tool"]
            args = (
                {"text": "Agent fixture speech."}
                if name == "speak"
                else {"command": "printf 'Agent fixture speech.' | tny speak --json"}
            )
            delta = {
                "tool_calls": [
                    {
                        "index": 0,
                        "id": "call_speech",
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
        data = (
            "".join(f"data: {json.dumps(frame)}\n\n" for frame in frames)
            + "data: [DONE]\n\n"
        )
        self.reply(200, "text/event-stream", data.encode())


class SpeechTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="tny-speech-test-")
        self.home = Path(self.tmp.name)
        self.bin = self.home / "bin"
        self.bin.mkdir()
        (self.home / "codex").mkdir()
        self.log = self.home / "played.json"
        player = self.bin / "ffplay"
        player.write_text(
            f"#!{sys.executable}\n"
            + """import json, os, sys, time
from pathlib import Path
st = os.fstat(0)
data = sys.stdin.buffer.read()
log = Path(os.environ["SPEECH_PLAYER_LOG"])
tmp = log.with_suffix(".tmp")
tmp.write_text(json.dumps({"bytes":len(data), "links":st.st_nlink, "pid":os.getpid()}))
tmp.replace(log)
print("must not leak to stdout")
print("must not leak to stderr", file=sys.stderr)
if os.environ.get("SPEECH_PLAYER_WAIT"): time.sleep(30)
sys.exit(int(os.environ.get("SPEECH_PLAYER_EXIT", "0")))
"""
        )
        player.chmod(0o700)
        self.state = {
            "mode": "raw",
            "requests": [],
            "chat": [],
            "tool": "speak",
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
            "PATH": str(self.bin),
            "TMPDIR": str(self.home),
            "LANG": "C.UTF-8",
            "CHATGPT_ACCESS_TOKEN": TOKEN,
            "CHATGPT_ACCOUNT_ID": ACCOUNT,
            "TNY_CODEX_BASE_URL": self.url + "/backend-api/codex",
            "SPEECH_PLAYER_LOG": str(self.log),
            "TNY_ISOLATE": "0",
        }
        if WASM:
            self.env["PATH"] = os.environ["PATH"]

    def tearDown(self):
        self.state["release"].set()
        self.server.shutdown()
        self.server.server_close()
        self.tmp.cleanup()

    def run_speech(self, *args, text=b"Hello, \xe4\xb8\x96\xe7\x95\x8c.\n", env=None):
        return subprocess.run(
            [TNY, "speak", *args],
            input=text,
            env=env or self.env,
            capture_output=True,
            timeout=20,
        )

    def test_export_raw_json_chunked_and_defaults(self):
        out = self.home / "speech.mp3"
        for mode in ("raw", "json", "chunked"):
            self.state["mode"] = mode
            r = self.run_speech("--output-file", str(out), "--json")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(out.read_bytes(), AUDIO)
            self.assertEqual(
                json.loads(r.stdout), {"kind": "speak", "ok": True, "played": False}
            )
            self.assertFalse(self.log.exists())
            _, headers, body = self.state["requests"][-1]
            self.assertEqual(
                body,
                {
                    "text": "Hello, 世界.\n",
                    "voice": "cove",
                    "pronunciation_language": "en-US",
                    "speed": 1.0,
                },
            )
            self.assertEqual(headers["originator"], "tny")
        self.env["TNY_CODEX_BASE_URL"] = self.url + "/backend-api/"
        r = self.run_speech("--output-file", str(out), "--voice", "glimmer")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.state["requests"][-1][2]["voice"], "glimmer")

    @unittest.skipIf(WASM or WINDOWS, "native playback")
    def test_playback_anonymous_and_errors(self):
        r = self.run_speech("--json")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr, b"")
        self.assertTrue(json.loads(r.stdout)["played"])
        played = json.loads(self.log.read_text())
        self.assertEqual(played["bytes"], len(AUDIO))
        self.assertEqual(played["links"], 0)
        self.assertFalse(list(self.home.glob("tny-speak-*")))
        self.env["SPEECH_PLAYER_EXIT"] = "9"
        r = self.run_speech()
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"playback failed", r.stderr)
        self.assertFalse(list(self.home.glob("tny-speak-*")))

    def test_bad_responses_preserve_existing_file(self):
        out = self.home / "speech.mp3"
        out.write_bytes(b"keep-existing")
        for mode in (
            "error",
            "bad-json",
            "bad-base64",
            "html",
            "empty",
            "truncated",
            "oversize",
        ):
            self.state["mode"] = mode
            r = self.run_speech("--output-file", str(out))
            self.assertNotEqual(r.returncode, 0, mode)
            self.assertEqual(out.read_bytes(), b"keep-existing")
            self.assertNotIn(TOKEN.encode(), r.stderr + r.stdout)
            self.assertFalse(list(self.home.glob("speech.mp3.tmp*")))

    def test_bad_input_and_options_never_call_network(self):
        for text in (b"", b" \n\t", b"x\0y", b"\xff", b"x" * (16 * 1024 + 1)):
            r = self.run_speech(text=text)
            self.assertEqual(r.returncode, 1, r.stderr)
        for args in (
            ("text-on-argv",),
            ("--voice",),
            ("--tts-provider", "missing"),
            ("--voice", ""),
        ):
            self.assertEqual(self.run_speech(*args).returncode, 1)
        self.assertEqual(self.state["requests"], [])

    def test_exact_text_limit_and_output_failure(self):
        out = self.home / "boundary.mp3"
        r = self.run_speech("--output-file", str(out), text=b"x" * (16 * 1024))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(out.read_bytes(), AUDIO)
        self.assertEqual(out.stat().st_mode & 0o777, 0o600)
        r = self.run_speech("--output-file", str(self.home / "missing" / "out.mp3"))
        self.assertEqual(r.returncode, 1)
        self.assertFalse(list(self.home.glob("*.XXXXXX")))

    def test_availability_and_codex_file_credentials(self):
        self.env.pop("CHATGPT_ACCESS_TOKEN")
        self.env.pop("CHATGPT_ACCOUNT_ID")
        auth = self.home / "codex/auth.json"
        for value in (
            None,
            {},
            {"OPENAI_API_KEY": "sk-fixture"},
            {"tokens": {"access_token": TOKEN}},
        ):
            if value is not None:
                auth.write_text(json.dumps(value))
            r = self.run_speech("--check", "--json")
            self.assertEqual(r.returncode, 1)
            self.assertFalse(json.loads(r.stdout)["available"])
        auth.write_text(
            json.dumps({"tokens": {"access_token": TOKEN, "account_id": ACCOUNT}})
        )
        r = self.run_speech("--check", "--json", "--output-file", "unused.mp3")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.state["requests"], [])
        r = self.run_speech("--output-file", str(self.home / "file.mp3"))
        self.assertEqual(r.returncode, 0, r.stderr)
        if not WASM:
            self.env["PATH"] = ""
            self.assertEqual(self.run_speech("--check").returncode, 1)
            r = self.run_speech("--output-file", str(self.home / "file.mp3"))
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_store_refresh_and_flag_precedence(self):
        self.env.pop("CHATGPT_ACCESS_TOKEN")
        self.env.pop("CHATGPT_ACCOUNT_ID")
        store = self.home / ".tny/codex-auth.json"
        store.parent.mkdir()
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
        self.assertEqual(
            self.run_speech("--check", "--output-file", "unused").returncode, 0
        )
        self.assertEqual(self.state["requests"], [])
        r = self.run_speech("--output-file", str(self.home / "file.mp3"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.state["refresh"]["refresh_token"], "refresh-fixture")
        self.assertEqual(
            json.loads(store.read_text())["tokens"]["refresh_token"], "rotated"
        )
        self.env["CHATGPT_ACCESS_TOKEN"] = "wrong-env-token"
        r = subprocess.run(
            [
                TNY,
                "--provider",
                "openai",
                "--chatgpt-token",
                TOKEN,
                "--chatgpt-account-id",
                ACCOUNT,
                "speak",
                "--output-file",
                str(self.home / "file.mp3"),
            ],
            input=b"Flag credential.",
            env=self.env,
            capture_output=True,
            timeout=20,
        )
        self.assertEqual(r.returncode, 0, r.stderr)

    @unittest.skipIf(WASM or WINDOWS, "native signals")
    def test_cancellation_network_and_player(self):
        for phase in ("network", "player", "group"):
            if self.log.exists():
                self.log.unlink()
            if phase == "network":
                self.state["mode"] = "stall"
            else:
                self.state["mode"] = "raw"
                self.env["SPEECH_PLAYER_WAIT"] = "1"
            p = subprocess.Popen(
                [TNY, "speak"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=self.env,
                start_new_session=True,
            )
            p.stdin.write(b"Cancellation fixture.")
            p.stdin.close()
            deadline = time.monotonic() + 5
            while not (
                self.state["ready"].is_set()
                if phase == "network"
                else self.log.exists()
            ):
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.01)
            if phase == "group":
                os.killpg(p.pid, signal.SIGINT)
            else:
                p.send_signal(signal.SIGINT)
            p.wait(timeout=5)
            self.assertEqual(p.returncode, 130, p.stderr.read())
            self.state["release"].set()
            self.assertFalse(list(self.home.glob("tny-speak-*")))
            if phase != "network":
                with self.assertRaises(ProcessLookupError):
                    os.kill(json.loads(self.log.read_text())["pid"], 0)
            p.stdout.close()
            p.stderr.close()

    @unittest.skipIf(WASM or WINDOWS, "native tool cancellation")
    def test_agent_speech_cancellation_stops_next_model_call(self):
        self.env.update(
            {
                "OPENAI_API_KEY": "fixture-chat-key",
                "OPENAI_BASE_URL": self.url + "/v1",
                "OPENAI_WIRE_API": "chat",
                "TNY_TOOLS": "all",
                "SPEECH_PLAYER_WAIT": "1",
            }
        )
        p = subprocess.Popen(
            [
                TNY,
                "--provider",
                "openai",
                "--cwd",
                str(self.home),
                "ask",
                "--ephemeral",
                "--yolo",
                "fixture speech",
            ],
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
        try:
            deadline = time.monotonic() + 5
            while not self.log.exists():
                self.assertIsNone(p.poll())
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.01)
            p.send_signal(signal.SIGINT)
            _, err = p.communicate(timeout=5)
            self.assertEqual(p.returncode, 130, err)
            self.assertEqual(len(self.state["chat"]), 1)
            with self.assertRaises(ProcessLookupError):
                os.kill(json.loads(self.log.read_text())["pid"], 0)
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()

    @unittest.skipIf(WASM or WINDOWS, "native tool playback")
    def test_other_chat_provider_typed_shell_and_unavailable(self):
        self.env.update(
            {
                "OPENAI_API_KEY": "fixture-chat-key",
                "OPENAI_BASE_URL": self.url + "/v1",
                "OPENAI_WIRE_API": "chat",
            }
        )
        for tool, profile, logged_in in (
            ("speak", "all", True),
            ("terminal", "terminal", True),
            ("speak", "all", False),
        ):
            self.state["chat"].clear()
            self.state["requests"].clear()
            self.state["tool"] = tool
            if self.log.exists():
                self.log.unlink()
            if not logged_in:
                self.env.pop("CHATGPT_ACCESS_TOKEN")
            self.env["TNY_TOOLS"] = profile
            r = subprocess.run(
                [
                    TNY,
                    "--provider",
                    "openai",
                    "--cwd",
                    str(self.home),
                    "ask",
                    "--ephemeral",
                    "--yolo",
                    "fixture speech",
                ],
                env=self.env,
                capture_output=True,
                timeout=30,
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            names = [t["function"]["name"] for t in self.state["chat"][0]["tools"]]
            self.assertEqual("speak" in names, tool == "speak" and logged_in)
            self.assertEqual(self.log.exists(), logged_in)
            speech = [
                req for req in self.state["requests"] if "pronunciation" in req[0]
            ]
            self.assertEqual(len(speech), int(logged_in))
            for path, headers, _ in self.state["requests"]:
                self.assertEqual(
                    headers["Authorization"],
                    f"Bearer {TOKEN}"
                    if "pronunciation" in path
                    else "Bearer fixture-chat-key",
                )


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
