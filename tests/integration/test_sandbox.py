#!/usr/bin/env python3
"""End-to-end terminal `os` sandbox: workspace write succeeds, outside fails."""

import json
import os
import shlex
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TNY = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", "build/tny")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    targets = {}
    results = {}

    def log_message(self, *_args):
        pass

    def _chunk(self, data):
        self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")

    def _stream(self, frames):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for frame in frames:
            self._chunk(f"data: {json.dumps(frame)}\n\n".encode())
        self._chunk(b"data: [DONE]\n\n")
        self._chunk(b"")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length))
        messages = body.get("messages", [])
        prompt = next(
            (
                m.get("content", "")
                for m in messages
                if m.get("role") == "user" and isinstance(m.get("content"), str)
            ),
            "",
        )
        scenario = "outside" if "outside" in prompt else "inside"
        tool_result = next(
            (m.get("content", "") for m in messages if m.get("role") == "tool"),
            None,
        )
        if tool_result is None:
            command = f"printf sandbox-ok > {shlex.quote(self.targets[scenario])}"
            frames = [
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "role": "assistant",
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "id": f"sandbox-{scenario}",
                                        "type": "function",
                                        "function": {
                                            "name": "terminal",
                                            "arguments": json.dumps(
                                                {"command": command}
                                            ),
                                        },
                                    }
                                ],
                            },
                        }
                    ]
                },
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]},
            ]
        else:
            self.results[scenario] = tool_result
            frames = [
                {"choices": [{"index": 0, "delta": {"content": "SANDBOX-OK"}}]},
                {
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                    "usage": {"prompt_tokens": 10, "completion_tokens": 2},
                },
            ]
        self._stream(frames)


def main():
    with tempfile.TemporaryDirectory(prefix="tny-sandbox-") as home:
        workspace = os.path.join(home, "workspace")
        temp_dir = os.path.join(workspace, "tmp")
        os.makedirs(temp_dir)
        inside = os.path.join(workspace, "inside.txt")
        outside = os.path.join(home, "outside.txt")
        Handler.targets = {"inside": inside, "outside": outside}
        Handler.results = {}

        settings_dir = os.path.join(home, ".tny")
        os.makedirs(settings_dir)
        with open(os.path.join(settings_dir, "settings.json"), "w") as f:
            json.dump(
                {
                    "permission_mode": "ask",
                    "permission": {"bash": {"*": "allow"}},
                },
                f,
            )
        with open(os.path.join(workspace, ".tny.json"), "w") as f:
            json.dump({"sandbox": "os"}, f)

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = dict(
            os.environ,
            HOME=home,
            TMPDIR=temp_dir,
            OPENAI_API_KEY="sandbox-fixture-not-real",
        )
        base = f"http://127.0.0.1:{server.server_port}/v1"
        try:
            doctor = subprocess.run(
                [TNY, "--cwd", workspace, "doctor", "--json"],
                env=env,
                capture_output=True,
                timeout=15,
                check=True,
            )
            effective = json.loads(doctor.stdout)["sandbox"]
            if effective != "os":
                print("skip: supported OS sandbox wrapper is unavailable")
                return

            def run(scenario):
                result = subprocess.run(
                    [
                        TNY,
                        "--cwd",
                        workspace,
                        "--provider",
                        "openai",
                        "--base-url",
                        base,
                        "--wire-api",
                        "chat",
                        "ask",
                        "--json",
                        "--no-save",
                        f"write {scenario}",
                    ],
                    env=env,
                    capture_output=True,
                    timeout=30,
                )
                assert result.returncode == 0, result.stderr.decode()
                assert json.loads(result.stdout)["output"] == "SANDBOX-OK"

            run("inside")
            assert open(inside).read() == "sandbox-ok"
            assert "exit code: 0" in Handler.results["inside"]

            run("outside")
            assert not os.path.exists(outside)
            denied = Handler.results["outside"]
            assert denied.startswith("error: os sandbox denied a write"), denied
            assert outside in denied, denied
            assert "workspace extra dirs" in denied, denied
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    main()
