#!/usr/bin/env python3
"""End-to-end: the builtin codex profile against the mock OpenAI provider
(docs/adr/0065, docs/adr/0066, docs/backends/codex.md).

The mock provider stands in for chatgpt.com/backend-api/codex
(TNY_CODEX_BASE_URL) and, beyond the strict Responses-wire checks
(store:false, stream, instructions, `input` array, flat tools), demands
the profile's headers: `Authorization: Bearer <access_token>`,
`chatgpt-account-id`, `OpenAI-Beta: responses=v1`. A second in-test server
stands in for auth.openai.com (TNY_CODEX_OAUTH_ISSUER): /oauth/token for
the refresh and authorization-code grants (PKCE verified), and the
device-code endpoints.

Credential sources, in precedence order, and what each run proves:
  flag   --chatgpt-token / --chatgpt-account-id, no HOME files at all
  env    CHATGPT_ACCESS_TOKEN (+ CHATGPT_ACCOUNT_ID, else the JWT claim)
  store  ~/.tny/codex-auth.json — written by the native browser (PKCE +
         localhost callback, or pasted redirect URL) and device-code
         logins, preferred over the Codex CLI's file, refreshed in place
  cli    $CODEX_HOME/auth.json from `codex login` — read, refreshed in
         place, API-key mode honored
plus the no-credential startup error and native logout.

Same suite for the wasm build (TNY=build/wasm/tny, docs/adr/0017) except
the browser login's listening socket, which wasm has no way to open.
"""

import base64
import hashlib
import json
import os
import pty
import select
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TNY = os.environ.get(
    "TNY", sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "tny")
)
MOCK = os.path.join(ROOT, "tests", "integration", "mock_openai.py")
IS_WASM = "/wasm/" in TNY.replace("\\", "/")

ACCOUNT = "acct_test_123"
CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann"


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode().rstrip("=")


def jwt(payload: dict) -> str:
    return "%s.%s.sig" % (
        b64url(json.dumps({"alg": "none"}).encode()),
        b64url(json.dumps(payload).encode()),
    )


def jwt_payload(token: str) -> dict:
    return json.loads(base64.urlsafe_b64decode(token.split(".")[1] + "=="))


def access_token(exp: int, account: str = ACCOUNT, tag: str = "a") -> str:
    return jwt(
        {
            "exp": exp,
            "tag": tag,
            "https://api.openai.com/auth": {"chatgpt_account_id": account},
        }
    )


def write_json(path: str, obj: dict) -> str:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(obj, f)
    return path


def cli_auth_path(home):
    return os.path.join(home, ".codex", "auth.json")


def store_path(home):
    return os.path.join(home, ".tny", "codex-auth.json")


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


def stop(mock):
    mock.terminate()
    mock.wait(timeout=5)


def base_env(home, port=1, issuer=None):
    env = dict(os.environ)
    for key in list(env):
        if (
            key.endswith("_API_KEY")
            or key.endswith("_BASE_URL")
            or key.startswith("CODEX_")
            or key.startswith("CHATGPT_")
        ):
            env.pop(key)
    env.pop("CLAUDE_CODE_OAUTH_TOKEN", None)
    env.update(
        HOME=home, TNY_CODEX_BASE_URL=f"http://127.0.0.1:{port}/v1", TNY_ISOLATE="0"
    )
    if issuer:
        env["TNY_CODEX_OAUTH_ISSUER"] = issuer
    return env


