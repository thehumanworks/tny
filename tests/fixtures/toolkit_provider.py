"""Shared, offline SDK toolkit provider. All credentials and media are synthetic."""

from __future__ import annotations

import base64
import io
import json
import sys
import threading
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

TOKEN = "toolkit-fixture-token"
ACCOUNT = "toolkit-fixture-account"
OPTIMISE_TOKEN = "toolkit-fixture-optimise-token"
SEED = 4242
REQUEST_ID = "fixture-request-id"
TEXT = "Improve src/context.txt while preserving UTF-8. Café."
PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a0xoAAAAASUVORK5CYII="
)
MP3 = b"ID3" + b"\0" * 61
READ_TOOLS = {
    "read_file",
    "list_files",
    "glob_files",
    "grep_files",
    "file_info",
    "read_tool_result",
}


def wav_bytes() -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(2)
        writer.setframerate(16000)
        writer.writeframes(b"\0" * 32000)
    return output.getvalue()


def workspace(path: Path) -> None:
    (path / "src").mkdir(parents=True)
    (path / "src/context.txt").write_text("UTF-8 fixture context\n")
    (path / "settings.json").write_text("{}")
    (path / "input.wav").write_bytes(wav_bytes())
    (path / "reference.png").write_bytes(PNG)


def fake_audio(path: Path) -> None:
    path.mkdir()
    for name in ("ffmpeg", "arecord"):
        script = path / name
        script.write_text(
            f"#!{sys.executable}\nimport signal, sys\nsys.stdout.buffer.write(b'\\0' * 48000)\nsys.stdout.buffer.flush()\nsignal.pause()\n"
        )
        script.chmod(0o755)
    for name in ("afplay", "ffplay", "mpv", "mpg123"):
        script = path / name
        script.write_text(
            f"#!{sys.executable}\nimport sys\nassert sys.stdin.buffer.read().startswith(b'ID3')\n"
        )
        script.chmod(0o755)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, status, data, content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            # Exercise split JSON, SSE, UTF-8 and binary responses through the
            # actual native transport, not an SDK mock of a successful call.
            for at in range(0, len(data), 7):
                self.wfile.write(data[at : at + 7])
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        owner = self.server.owner
        if self.path == "/requests":
            self.reply(200, json.dumps(owner.requests).encode())
        elif self.path == "/assets":
            self.reply(
                200,
                json.dumps(
                    {
                        "png": base64.b64encode(PNG).decode(),
                        "mp3": base64.b64encode(MP3).decode(),
                        "wav": base64.b64encode(wav_bytes()).decode(),
                        "text": TEXT,
                        "token": TOKEN,
                        "account": ACCOUNT,
                        "optimiseToken": OPTIMISE_TOKEN,
                        "seed": SEED,
                        "requestId": REQUEST_ID,
                        "python": sys.executable,
                    }
                ).encode(),
            )
        else:
            self.reply(404, b"{}")

    def do_POST(self):
        owner = self.server.owner
        raw = self.rfile.read(int(self.headers["Content-Length"]))
        if self.path == "/mode":
            owner.mode = json.loads(raw)["mode"]
            owner.arrived.clear()
            self.reply(200, b"{}")
            return
        audio = self.path == "/backend-api/transcribe"
        body = (
            {"wav": "audio.wav" in raw.decode("latin1"), "bytes": len(raw)}
            if audio
            else json.loads(raw)
        )
        owner.requests.append(
            {"path": self.path, "headers": dict(self.headers), "body": body}
        )
        owner.arrived.set()
        chat = self.path == "/v1/chat/completions"
        expected = OPTIMISE_TOKEN if chat else TOKEN
        if self.headers.get("Authorization") != f"Bearer {expected}" or (
            not chat and self.headers.get("chatgpt-account-id") != ACCOUNT
        ):
            self.reply(401, b'{"error":"fixture credential mismatch"}')
            return
        if owner.mode == "error":
            self.reply(400, json.dumps({"error": TOKEN + " private prompt"}).encode())
            return
        if owner.mode == "stall":
            owner.release.wait(10)
        if owner.mode == "invalid":
            self.reply(200, b"{}")
            return
        if audio:
            self.reply(200, json.dumps({"text": TEXT}).encode())
        elif self.path.endswith("/images/generations") or self.path.endswith(
            "/images/edits"
        ):
            item = {"b64_json": base64.b64encode(PNG).decode()}
            envelope = {"data": [item]}
            # Only the "identified" mode returns provider metadata, so the
            # default mode proves absence is preserved rather than invented.
            if owner.mode == "identified":
                item["seed"] = SEED
                envelope["request_id"] = REQUEST_ID
            self.reply(200, json.dumps(envelope).encode())
        elif self.path == "/backend-api/pronunciation/synthesize?format=mp3":
            self.reply(200, MP3, "audio/mpeg")
        elif chat:
            results = [m for m in body["messages"] if m["role"] == "tool"]
            delta = {"content": TEXT}
            if owner.mode in ("explore", "write") and not results:
                name = "read_file" if owner.mode == "explore" else "write_file"
                args = {"path": "src/context.txt"}
                if name == "write_file":
                    args["content"] = "changed"
                delta = {
                    "content": "Exploring",
                    "tool_calls": [
                        {
                            "index": 0,
                            "id": "read_context",
                            "type": "function",
                            "function": {"name": name, "arguments": json.dumps(args)},
                        }
                    ],
                }
            reason = "tool_calls" if "tool_calls" in delta else "stop"
            frames = [
                {"choices": [{"delta": delta, "finish_reason": None}]},
                {"choices": [{"delta": {}, "finish_reason": reason}]},
            ]
            wire = (
                "".join("data: " + json.dumps(f) + "\n\n" for f in frames)
                + "data: [DONE]\n\n"
            )
            self.reply(200, wire.encode(), "text/event-stream")
        else:
            self.reply(404, b"{}")


class Provider:
    def __init__(self):
        self.requests = []
        self.mode = "ok"
        self.arrived = threading.Event()
        self.release = threading.Event()
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.owner = self
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}"

    def close(self):
        self.release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


if __name__ == "__main__":
    provider = Provider()
    print(provider.url, flush=True)
    try:
        sys.stdin.buffer.read()
    finally:
        provider.close()
