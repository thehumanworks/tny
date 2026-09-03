#!/usr/bin/env python3
"""End-to-end: the builtin codex profile against the mock OpenAI provider
(docs/adr/0065, docs/backends/codex.md).

A `codex login` leaves $CODEX_HOME/auth.json behind; tny reads it directly
and drives the Responses-compatible ChatGPT backend on its own native loop.
The mock stands in for chatgpt.com/backend-api/codex (TNY_CODEX_BASE_URL)
and, beyond the usual strict Responses-wire checks (store:false, stream,
instructions, `input` array, flat tools), demands the profile's headers:
`Authorization: Bearer <access_token>`, `chatgpt-account-id`, and
`OpenAI-Beta: responses=v1`.

Runs:
  1. explicit --provider codex, account id from the access-token claim
  2. auto-detection: no flag, no keys, only auth.json → provider "codex"
  3. refresh: an expired access token is exchanged at the refresh endpoint
     (CODEX_REFRESH_TOKEN_URL_OVERRIDE) before the turn, auth.json is
     rewritten, and the request carries the NEW bearer
  4. API-key auth.json (`codex login --with-api-key`) → api.openai.com shape
     (no ChatGPT headers), still provider "codex"
  5. no login at all: a clear startup error naming `tny --provider codex login`

Same suite for the wasm build (TNY=build/wasm/tny, docs/adr/0017): the
profile is plain HTTPS, so wasm parity is by construction.
"""

import base64
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TNY = os.environ.get("TNY", sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "tny"))
MOCK = os.path.join(ROOT, "tests", "integration", "mock_openai.py")

ACCOUNT = "acct_test_123"


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode().rstrip("=")


def jwt(payload: dict) -> str:
    return "%s.%s.sig" % (
        b64url(json.dumps({"alg": "none"}).encode()),
        b64url(json.dumps(payload).encode()),
    )


def access_token(exp: int, account: str = ACCOUNT, tag: str = "a") -> str:
    return jwt({"exp": exp, "tag": tag, "https://api.openai.com/auth": {"chatgpt_account_id": account}})