def ask(env, ws, *flags, timeout=60):
    return subprocess.run(
        [
            TNY,
            *flags,
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
        timeout=timeout,
    )


def expect_ok(r, label):
    assert r.returncode == 0, f"{label}: exit {r.returncode}\n{r.stderr.decode()}"
    out = json.loads(r.stdout.decode())
    assert out.get("provider") == "codex", (
        f"{label}: provider is {out.get('provider')!r}"
    )
    assert out.get("output"), f"{label}: no answer: {out}"
    return out


def providers_json(env, ws):
    r = subprocess.run(
        [TNY, "--cwd", ws, "providers", "--json"],
        env=env,
        capture_output=True,
        timeout=30,
    )
    assert r.returncode == 0, r.stderr.decode()
    rows = {p["name"]: p for p in json.loads(r.stdout.decode())["providers"]}
    return rows["codex"]


# ---------------------------------------------------------------- mock issuer
class Issuer(BaseHTTPRequestHandler):
    """auth.openai.com stand-in: refresh + authorization-code grants with PKCE
    verification, and the Codex device-code endpoints."""

    state = {
        "refresh_calls": [],
        "code_calls": [],
        "issued_code": "auth-code-1",
        "device_polls": 0,
        "device_verifier": "device-verifier-xyz",
        "device_auth_id": "dev-auth-1",
        "user_code": "ABCD-EFGH",
        "next_tag": "browser",
    }

    def log_message(self, *a):
        pass

    def _send(self, status, obj):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _tokens(self, tag):
        return {
            "access_token": access_token(int(time.time()) + 3600, tag=tag),
            "refresh_token": f"refresh-{tag}",
            "id_token": jwt(
                {
                    "email": "user@example.test",
                    "https://api.openai.com/auth": {"chatgpt_account_id": ACCOUNT},
                }
            ),
            "expires_in": 3600,
            "token_type": "Bearer",
        }

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n)
        ctype = self.headers.get("Content-Type", "")
        if "json" in ctype:
            body = json.loads(raw)
        else:
            body = {k: v[0] for k, v in urllib.parse.parse_qs(raw.decode()).items()}
        st = Issuer.state
        if self.path == "/oauth/token":
            if body.get("grant_type") == "refresh_token":
                st["refresh_calls"].append((ctype, body))
                if body.get("client_id") != CLIENT_ID or not str(
                    body.get("refresh_token", "")
                ).startswith("refresh-"):
                    return self._send(401, {"error": "invalid_grant"})
                return self._send(200, self._tokens("refreshed"))
            if body.get("grant_type") == "authorization_code":
                st["code_calls"].append((ctype, body))
                ok = (
                    body.get("client_id") == CLIENT_ID
                    and body.get("code") == st["issued_code"]
                )
                verifier = body.get("code_verifier", "")
                if body.get("redirect_uri", "").endswith("/deviceauth/callback"):
                    ok = ok and verifier == st["device_verifier"]
                else:
                    ok = ok and "/auth/callback" in body.get("redirect_uri", "")
                    ok = ok and b64url(
                        hashlib.sha256(verifier.encode()).digest()
                    ) == st.get("challenge")
                if not ok:
                    return self._send(
                        400,
                        {
                            "error": "invalid_grant",
                            "error_description": f"bad exchange: {body}",
                        },
                    )
                return self._send(200, self._tokens(st["next_tag"]))
            return self._send(400, {"error": "unsupported_grant_type"})
        if self.path == "/api/accounts/deviceauth/usercode":
            if body.get("client_id") != CLIENT_ID:
                return self._send(400, {"error": "bad_client"})
            return self._send(
                200,
                {
                    "device_auth_id": st["device_auth_id"],
                    "user_code": st["user_code"],
                    "interval": "1",
                },
            )
        if self.path == "/api/accounts/deviceauth/token":
            if (
                body.get("device_auth_id") != st["device_auth_id"]
                or body.get("user_code") != st["user_code"]
            ):
                return self._send(400, {"error": {"code": "invalid_request"}})
            st["device_polls"] += 1
            if st["device_polls"] < 3:
                return self._send(
                    403, {"error": {"code": "deviceauth_authorization_pending"}}
                )
            return self._send(
                200,
                {
                    "authorization_code": st["issued_code"],
                    "code_verifier": st["device_verifier"],
                    "code_challenge": "ignored",
                },
            )
        return self._send(404, {"error": "no such route"})


def start_issuer():
    server = HTTPServer(("127.0.0.1", 0), Issuer)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, "http://127.0.0.1:%d" % server.server_address[1]


