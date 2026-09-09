#!/usr/bin/env python3
"""Account STT + independent chat, real HTTP/PTYs, fake microphone; no live auth."""

from __future__ import annotations

import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import wave
from email import policy
from email.parser import BytesParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TNY = str(Path(os.environ.get("TNY", ROOT / "build/tny")).resolve())
WASM = "wasm" in TNY
MICROPHONE = not WASM and sys.platform not in ("win32", "cygwin", "msys")
TOKEN, ACCOUNT = "fixture-dictation-token", "fixture-dictation-account"
TEXT = "Check the changes, 世界."
OPTIMISED = "Review the changes and report regressions, 世界."


def wav_bytes():
    out = io.BytesIO()
    with wave.open(out, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(24000)
        f.writeframes(b"\x00\x10\x00\xf0" * 12000)
    return out.getvalue()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, status, data, mime="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        state = self.server.state
        body = self.rfile.read(int(self.headers["Content-Length"]))
        headers = {k.lower(): v for k, v in self.headers.items()}
        state["requests"].append((self.path, headers, body))
        if self.path == "/v1/chat/completions":
            request = json.loads(body)
            state["chat"].append(request)
            optimising = any(
                "You optimise a draft prompt" in str(m.get("content"))
                for m in request["messages"]
            )
            frames = [
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "content": OPTIMISED if optimising else "CHAT-OK"
                            },
                            "finish_reason": None,
                        }
                    ]
                },
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]},
            ]
            data = (
                "".join(f"data: {json.dumps(f)}\n\n" for f in frames)
                + "data: [DONE]\n\n"
            )
            self.reply(200, data.encode(), "text/event-stream")
            return
        if self.path == "/oauth/token":
            state["refresh"] = json.loads(body)
            self.reply(
                200,
                json.dumps(
                    {
                        "access_token": TOKEN,
                        "refresh_token": "rotated",
                        "expires_in": 3600,
                    }
                ).encode(),
            )
            return
        if self.path != "/backend-api/transcribe":
            self.reply(404, b"{}")
            return
        if (
            headers.get("authorization") != f"Bearer {TOKEN}"
            or headers.get("chatgpt-account-id") != ACCOUNT
        ):
            self.reply(401, b"{}")
            return
        msg = BytesParser(policy=policy.default).parsebytes(
            f"Content-Type: {headers['content-type']}\r\nMIME-Version: 1.0\r\n\r\n".encode()
            + body
        )
        parts = list(msg.iter_parts())
        state["uploads"].append(parts)
        mode = state["mode"]
        if isinstance(mode, int):
            self.reply(mode, TOKEN.encode())
        elif mode == "stall":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "100")
            self.end_headers()
            state["ready"].set()
            state["release"].wait(15)
        elif mode == "truncated":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "100")
            self.end_headers()
            self.wfile.write(b'{"text":"partial"}')
            self.close_connection = True
        elif mode == "html":
            self.reply(200, b"<html>sign in</html>", "text/html")
        elif mode == "oversize-wire":
            self.reply(200, b" " * (64 * 1024 * 6 + 1025))
        elif mode == "malformed":
            self.reply(200, b'{"text":')
        elif mode == "chunked":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            data = json.dumps({"text": state["text"]}, ensure_ascii=False).encode()
            for byte in data:
                self.wfile.write(b"1\r\n" + bytes([byte]) + b"\r\n")
                self.wfile.flush()
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.reply(
                200, json.dumps({"text": state["text"]}, ensure_ascii=False).encode()
            )


class DictationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="tny-dictation-test-")
        self.home = Path(self.tmp.name)
        self.bin = self.home / "bin"
        self.bin.mkdir()
        (self.home / "codex").mkdir()
        self.wav = self.home / "known.wav"
        self.wav.write_bytes(wav_bytes())
        self.log = self.home / "recorder.json"
        script = (
            f"#!{sys.executable}\n"
            + """import json, os, signal, sys, time
from pathlib import Path
mode = os.environ.get("DICTATION_RECORDER_MODE", "normal")
log = Path(os.environ["DICTATION_RECORDER_LOG"])
temp = log.with_suffix(".tmp")
temp.write_text(json.dumps({"pid":os.getpid(), "argv":sys.argv}))
temp.replace(log)
if mode == "fail": sys.exit(12)
if mode == "hang": signal.signal(signal.SIGTERM, signal.SIG_IGN)
if mode == "short":
    os.write(1, b"x" * 10)
elif mode == "odd":
    os.write(1, b"x" * 48001)
elif mode == "flood":
    for _ in range(1000): os.write(1, b"x" * 65536)
else:
    os.write(1, b"\\x00\\x10\\x00\\xf0" * 12000)
while True: time.sleep(1)
"""
        )
        for name in ("ffmpeg", "arecord"):
            path = self.bin / name
            path.write_text(script)
            path.chmod(0o700)
        self.state = {
            "mode": "normal",
            "text": TEXT,
            "requests": [],
            "uploads": [],
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
            "PATH": os.environ["PATH"] if WASM else str(self.bin),
            "TMPDIR": str(self.home),
            "LANG": "C.UTF-8",
            "CHATGPT_ACCESS_TOKEN": TOKEN,
            "CHATGPT_ACCOUNT_ID": ACCOUNT,
            "TNY_CODEX_BASE_URL": self.url + "/backend-api/codex/",
            "DICTATION_RECORDER_LOG": str(self.log),
            "TNY_ISOLATE": "0",
        }

    def tearDown(self):
        self.state["release"].set()
        self.server.shutdown()
        self.server.server_close()
        self.tmp.cleanup()

    def run_dictate(self, *args, env=None, audio=True, prefix=()):
        return subprocess.run(
            [
                TNY,
                *prefix,
                "dictate",
                *(["--input-file", str(self.wav)] if audio else []),
                *args,
            ],
            env=env or self.env,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=20,
        )

    def check_failure(self, p, code=1):
        self.assertEqual(p.returncode, code, p.stderr)
        self.assertEqual(p.stdout, "")
        self.assertNotIn(TOKEN, p.stderr)

    def test_file_uses_subscription_while_grok_is_selected(self):
        p = self.run_dictate(
            "--json", prefix=("--provider", "grok", "--base-url", "http://127.0.0.1:1")
        )
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(
            json.loads(p.stdout), {"kind": "dictate", "provider": "codex", "text": TEXT}
        )
        self.assertFalse(self.log.exists())
        self.assertEqual(len(self.state["uploads"]), 1)
        parts = self.state["uploads"][0]
        self.assertEqual(len(parts), 1)
        self.assertEqual(
            parts[0].get_param("name", header="content-disposition"), "file"
        )
        self.assertEqual(parts[0].get_filename(), "audio.wav")
        self.assertEqual(parts[0].get_content_type(), "audio/wav")
        self.assertEqual(parts[0].get_payload(decode=True), self.wav.read_bytes())
        self.assertFalse(self.state["chat"])
        self.assertFalse((self.home / ".tny").exists())

    def test_chunked_utf8_transcript_and_plain_stdout(self):
        self.state["mode"] = "chunked"
        p = self.run_dictate()
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, TEXT + "\n")

    def test_rejections_are_bounded_and_secret_safe(self):
        for mode in (401, 403, 429, "html", "truncated", "oversize-wire", "malformed"):
            with self.subTest(mode=mode):
                self.state["mode"] = mode
                self.check_failure(
                    self.run_dictate(), 2 if isinstance(mode, int) else 1
                )

    def test_invalid_transcripts_never_become_prompts(self):
        for text in (
            None,
            3,
            "",
            " \t\r\n",
            "bad\u0000tail",
            "bad\x1b[2J",
            "bad\u009b",
            "x" * 65537,
        ):
            with self.subTest(text=str(text)[:25]):
                self.state["text"] = text
                self.check_failure(self.run_dictate())

    def test_missing_subscription_and_header_injection_fail_before_request(self):
        for token in (None, "", "bad\r\nheader"):
            env = self.env.copy()
            if token is None:
                del env["CHATGPT_ACCESS_TOKEN"]
            else:
                env["CHATGPT_ACCESS_TOKEN"] = token
            env["OPENAI_API_KEY"] = "chat-api-key-is-not-subscription"
            self.check_failure(self.run_dictate(env=env))
        self.assertFalse(self.state["requests"])

    def test_file_validation_happens_before_network(self):
        for data in (b"", b"not a WAV", wav_bytes()[:-1], b"RIFF" + b"\xff" * 40):
            self.wav.write_bytes(data)
            self.check_failure(self.run_dictate())
        with self.wav.open("wb") as f:
            f.truncate(25 * 1024 * 1024 + 1)
        self.check_failure(self.run_dictate())
        self.assertFalse(self.state["requests"])

    def test_unknown_provider_and_bad_options_fail_without_recording(self):
        for args in (
            ("--stt-provider", "other"),
            ("--seconds", "0"),
            ("--seconds", "301"),
            ("--seconds", "1junk"),
            ("--device", "default"),
            ("--nonsense",),
        ):
            with self.subTest(args=args):
                self.check_failure(self.run_dictate(*args))
        self.assertFalse(self.log.exists())
        self.assertFalse(self.state["requests"])

    def test_check_is_local_only_and_provider_environment_is_independent(self):
        p = self.run_dictate(
            "--check", "--json", env={**self.env, "TNY_STT_PROVIDER": "codex"}
        )
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertTrue(json.loads(p.stdout)["available"])
        self.assertFalse(self.log.exists())
        self.assertFalse(self.state["requests"])
        self.check_failure(
            self.run_dictate(env={**self.env, "TNY_STT_PROVIDER": "missing"})
        )
        p = self.run_dictate(
            "--stt-provider", "codex", env={**self.env, "TNY_STT_PROVIDER": "missing"}
        )
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_missing_recorder_is_actionable_and_file_mode_still_works(self):
        env = {**self.env, "PATH": str(self.home / "empty")}
        if WASM:
            env["PATH"] = os.environ["PATH"]
        p = self.run_dictate("--seconds", "1", audio=False, env=env)
        self.check_failure(p)
        self.assertIn("microphone", p.stderr)
        self.assertFalse(self.state["requests"])
        p = self.run_dictate(env=env)
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_expired_account_refresh_uses_existing_codex_store(self):
        env = self.env.copy()
        del env["CHATGPT_ACCESS_TOKEN"]
        del env["CHATGPT_ACCOUNT_ID"]
        env["CODEX_REFRESH_TOKEN_URL_OVERRIDE"] = self.url + "/oauth/token"
        auth = self.home / "codex/auth.json"
        auth.write_text(
            json.dumps(
                {
                    "auth_mode": "chatgpt",
                    "tokens": {
                        "access_token": "expired",
                        "account_id": ACCOUNT,
                        "refresh_token": "old-refresh",
                    },
                    "last_refresh": "2020-01-01T00:00:00Z",
                }
            )
        )
        p = self.run_dictate(env=env)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(self.state["refresh"]["refresh_token"], "old-refresh")
        self.assertEqual(json.loads(auth.read_text())["tokens"]["access_token"], TOKEN)

    @unittest.skipUnless(MICROPHONE, "native microphone lifecycle")
    def test_timed_microphone_capture_and_literal_device(self):
        p = self.run_dictate(
            "--seconds", "1", "--device", "device ; literal", audio=False
        )
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, TEXT + "\n")
        log = json.loads(self.log.read_text())
        self.assertTrue(any("device ; literal" in a for a in log["argv"]))
        self.assert_recorder_stopped(log["pid"])
        with wave.open(
            io.BytesIO(self.state["uploads"][0][0].get_payload(decode=True))
        ) as f:
            self.assertEqual(
                (f.getframerate(), f.getnchannels(), f.getsampwidth()), (24000, 1, 2)
            )
            self.assertEqual(f.getnframes(), 24000)

    @unittest.skipUnless(MICROPHONE, "native recorder escalation")
    def test_uncooperative_recorder_is_killed_after_stop(self):
        p = self.run_dictate(
            "--seconds",
            "1",
            audio=False,
            env={**self.env, "DICTATION_RECORDER_MODE": "hang"},
        )
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assert_recorder_stopped(json.loads(self.log.read_text())["pid"])

    @unittest.skipUnless(MICROPHONE, "native recording cancellation")
    def test_cli_cancel_while_recording_discards_audio(self):
        p = subprocess.Popen(
            [TNY, "dictate", "--seconds", "300"],
            env=self.env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            deadline = time.monotonic() + 5
            while not self.log.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(self.log.exists())
            pid = json.loads(self.log.read_text())["pid"]
            p.send_signal(signal.SIGINT)
            out, err = p.communicate(timeout=4)
            self.assertEqual(p.returncode, 130, err)
            self.assertEqual(out, "")
            self.assertFalse(self.state["requests"])
            self.assert_recorder_stopped(pid)
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()

    @unittest.skipUnless(MICROPHONE, "native microphone lifecycle")
    def test_recorder_failure_preserves_no_audio_and_makes_no_request(self):
        p = self.run_dictate(
            "--seconds",
            "1",
            audio=False,
            env={**self.env, "DICTATION_RECORDER_MODE": "fail"},
        )
        self.check_failure(p)
        self.assertFalse(self.state["requests"])
        self.assert_recorder_stopped(json.loads(self.log.read_text())["pid"])

    def assert_recorder_stopped(self, pid):
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    @unittest.skipUnless(MICROPHONE, "native signals")
    def test_cancel_during_transcription_has_no_partial_stdout(self):
        self.state["mode"] = "stall"
        p = subprocess.Popen(
            [TNY, "dictate", "--input-file", str(self.wav)],
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            self.assertTrue(self.state["ready"].wait(10))
            p.send_signal(signal.SIGINT)
            out, err = p.communicate(timeout=4)
            self.assertEqual(p.returncode, 130, err)
            self.assertEqual(out, "")
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()

    def start_tui(self):
        from test_tui import Term

        settings = self.home / ".tny/settings.json"
        settings.parent.mkdir(exist_ok=True)
        settings.write_text(
            json.dumps(
                {
                    "grok": {
                        "base_url": self.url + "/v1",
                        "api_key": "fixture-chat-key",
                        "model": "grok-fixture",
                        "wire_api": "chat",
                    }
                }
            )
        )
        term = Term(
            [TNY, "--provider", "grok", "--ephemeral", "--no-extensions"],
            self.env,
            str(self.home),
        )
        try:
            term.expect_on_screen("grok-fixture")
            return term
        except BaseException:
            term.close()
            term.proc.wait(timeout=5)
            raise

    def wait_recorded(self, term):
        deadline = time.monotonic() + 5
        while not self.log.exists() and time.monotonic() < deadline:
            term.pump(0.05)
        self.assertTrue(self.log.exists())
        term.pump(0.15)

    @unittest.skipUnless(MICROPHONE, "native TUI")
    def test_tui_preserves_draft_and_sends_only_on_second_enter_to_grok(self):
        term = self.start_tui()
        try:
            term.send("Please \x12")
            term.expect_on_screen("Listening")
            self.wait_recorded(term)
            term.send("\r")
            term.expect_on_screen("Dictation ready")
            term.expect_on_screen("Please " + TEXT)
            self.assertFalse(self.state["chat"])
            self.assert_recorder_stopped(json.loads(self.log.read_text())["pid"])
            term.send("\r")
            term.expect("CHAT-OK")
            self.assertEqual(len(self.state["chat"]), 1)
            chat = self.state["chat"][0]
            self.assertEqual(chat["model"], "grok-fixture")
            self.assertTrue(
                any(
                    m.get("role") == "user" and m.get("content") == "Please " + TEXT
                    for m in chat["messages"]
                )
            )
            for path, headers, _ in self.state["requests"]:
                self.assertEqual(
                    headers["authorization"],
                    f"Bearer {TOKEN}"
                    if path.endswith("/transcribe")
                    else "Bearer fixture-chat-key",
                )
                if path.endswith("/chat/completions"):
                    self.assertNotIn("chatgpt-account-id", headers)
            term.send("/quit\r")
            self.assertEqual(term.wait(), 0)
        finally:
            term.close()
            term.proc.wait(timeout=5)

    @unittest.skipUnless(MICROPHONE, "native TUI")
    def test_dictation_then_optimisation_then_explicit_chat_submission(self):
        self.env.update(
            OPENROUTER_BASE_URL=f"http://127.0.0.1:{self.server.server_port}/v1",
            OPENROUTER_API_KEY="fixture-optimise-key",
        )
        term = self.start_tui()
        try:
            term.send("\x12")
            term.expect_on_screen("Listening")
            self.wait_recorded(term)
            term.send("\r")
            term.expect_on_screen("Dictation ready")
            term.expect_on_screen(TEXT)
            term.send("\x0f")
            term.expect_on_screen("Prompt optimised")
            term.expect_on_screen(OPTIMISED)
            self.assertEqual(len(self.state["chat"]), 1)
            rewritten = self.state["chat"][0]
            self.assertEqual(rewritten["model"], "inception/mercury-2.5")
            self.assertEqual(rewritten["messages"][-1]["content"], TEXT)
            term.send("\r")
            term.expect("CHAT-OK")
            self.assertEqual(len(self.state["chat"]), 2)
            chat = self.state["chat"][-1]
            self.assertEqual(chat["model"], "grok-fixture")
            self.assertEqual(chat["messages"][-1]["content"], OPTIMISED)
            term.send("/quit\r")
            self.assertEqual(term.wait(), 0)
        finally:
            term.close()
            term.proc.wait(timeout=5)

    @unittest.skipUnless(MICROPHONE, "native TUI")
    def test_tui_escape_stops_microphone_and_preserves_draft(self):
        term = self.start_tui()
        try:
            term.send("Keep this draft\x12")
            term.expect_on_screen("Listening")
            self.wait_recorded(term)
            pid = json.loads(self.log.read_text())["pid"]
            term.send("\x1b")
            term.expect_on_screen("Dictation cancelled")
            term.expect_on_screen("Keep this draft")
            self.assert_recorder_stopped(pid)
            self.assertFalse(self.state["requests"])
            term.send("\x15/quit\r")
            self.assertEqual(term.wait(), 0)
        finally:
            term.close()
            term.proc.wait(timeout=5)

    @unittest.skipUnless(MICROPHONE, "native TUI")
    def test_slash_dictation_cancels_stalled_upload_without_sending(self):
        self.state["mode"] = "stall"
        term = self.start_tui()
        try:
            term.send("/dictate\r")
            term.expect_on_screen("Listening")
            self.wait_recorded(term)
            term.send("\x12")
            term.expect_on_screen("Transcribing")
            self.assertTrue(self.state["ready"].wait(5))
            term.send("\x03")
            term.expect_on_screen("Dictation cancelled")
            self.assertFalse(self.state["chat"])
            term.send("/quit\r")
            self.assertEqual(term.wait(), 0)
        finally:
            term.close()
            term.proc.wait(timeout=5)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