def write_auth(home: str, obj: dict) -> str:
    codex_home = os.path.join(home, ".codex")
    os.makedirs(codex_home, exist_ok=True)
    path = os.path.join(codex_home, "auth.json")
    with open(path, "w") as f:
        json.dump(obj, f)
    return path


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start_mock(extra_env):
    port = free_port()
    env = dict(os.environ, MOCK_EXPECT_WIRE="responses")
    env.update(extra_env)
    mock = subprocess.Popen(
        [sys.executable, MOCK, str(port)],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    line = mock.stdout.readline().decode()
    assert "ready" in line, f"mock did not start: {line!r}"
    return mock, port


def base_env(home, port):
    env = dict(os.environ)
    for key in list(env):
        if key.endswith("_API_KEY") or key.endswith("_BASE_URL") or key.startswith("CODEX_"):
            env.pop(key)
    env.update(
        HOME=home,
        TNY_CODEX_BASE_URL=f"http://127.0.0.1:{port}/v1",
        TNY_ISOLATE="0",
    )
    env.pop("CLAUDE_CODE_OAUTH_TOKEN", None)
    return env


def ask(env, ws, *flags):
    return subprocess.run(
        [TNY, *flags, "--cwd", ws, "ask", "--json", "--yolo", "--no-save", "list files"],
        env=env,
        capture_output=True,
        timeout=60,
    )


def expect_ok(r, label):
    assert r.returncode == 0, f"{label}: exit {r.returncode}\n{r.stderr.decode()}"
    out = json.loads(r.stdout.decode())
    assert out.get("provider") == "codex", f"{label}: provider is {out.get('provider')!r}"
    assert "list_files" in r.stdout.decode() or out.get("output"), f"{label}: no answer: {out}"
    return out


class RefreshHandler(BaseHTTPRequestHandler):
    calls = []

    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(n))
        RefreshHandler.calls.append((self.path, dict(self.headers), body))
        ok = (
            self.path == "/oauth/token"
            and body.get("grant_type") == "refresh_token"
            and body.get("client_id") == "app_EMoamEEZ73f0CkXaXp7hrann"
            and body.get("refresh_token") == "refresh-old"
        )
        reply = (
            {
                "access_token": access_token(int(time.time()) + 3600, tag="new"),
                "refresh_token": "refresh-new",
                "id_token": jwt({"email": "user@example.test"}),
            }
            if ok
            else {"error": "invalid_grant"}
        )
        data = json.dumps(reply).encode()
        self.send_response(200 if ok else 401)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    with tempfile.TemporaryDirectory(prefix="tny-codex-") as tmp:
        home = os.path.join(tmp, "home")
        ws = os.path.join(tmp, "ws")
        os.makedirs(home)
        os.makedirs(ws)
        with open(os.path.join(ws, "a.txt"), "w") as f:
            f.write("hello\n")
        fresh = access_token(int(time.time()) + 24 * 3600)

        # ---- 1. explicit provider, claim-derived account id ----------------
        write_auth(
            home,
            {
                "auth_mode": "chatgpt",
                "OPENAI_API_KEY": None,
                "tokens": {"access_token": fresh, "refresh_token": "refresh-old", "id_token": jwt({})},
                "last_refresh": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            },
        )
        mock, port = start_mock(
            {
                "MOCK_EXPECT_HEADERS": ";".join(
                    [
                        f"Authorization=Bearer {fresh}",
                        f"chatgpt-account-id={ACCOUNT}",
                        "OpenAI-Beta=responses=v1",
                    ]
                )
            }
        )
        try:
            r = ask(base_env(home, port), ws, "--provider", "codex")
            expect_ok(r, "explicit provider")
            print("ok  --provider codex: bearer + chatgpt-account-id + OpenAI-Beta on the responses wire")

            # ---- 2. auto-detection from auth.json alone ---------------------
            r = ask(base_env(home, port), ws)
            expect_ok(r, "auto-detect")
            print("ok  auto-detected codex from $CODEX_HOME/auth.json")
        finally:
            mock.terminate()
            mock.wait(timeout=5)

        # ---- 3. expired token → refresh grant before the turn ---------------
        server = HTTPServer(("127.0.0.1", 0), RefreshHandler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        refresh_url = "http://127.0.0.1:%d/oauth/token" % server.server_address[1]
        expired = access_token(int(time.time()) - 60, tag="old")
        auth_path = write_auth(
            home,
            {
                "auth_mode": "chatgpt",
                "tokens": {"access_token": expired, "refresh_token": "refresh-old", "account_id": ACCOUNT},
                "last_refresh": "2020-01-01T00:00:00Z",
            },
        )
        mock, port = start_mock(
            {
                # the request must carry the refreshed bearer, never the expired one
                "MOCK_EXPECT_HEADERS": f"chatgpt-account-id={ACCOUNT};OpenAI-Beta=responses=v1",
            }
        )
        try:
            env = base_env(home, port)
            env["CODEX_REFRESH_TOKEN_URL_OVERRIDE"] = refresh_url
            r = ask(env, ws, "--provider", "codex")
            expect_ok(r, "refresh")
            assert len(RefreshHandler.calls) == 1, RefreshHandler.calls
            with open(auth_path) as f:
                stored = json.load(f)
            new_access = stored["tokens"]["access_token"]
            assert new_access != expired, "auth.json still holds the expired token"
            assert json.loads(base64.urlsafe_b64decode(new_access.split(".")[1] + "==")).get("tag") == "new"
            assert stored["tokens"]["refresh_token"] == "refresh-new", stored
            assert stored["tokens"]["account_id"] == ACCOUNT, stored
            assert stored["last_refresh"] > "2020-01-02", stored
            if os.name != "nt":
                assert (os.stat(auth_path).st_mode & 0o077) == 0, oct(os.stat(auth_path).st_mode)
            print("ok  expired access token refreshed at auth.openai.com shape, auth.json rewritten 0600")

            # the second run is fresh: no second refresh call
            r = ask(env, ws, "--provider", "codex")
            expect_ok(r, "post-refresh")
            assert len(RefreshHandler.calls) == 1, "refreshed a fresh token again"
            print("ok  fresh token is not refreshed again")
        finally:
            mock.terminate()
            mock.wait(timeout=5)
            server.shutdown()

        # ---- 4. API-key auth.json → plain OpenAI API shape ------------------
        write_auth(home, {"auth_mode": "apikey", "OPENAI_API_KEY": "sk-from-codex-auth"})
        mock, port = start_mock(
            {
                "MOCK_EXPECT_HEADERS": "Authorization=Bearer sk-from-codex-auth",
                "MOCK_REJECT_HEADERS": "chatgpt-account-id;OpenAI-Beta",
            }
        )
        try:
            env = base_env(home, port)
            # the API-key mode targets api.openai.com; point it at the mock
            # through the same override every openai run uses
            r = subprocess.run(
                [
                    TNY,
                    "--provider",
                    "codex",
                    "--base-url",
                    f"http://127.0.0.1:{port}/v1",
                    "--cwd",
                    ws,
                    "ask",
                    "--json",
                    "--yolo",
                    "--no-save",
                    "list files",
                ],
                env=env,
                capture_output=True,
                timeout=60,
            )
            expect_ok(r, "api-key mode")
            print("ok  API-key auth.json rides plain bearer auth without ChatGPT headers")
        finally:
            mock.terminate()
            mock.wait(timeout=5)

        # ---- 5. no login: startup-class error, exit 1 -----------------------
        os.unlink(os.path.join(home, ".codex", "auth.json"))
        env = base_env(home, 1)
        r = ask(env, ws, "--provider", "codex")
        assert r.returncode == 1, r.stderr.decode()
        assert b"tny --provider codex login" in r.stderr, r.stderr
        print("ok  missing login names `tny --provider codex login`")

        # ---- logout deletes the file natively --------------------------------
        write_auth(home, {"tokens": {"access_token": fresh}})
        r = subprocess.run(
            [TNY, "--provider", "codex", "--cwd", ws, "logout"], env=env, capture_output=True, timeout=30
        )
        assert r.returncode == 0, r.stderr.decode()
        assert not os.path.exists(os.path.join(home, ".codex", "auth.json"))
        print("ok  logout removes $CODEX_HOME/auth.json")

    print("all codex (ChatGPT backend) integration tests passed")


if __name__ == "__main__":
    main()