def read_until(fd_or_proc, needle: bytes, timeout: float, from_pty=None) -> bytes:
    """Collect stdout bytes until needle appears (pty master or a pipe)."""
    buf = b""
    deadline = time.time() + timeout
    fd = from_pty if from_pty is not None else fd_or_proc.stdout.fileno()
    while needle not in buf:
        if time.time() > deadline:
            raise AssertionError(f"timeout waiting for {needle!r}; got {buf!r}")
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                chunk = b""
            if not chunk:
                if from_pty is None and fd_or_proc.poll() is not None:
                    raise AssertionError(
                        f"process exited early ({fd_or_proc.returncode}); got {buf!r}"
                    )
                time.sleep(0.05)
                continue
            buf += chunk
    return buf


def authorize_url_from(output: bytes) -> dict:
    line = next(
        ln for ln in output.decode().splitlines() if "/oauth/authorize?" in ln
    ).strip()
    parsed = urllib.parse.urlparse(line)
    return {
        "url": line,
        **{k: v[0] for k, v in urllib.parse.parse_qs(parsed.query).items()},
    }


def main():
    with tempfile.TemporaryDirectory(prefix="tny-codex-") as tmp:
        home = os.path.join(tmp, "home")
        ws = os.path.join(tmp, "ws")
        os.makedirs(home)
        os.makedirs(ws)
        with open(os.path.join(ws, "a.txt"), "w") as f:
            f.write("hello\n")
        fresh = access_token(int(time.time()) + 24 * 3600)
        issuer, issuer_url = start_issuer()

        # ================================================ file-less sources
        # ---- flag: token + explicit id, an empty HOME -----------------------
        mock, port = start_mock(
            {
                "MOCK_EXPECT_HEADERS": "Authorization=Bearer opaque-flag-token;chatgpt-account-id=acct_flag;OpenAI-Beta=responses=v1"
            }
        )
        try:
            env = base_env(home, port)
            r = ask(
                env,
                ws,
                "--provider",
                "codex",
                "--chatgpt-token",
                "opaque-flag-token",
                "--chatgpt-account-id",
                "acct_flag",
            )
            expect_ok(r, "flag credential")
            # auto-detection needs no --provider either
            r = ask(
                env,
                ws,
                "--chatgpt-token",
                "opaque-flag-token",
                "--chatgpt-account-id",
                "acct_flag",
            )
            expect_ok(r, "flag credential, auto-detected")
            r = ask(env, ws, "--provider", "codex", "--chatgpt-token", "")
            assert r.returncode == 1 and b"must not be empty" in r.stderr, r.stderr
            print(
                "ok  --chatgpt-token/--chatgpt-account-id drive the profile with no files"
            )
        finally:
            stop(mock)

        # ---- the flag beats env --------------------------------------------
        mock, port = start_mock(
            {
                "MOCK_EXPECT_HEADERS": "Authorization=Bearer opaque-flag-token;chatgpt-account-id=acct_flag"
            }
        )
        try:
            env = base_env(home, port)
            env["CHATGPT_ACCESS_TOKEN"] = "opaque-env"
            env["CHATGPT_ACCOUNT_ID"] = "acct_env_explicit"
            expect_ok(
                ask(
                    env,
                    ws,
                    "--chatgpt-token",
                    "opaque-flag-token",
                    "--chatgpt-account-id",
                    "acct_flag",
                ),
                "flag beats env",
            )
            print("ok  --chatgpt-token beats CHATGPT_ACCESS_TOKEN")
        finally:
            stop(mock)

        # ---- env: token with the account id in the JWT claim ---------------
        mock, port = start_mock(
            {
                "MOCK_EXPECT_HEADERS": f"Authorization=Bearer {fresh};chatgpt-account-id={ACCOUNT};OpenAI-Beta=responses=v1"
            }
        )
        try:
            env = base_env(home, port)
            env["CHATGPT_ACCESS_TOKEN"] = fresh
            expect_ok(ask(env, ws), "env credential, auto-detected")
            row = providers_json(env, ws)
            assert row["healthy"] and "CHATGPT_ACCESS_TOKEN" in row["hint"], row
            print(
                "ok  CHATGPT_ACCESS_TOKEN auto-detects codex; account id from the JWT claim"
            )
        finally:
            stop(mock)
        mock, port = start_mock(
            {
                "MOCK_EXPECT_HEADERS": "Authorization=Bearer opaque-env;chatgpt-account-id=acct_env_explicit"
            }
        )
        try:
            env = base_env(home, port)
            env["CHATGPT_ACCESS_TOKEN"] = "opaque-env"
            env["CHATGPT_ACCOUNT_ID"] = "acct_env_explicit"
            expect_ok(
                ask(env, ws, "--provider", "codex"), "env credential, explicit id"
            )
            print("ok  CHATGPT_ACCOUNT_ID beats the claim")
        finally:
            stop(mock)

        # ================================================ Codex CLI file
        write_json(
            cli_auth_path(home),
            {
                "auth_mode": "chatgpt",
                "OPENAI_API_KEY": None,
                "tokens": {
                    "access_token": fresh,
                    "refresh_token": "refresh-cli",
                    "id_token": jwt({}),
                },
                "last_refresh": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            },
        )
        mock, port = start_mock(
            {
                "MOCK_EXPECT_HEADERS": f"Authorization=Bearer {fresh};chatgpt-account-id={ACCOUNT};OpenAI-Beta=responses=v1"
            }
        )
        try:
            env = base_env(home, port)
            expect_ok(ask(env, ws, "--provider", "codex"), "codex cli file")
            expect_ok(ask(env, ws), "codex cli file, auto-detected")
            assert "$CODEX_HOME/auth.json" in providers_json(env, ws)["hint"]
            # /models: the ChatGPT catalog wants ?client_version= and the
            # profile's headers, answers {"models":[…]} keyed by slug
            r = subprocess.run(
                [TNY, "--provider", "codex", "models", "--json"],
                env=env,
                capture_output=True,
                timeout=30,
            )
            assert r.returncode == 0, r.stderr
            assert b"showing configured" not in r.stderr, r.stderr
            cat = json.loads(r.stdout)
            assert cat["kind"] == "models" and cat["provider"] == "codex", cat
            assert [m["id"] for m in cat["models"]] == ["gpt-5.6-sol", "gpt-5.5"], cat
            assert cat["models"][0]["efforts"] == ["low", "medium", "high"], cat
            assert cat["models"][0]["default_effort"] == "low", cat
            assert "efforts" not in cat["models"][1], cat
            r = subprocess.run(
                [TNY, "--provider", "codex", "models"],
                env=env,
                capture_output=True,
                timeout=30,
            )
            assert r.returncode == 0, r.stderr
            assert (
                b"gpt-5.6-sol  \xe2\x80\x94  GPT-5.6-Sol  (active)  [effort: low medium high]"
                in r.stdout
            ), r.stdout
            assert b"gpt-reserve" not in r.stdout, r.stdout
            print(
                "ok  tny models reads the ChatGPT catalog (client_version, hidden slugs dropped)"
            )
            print(
                "ok  $CODEX_HOME/auth.json from `codex login` still works and auto-detects"
            )
        finally:
            stop(mock)

        # ---- expired CLI token → refresh grant, rewritten in place ----------
        expired = access_token(int(time.time()) - 60, tag="old")
        write_json(
            cli_auth_path(home),
            {
                "auth_mode": "chatgpt",
                "tokens": {
                    "access_token": expired,
                    "refresh_token": "refresh-cli",
                    "account_id": ACCOUNT,
                },
                "last_refresh": "2020-01-01T00:00:00Z",
            },
        )
        mock, port = start_mock(
            {
                "MOCK_EXPECT_HEADERS": f"chatgpt-account-id={ACCOUNT};OpenAI-Beta=responses=v1"
            }
        )
        try:
            env = base_env(home, port, issuer_url)
            expect_ok(ask(env, ws, "--provider", "codex"), "cli refresh")
            assert len(Issuer.state["refresh_calls"]) == 1, Issuer.state[
                "refresh_calls"
            ]
            ctype, body = Issuer.state["refresh_calls"][0]
            assert "json" in ctype and body["refresh_token"] == "refresh-cli", (
                ctype,
                body,
            )
            with open(cli_auth_path(home)) as f:
                stored = json.load(f)
            assert (
                jwt_payload(stored["tokens"]["access_token"])["tag"] == "refreshed"
            ), stored
            assert stored["tokens"]["refresh_token"] == "refresh-refreshed", stored
            assert (
                stored["tokens"]["account_id"] == ACCOUNT
                and stored["last_refresh"] > "2020-01-02"
            ), stored
            assert (os.stat(cli_auth_path(home)).st_mode & 0o077) == 0
            assert not os.path.exists(store_path(home)), (
                "CLI refresh must not create tny's store"
            )
            expect_ok(ask(env, ws, "--provider", "codex"), "post-refresh")
            assert len(Issuer.state["refresh_calls"]) == 1, (
                "fresh token refreshed again"
            )
            print(
                "ok  expired Codex CLI token refreshed via auth.openai.com shape, rewritten in place, not re-refreshed"
            )
        finally:
            stop(mock)

        # ---- API-key auth.json → plain OpenAI API shape --------------------
        write_json(
            cli_auth_path(home),
            {"auth_mode": "apikey", "OPENAI_API_KEY": "sk-from-codex-auth"},
        )
        mock, port = start_mock(
            {
                "MOCK_EXPECT_HEADERS": "Authorization=Bearer sk-from-codex-auth",
                "MOCK_REJECT_HEADERS": "chatgpt-account-id;OpenAI-Beta",
            }
        )
        try:
            env = base_env(home, port)
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
            print(
                "ok  API-key auth.json rides plain bearer auth without ChatGPT headers"
            )
        finally:
            stop(mock)
        os.unlink(cli_auth_path(home))

        # ================================================ native login → tny store
        # ---- device-code flow ----------------------------------------------
        Issuer.state["next_tag"] = "device"
        env = base_env(home, 1, issuer_url)
        r = subprocess.run(
            [TNY, "--provider", "codex", "--cwd", ws, "login", "--device"],
            env=env,
            capture_output=True,
            timeout=120,
        )
        assert r.returncode == 0, r.stderr.decode() + r.stdout.decode()
        out = r.stdout.decode()
        assert f"{issuer_url}/codex/device" in out and "ABCD-EFGH" in out, out
        assert Issuer.state["device_polls"] >= 3, Issuer.state["device_polls"]
        ctype, body = Issuer.state["code_calls"][-1]
        assert (
            body["code_verifier"] == "device-verifier-xyz"
            and body["redirect_uri"] == f"{issuer_url}/deviceauth/callback"
        ), body
        assert "x-www-form-urlencoded" in ctype, ctype
        with open(store_path(home)) as f:
            store = json.load(f)
        assert (
            jwt_payload(store["tokens"]["access_token"])["tag"] == "device"
            and store["tokens"]["account_id"] == ACCOUNT
        ), store
        assert (
            store["tokens"]["refresh_token"] == "refresh-device"
            and store["auth_mode"] == "chatgpt"
            and store["expires_at"]
        ), store
        assert (os.stat(store_path(home)).st_mode & 0o077) == 0
        for secret in ("refresh-device", store["tokens"]["access_token"]):
            assert secret not in out + r.stderr.decode(), "token printed"
        print(
            "ok  --device login: usercode → pending polls → exchange with the server verifier → ~/.tny/codex-auth.json 0600"
        )

        # the store now drives turns
        device_token = store["tokens"]["access_token"]
        mock, port = start_mock(
            {
                "MOCK_EXPECT_HEADERS": f"Authorization=Bearer {device_token};chatgpt-account-id={ACCOUNT}"
            }
        )
        try:
            env = base_env(home, port)
            expect_ok(ask(env, ws), "store credential, auto-detected")
            assert "~/.tny/codex-auth.json" in providers_json(env, ws)["hint"]
            print("ok  the tny store auto-detects and drives the profile")
        finally:
            stop(mock)

        # ---- store beats the CLI file; env beats the store -----------------
        write_json(
            cli_auth_path(home),
            {"tokens": {"access_token": fresh, "account_id": "acct_cli"}},
        )
        mock, port = start_mock(
            {
                "MOCK_EXPECT_HEADERS": f"Authorization=Bearer {device_token};chatgpt-account-id={ACCOUNT}"
            }
        )
        try:
            expect_ok(ask(base_env(home, port), ws), "store beats cli")
        finally:
            stop(mock)
        mock, port = start_mock(
            {
                "MOCK_EXPECT_HEADERS": "Authorization=Bearer opaque-env;chatgpt-account-id=acct_env_explicit"
            }
        )
        try:
            env = base_env(home, port)
            env["CHATGPT_ACCESS_TOKEN"] = "opaque-env"
            env["CHATGPT_ACCOUNT_ID"] = "acct_env_explicit"
            expect_ok(ask(env, ws), "env beats store")
        finally:
            stop(mock)
        print("ok  precedence: env > ~/.tny/codex-auth.json > $CODEX_HOME/auth.json")

        # ---- expired store token → refreshed in place, CLI file untouched --
        Issuer.state["refresh_calls"].clear()
        write_json(
            store_path(home),
            {
                "auth_mode": "chatgpt",
                "tokens": {
                    "access_token": access_token(int(time.time()) - 5, tag="stale"),
                    "refresh_token": "refresh-store",
                    "account_id": ACCOUNT,
                },
                "last_refresh": "2020-01-01T00:00:00Z",
            },
        )
        with open(cli_auth_path(home)) as f:
            cli_before = f.read()
        mock, port = start_mock(
            {"MOCK_EXPECT_HEADERS": f"chatgpt-account-id={ACCOUNT}"}
        )
        try:
            expect_ok(ask(base_env(home, port, issuer_url), ws), "store refresh")
            assert [b["refresh_token"] for _, b in Issuer.state["refresh_calls"]] == [
                "refresh-store"
            ], Issuer.state["refresh_calls"]
            with open(store_path(home)) as f:
                store = json.load(f)
            assert (
                jwt_payload(store["tokens"]["access_token"])["tag"] == "refreshed"
                and store["tokens"]["refresh_token"] == "refresh-refreshed"
            ), store
            with open(cli_auth_path(home)) as f:
                assert f.read() == cli_before, (
                    "CLI file must stay untouched when tny's store wins"
                )
            print(
                "ok  expired tny store refreshed in place; the Codex CLI file is left alone"
            )
        finally:
            stop(mock)
        os.unlink(cli_auth_path(home))
        os.unlink(store_path(home))

        # ---- browser flow: PKCE + state + localhost callback ---------------
        if not IS_WASM:
            Issuer.state["next_tag"] = "browser"
            cb_port = free_port()
            env = base_env(home, 1, issuer_url)
            env["TNY_CODEX_CALLBACK_PORT"] = str(cb_port)
            p = subprocess.Popen(
                [TNY, "--provider", "codex", "--cwd", ws, "login"],
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                out = read_until(p, b"/oauth/authorize?", 30)
                q = authorize_url_from(out)
                assert q["response_type"] == "code" and q["client_id"] == CLIENT_ID, q
                assert (
                    q["redirect_uri"] == f"http://localhost:{cb_port}/auth/callback"
                ), q
                assert (
                    q["scope"] == "openid profile email offline_access"
                    and q["code_challenge_method"] == "S256"
                ), q
                assert (
                    q["originator"] == "tny"
                    and q["codex_cli_simplified_flow"] == "true"
                    and q["id_token_add_organizations"] == "true"
                ), q
                assert len(q["state"]) >= 43 and len(q["code_challenge"]) == 43, q
                Issuer.state["challenge"] = q["code_challenge"]
                cb = f"http://127.0.0.1:{cb_port}/auth/callback"
                # a wrong state is refused and the login keeps waiting
                try:
                    urllib.request.urlopen(
                        f"{cb}?code=auth-code-1&state=WRONG", timeout=5
                    )
                    raise AssertionError("wrong state accepted")
                except urllib.error.HTTPError as e:
                    assert e.code == 400, e.code
                time.sleep(0.3)
                assert p.poll() is None, "login exited on a bad-state callback"
                # an unrelated path is a 404
                try:
                    urllib.request.urlopen(
                        f"http://127.0.0.1:{cb_port}/favicon.ico", timeout=5
                    )
                    raise AssertionError("unknown path accepted")
                except urllib.error.HTTPError as e:
                    assert e.code == 404, e.code
                with urllib.request.urlopen(
                    f"{cb}?code=auth-code-1&state={q['state']}", timeout=5
                ) as resp:
                    assert resp.status == 200 and b"Signed in" in resp.read()
                p.wait(timeout=30)
                assert p.returncode == 0, p.stderr.read().decode()
            finally:
                if p.poll() is None:
                    p.kill()
            ctype, body = Issuer.state["code_calls"][-1]
            assert (
                body["redirect_uri"] == f"http://localhost:{cb_port}/auth/callback"
                and body["code"] == "auth-code-1"
            ), body
            assert (
                b64url(hashlib.sha256(body["code_verifier"].encode()).digest())
                == q["code_challenge"]
            ), "PKCE verifier/challenge mismatch"
            with open(store_path(home)) as f:
                store = json.load(f)
            assert (
                jwt_payload(store["tokens"]["access_token"])["tag"] == "browser"
                and store["tokens"]["account_id"] == ACCOUNT
            ), store
            print(
                "ok  browser login: authorize URL (PKCE S256, state, originator tny), bad state refused, callback exchanged, store written"
            )

            # ---- browser flow: pasted redirect URL on a terminal -----------
            os.unlink(store_path(home))
            Issuer.state["next_tag"] = "pasted"
            env["TNY_CODEX_CALLBACK_PORT"] = str(free_port())
            master, slave = pty.openpty()
            p = subprocess.Popen(
                [TNY, "--provider", "codex", "--cwd", ws, "login"],
                env=env,
                stdin=slave,
                stdout=slave,
                stderr=slave,
                close_fds=True,
            )
            os.close(slave)
            try:
                out = read_until(None, b"paste the redirect URL", 30, from_pty=master)
                q = authorize_url_from(out)
                Issuer.state["challenge"] = q["code_challenge"]
                os.write(
                    master,
                    f"{q['redirect_uri']}?code=auth-code-1&state=WRONG\r".encode(),
                )
                out = read_until(None, b"state mismatch", 15, from_pty=master)
                os.write(
                    master,
                    f"{q['redirect_uri']}?code=auth-code-1&state={q['state']}\r".encode(),
                )
                read_until(None, b"Signed in", 30, from_pty=master)
                p.wait(timeout=30)
                assert p.returncode == 0
            finally:
                os.close(master)
                if p.poll() is None:
                    p.kill()
            with open(store_path(home)) as f:
                store = json.load(f)
            assert jwt_payload(store["tokens"]["access_token"])["tag"] == "pasted", (
                store
            )
            print("ok  browser login: pasted redirect URL with state check")
        else:
            print(
                "skip browser login on wasm (no listening socket); device flow covered above"
            )
            write_json(store_path(home), {"tokens": {"access_token": fresh}})

        # ---- logout drops tny's store only ---------------------------------
        write_json(cli_auth_path(home), {"tokens": {"access_token": fresh}})
        env = base_env(home, 1)
        r = subprocess.run(
            [TNY, "--provider", "codex", "--cwd", ws, "logout"],
            env=env,
            capture_output=True,
            timeout=30,
        )
        assert r.returncode == 0, r.stderr.decode()
        assert not os.path.exists(store_path(home)) and os.path.exists(
            cli_auth_path(home)
        )
        assert b"codex logout" in r.stdout, r.stdout
        os.unlink(cli_auth_path(home))
        print(
            "ok  logout removes ~/.tny/codex-auth.json and points at the Codex CLI's own file"
        )

        # ---- no credential: startup-class error, exit 1 --------------------
        r = ask(base_env(home, 1), ws, "--provider", "codex")
        assert r.returncode == 1, r.stderr.decode()
        assert (
            b"tny --provider codex login" in r.stderr
            and b"CHATGPT_ACCESS_TOKEN" in r.stderr
        ), r.stderr
        print("ok  missing credential names login, the env var, and the flag")

        issuer.shutdown()

    print("all codex (ChatGPT backend) integration tests passed")


if __name__ == "__main__":
    main()
