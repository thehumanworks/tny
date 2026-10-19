#!/usr/bin/env python3
"""Chat-only profiles: reproduce Responses 404, then exercise the chat tool loop.

All credentials are synthetic; no network access beyond the loopback fixture.
The exact production Inception endpoint default is covered by core unit tests.
"""

import json
import os
import subprocess
import tempfile
import threading
from http.server import ThreadingHTTPServer
from pathlib import Path

from mock_openai import Handler, need

ROOT = Path(__file__).resolve().parents[2]
TNY = os.environ.get("TNY", str(ROOT / "build/tny"))
KEY = "synthetic-inception-secret"
REQUESTS = []


class ChatOnly(Handler):
    def do_POST(self):
        REQUESTS.append(self.path)
        if self.path != "/v1/chat/completions":
            self.close_connection = True
            self._json(404, {"error": {"message": "endpoint not found"}})
            return
        super().do_POST()

    def _post_chat(self, req):
        need(req.get("model") == "mercury-3", "provider-local model required")
        need(self.headers.get("Authorization") == f"Bearer {KEY}", "wrong credential")
        need("input" not in req and isinstance(req.get("messages"), list), "wrong body")
        super()._post_chat(req)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        home, ws = Path(tmp) / "home", Path(tmp) / "ws"
        home.mkdir()
        ws.mkdir()
        (ws / "fixture.txt").write_text("fixture\n")
        server = ThreadingHTTPServer(("127.0.0.1", 0), ChatOnly)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = {
            k: v
            for k, v in os.environ.items()
            if not k.startswith(("OPENAI_", "INCEPTION_", "TNY_", "MOCK_"))
        }
        env.update(
            HOME=str(home), INCEPTION_API_KEY=KEY, OPENAI_API_KEY="foreign-secret"
        )
        base = f"http://127.0.0.1:{server.server_port}/v1"

        def run(*args):
            result = subprocess.run(
                [TNY, "--cwd", str(ws), *args], env=env, capture_output=True, timeout=45
            )
            assert KEY.encode() not in result.stdout + result.stderr, result
            assert b"foreign-secret" not in result.stdout + result.stderr, result
            return result

        try:
            setup = run(
                "provider",
                "setup",
                "inception",
                "--base-url",
                base,
                "--api-key-env",
                "INCEPTION_API_KEY",
                "--model",
                "mercury-3",
            )
            assert setup.returncode == 0, setup.stderr
            failed = run(
                "--provider", "inception", "ask", "--json", "--no-save", "list files"
            )
            assert failed.returncode != 0 and b"404" in failed.stdout + failed.stderr, (
                failed
            )
            assert REQUESTS == ["/v1/responses"], REQUESTS  # no speculative retry
            good = run(
                "--provider",
                "inception",
                "--wire-api",
                "chat",
                "ask",
                "--json",
                "--no-save",
                "list files",
            )
            assert good.returncode == 0, good.stderr
            answer = json.loads(good.stdout)
            assert "MOCK-OK" in answer["output"] and answer["steps"] == 2, answer
            assert REQUESTS[1:] == ["/v1/chat/completions"] * 2, REQUESTS
            setup = run("provider", "setup", "inception", "--wire-api", "chat")
            assert setup.returncode == 0, setup.stderr
            good = run(
                "--provider", "inception", "ask", "--json", "--no-save", "list files"
            )
            assert good.returncode == 0 and b"MOCK-OK" in good.stdout, good.stderr
            # Scoped environment beats settings; CLI beats both, even invalid env.
            env["INCEPTION_WIRE_API"] = "responses"
            before = len(REQUESTS)
            failed = run(
                "--provider", "inception", "ask", "--json", "--no-save", "list files"
            )
            assert failed.returncode != 0 and REQUESTS[before:] == ["/v1/responses"]
            env["INCEPTION_WIRE_API"] = "secret-invalid-wire"
            before = len(REQUESTS)
            failed = run(
                "--provider", "inception", "ask", "--json", "--no-save", "list files"
            )
            assert failed.returncode != 0 and len(REQUESTS) == before
            assert b"wire_api must be chat|responses" in failed.stderr
            assert b"secret-invalid-wire" not in failed.stdout + failed.stderr
            good = run(
                "--provider",
                "inception",
                "--wire-api",
                "chat",
                "ask",
                "--json",
                "--no-save",
                "list files",
            )
            assert good.returncode == 0 and b"MOCK-OK" in good.stdout
            env["INCEPTION_WIRE_API"] = "chat"
            setup = run("provider", "setup", "inception", "--wire-api", "responses")
            assert setup.returncode == 0
            good = run(
                "--provider", "inception", "ask", "--json", "--no-save", "list files"
            )
            assert good.returncode == 0 and b"MOCK-OK" in good.stdout
            print(
                "ok chat-only provider: settings/env/CLI precedence, safe errors, streaming tool loop, no fallback"
            )
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    main()
