#!/usr/bin/env python3
"""End-to-end: Sign in with ChatGPT for the builtin openai provider
(docs/adr/0186, docs/backends/openai-chatgpt.md).

One in-test server stands in for both auth.openai.com
(TNY_OPENAI_OAUTH_ISSUER: /api/accounts/oauth/token and /revoke) and
api.openai.com/v1 (TNY_OPENAI_SIGNIN_BASE_URL: /responses and /models), with
the ChatGPT-plan route's request rules enforced: Bearer = the stored access
token, store:false + stream:true, no role:"system" input items and none of
the unsupported fields. The test plays the browser by reading the printed
authorize URL and calling tny's loopback callback itself.

What each run proves:
  register   first sign-in: dynamic_agent_client + agent_name_hint, host id,
             127.0.0.1 redirect, PKCE S256, nonce-bound ID token, the issued
             client id required on the callback, form-encoded exchange,
             0600 store, no token on the terminal
  ask        the plan token rides /responses with plan shaping; auto-detect
  guards     an API key wins; a moved base URL never sees the token
  refresh    near-expiry refresh (form, no scope) under the lock; a terminal
             refresh error clears tokens but keeps the registration
  reauth     saved client id, login_hint, no agent_name_hint; another
             client id or another account is refused
  consent    a grant without chatgpt.tokens.use.direct is kept as declined;
             the next sign-in asks prompt=consent
  errors     usage-limit stops without retry and links to ChatGPT usage
  catalog    `models` reads the {"models":[…]} plan catalog
  logout     RFC 7009 revoke, registration kept; --forget deletes the store

The wasm build has no listening socket: it runs the store-seeded parts
(ask, refresh, errors, catalog, logout) and skips the browser sign-in.
"""

import base64
import hashlib
import json
import os
import re
import select
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TNY = os.environ.get(
    "TNY", sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "tny")
)
IS_WASM = "/wasm/" in TNY.replace("\\", "/")

ISSUED_CLIENT = "oaiapp_test_client_1"
SUBJECT = "user-sub-1"
EMAIL = "user@example.test"
PLAN_SCOPE = (
    "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"
)
UNSUPPORTED = (
    "background conversation max_output_tokens max_tool_calls metadata moderation "
    "multi_agent prompt prompt_cache_retention safety_identifier temperature "
    "top_logprobs top_p truncation user previous_response_id"
).split()


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode().rstrip("=")


def jwt(payload: dict) -> str:
    return "%s.%s.sig" % (
        b64url(json.dumps({"alg": "RS256"}).encode()),
        b64url(json.dumps(payload).encode()),
    )


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


# ------------------------------------------------------------ mock services
class Mock(BaseHTTPRequestHandler):
    """Issuer + Responses API stand-in. Class-level state, reset per case."""

    st: dict = {}

    @classmethod
    def reset(cls, **kw):
        cls.st = {
            "issuer": "",
            "token_calls": [],
            "revoke_calls": [],
            "api_calls": [],
            "nonce": None,
            "challenge": None,
            "next_access": "plan-access-1",
            "next_refresh": "plan-refresh-1",
            "valid_access": {"plan-access-1"},
            "subject": SUBJECT,
            "scope": PLAN_SCOPE,
            "refresh_error": None,
            "api_error": None,
        }
        cls.st.update(kw)

    def log_message(self, *a):
        pass

    def _json(self, status, obj):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _tokens(self, client_id):
        st = Mock.st
        access, refresh = st["next_access"], st["next_refresh"]
        st["valid_access"].add(access)
        now = int(time.time())
        return {
            "access_token": access,
            "refresh_token": refresh,
            "id_token": jwt(
                {
                    "iss": st["issuer"],
                    "aud": [client_id],
                    "sub": st["subject"],
                    "email": EMAIL,
                    "nonce": st["nonce"],
                    "iat": now,
                    "exp": now + 3600,
                }
            ),
            "scope": st["scope"],
            "expires_in": 3600,
            "token_type": "Bearer",
        }

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n)
        st = Mock.st
        ctype = self.headers.get("Content-Type", "")
        if self.path.startswith("/api/accounts/oauth/"):
            form = {k: v[0] for k, v in urllib.parse.parse_qs(raw.decode()).items()}
            if self.path == "/api/accounts/oauth/revoke":
                st["revoke_calls"].append((ctype, form))
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            st["token_calls"].append((ctype, form))
            if "x-www-form-urlencoded" not in ctype:
                return self._json(400, {"error": "invalid_request"})
            if form.get("grant_type") == "authorization_code":
                v = form.get("code_verifier", "")
                ok = (
                    form.get("code") == "auth-code-1"
                    and form.get("client_id", "").startswith("oaiapp_")
                    and form.get("resource") == "https://api.openai.com/v1"
                    and b64url(hashlib.sha256(v.encode()).digest()) == st["challenge"]
                )
                if not ok:
                    return self._json(400, {"error": "invalid_grant"})
                return self._json(200, self._tokens(form["client_id"]))
            if form.get("grant_type") == "refresh_token":
                if st["refresh_error"]:
                    return self._json(400, {"error": st["refresh_error"]})
                if form.get("client_id") != ISSUED_CLIENT or "scope" in form:
                    return self._json(400, {"error": "invalid_request"})
                st["next_access"], st["next_refresh"] = (
                    "plan-access-refreshed",
                    "plan-refresh-2",
                )
                tok = self._tokens(ISSUED_CLIENT)
                del tok["id_token"], tok["scope"]  # refresh omits both
                return self._json(200, tok)
            return self._json(400, {"error": "unsupported_grant_type"})
        auth = self.headers.get("Authorization")
        try:
            req = json.loads(raw)
        except ValueError:
            req = {}
        st["api_calls"].append((self.path, auth, req))  # every API POST, any wire
        if self.path.endswith("/responses"):
            if st["api_error"]:
                status, body = st["api_error"]
                return self._json(status, body)
            problems = []
            if self.path == "/v1/responses":
                if auth not in {"Bearer " + t for t in st["valid_access"]}:
                    return self._json(401, {"error": {"code": "invalid_api_key"}})
                if req.get("store") is not False or req.get("stream") is not True:
                    problems.append("store:false + stream:true required")
                problems += [f"unsupported {k}" for k in UNSUPPORTED if k in req]
                for item in req.get("input", []):
                    if item.get("role") == "system":
                        problems.append("role:system input item")
            if problems:
                return self._json(
                    400,
                    {
                        "error": {
                            "code": "subscription_sharing_unsupported_capability",
                            "param": "body",
                            "message": "; ".join(problems),
                        }
                    },
                )
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            for ev in (
                {"type": "response.output_text.delta", "delta": "PLAN-OK"},
                {"type": "response.completed", "response": {"status": "completed"}},
            ):
                self.wfile.write(
                    f"event: {ev['type']}\ndata: {json.dumps(ev)}\n\n".encode()
                )
            return
        return self._json(404, {"error": "no such route"})

    def do_GET(self):
        if self.path.startswith("/v1/models"):
            auth = self.headers.get("Authorization")
            if auth not in {"Bearer " + t for t in Mock.st["valid_access"]}:
                return self._json(401, {"error": {"code": "invalid_api_key"}})
            return self._json(
                200,
                {
                    "models": [
                        {
                            "slug": "gpt-6.1-sol",
                            "display_name": "GPT-6.1",
                            "visibility": "list",
                        },
                        {
                            "slug": "gpt-hidden",
                            "display_name": "x",
                            "visibility": "hide",
                        },
                    ]
                },
            )
        return self._json(404, {"error": "no such route"})


def start_mock():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Mock)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, "http://127.0.0.1:%d" % server.server_address[1]


# ------------------------------------------------------------------ helpers
def base_env(home, issuer):
    env = dict(os.environ)
    for key in list(env):
        if (
            key.endswith("_API_KEY")
            or key.endswith("_BASE_URL")
            or key.startswith("CODEX_")
            or key.startswith("CHATGPT_")
            or key.startswith("TNY_OPENAI_")
            or key.startswith("TNY_CODEX_")
        ):
            env.pop(key)
    env.pop("CLAUDE_CODE_OAUTH_TOKEN", None)
    env.update(
        HOME=home,
        TNY_ISOLATE="0",
        TNY_OPENAI_OAUTH_ISSUER=issuer,
        TNY_OPENAI_SIGNIN_BASE_URL=issuer + "/v1",
    )
    return env


def store_path(home):
    return os.path.join(home, ".tny", "openai-auth.json")


def load_store(home):
    with open(store_path(home)) as f:
        return json.load(f)


def iso(epoch):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(epoch))


def seed_store(home, issuer, access="plan-access-1", expires_in=3600, plan=True):
    os.makedirs(os.path.join(home, ".tny"), exist_ok=True)
    rec = {
        "version": 1,
        "issuer": issuer,
        "client_id": ISSUED_CLIENT,
        "subject": SUBJECT,
        "email": EMAIL,
        "access_token": access,
        "refresh_token": "plan-refresh-1",
        "token_type": "Bearer",
        "scopes": PLAN_SCOPE.split(),
        "plan_usage": plan,
        "expires_in": 3600,
        "expires_at": iso(int(time.time()) + expires_in),
    }
    with open(store_path(home), "w") as f:
        json.dump(rec, f)
    os.chmod(store_path(home), 0o600)


def run(env, ws, *args, timeout=60):
    return subprocess.run(
        [TNY, "--cwd", ws, *args],
        env=env,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=timeout,
    )


def ask(env, ws, *flags):
    return run(env, ws, *flags, "ask", "--json", "--yolo", "--no-save", "say ok")


def no_secret(r, *secrets):
    text = r.stdout.decode(errors="replace") + r.stderr.decode(errors="replace")
    for s in secrets:
        assert s not in text, f"secret {s!r} reached the terminal"


def read_until(proc, needle: bytes, timeout: float) -> bytes:
    buf = b""
    deadline = time.time() + timeout
    fd = proc.stdout.fileno()
    while needle not in buf:
        if time.time() > deadline:
            raise AssertionError(f"timeout waiting for {needle!r}; got {buf!r}")
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            chunk = os.read(fd, 65536)
            if not chunk:
                if proc.poll() is not None:
                    raise AssertionError(f"exited early ({proc.returncode}): {buf!r}")
                continue
            buf += chunk
    # the authorize URL line may still be arriving
    time.sleep(0.1)
    r, _, _ = select.select([fd], [], [], 0.1)
    if r:
        buf += os.read(fd, 65536)
    return buf


def authorize_params(out: bytes) -> dict:
    line = next(
        ln for ln in out.decode().splitlines() if "/api/accounts/authorize?" in ln
    ).strip()
    q = urllib.parse.parse_qs(urllib.parse.urlparse(line).query)
    assert all(len(v) == 1 for v in q.values()), q
    return {k: v[0] for k, v in q.items()}


def callback(port, **params):
    url = f"http://127.0.0.1:{port}/auth/callback?" + urllib.parse.urlencode(params)
    try:
        with urllib.request.urlopen(url, timeout=5) as resp:
            return resp.status
    except urllib.error.HTTPError as e:
        return e.code


def start_login(env, ws):
    port = free_port()
    env = dict(env, TNY_OPENAI_CALLBACK_PORT=str(port))
    p = subprocess.Popen(
        [TNY, "--provider", "openai", "--cwd", ws, "login"],
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    q = authorize_params(read_until(p, b"/api/accounts/authorize?", 30))
    Mock.st["nonce"] = q["nonce"]
    Mock.st["challenge"] = q["code_challenge"]
    return p, port, q


def finish(p):
    try:
        out, err = p.communicate(timeout=30)
    finally:
        if p.poll() is None:
            p.kill()
    return p.returncode, out.decode(), err.decode()


# -------------------------------------------------------------------- cases
def case_register(env, ws, home, issuer):
    Mock.reset(issuer=issuer)
    p, port, q = start_login(env, ws)
    assert q["response_type"] == "code" and q["client_id"] == "dynamic_agent_client", q
    assert q["agent_name_hint"] == "tny", q
    assert re.fullmatch(
        r"urn:uuid:[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}",
        q["ext_agent_host_id"],
    ), q
    assert q["redirect_uri"] == f"http://127.0.0.1:{port}/auth/callback", q
    assert q["scope"] == PLAN_SCOPE and q["resource"] == "https://api.openai.com/v1", q
    assert q["code_challenge_method"] == "S256" and len(q["code_challenge"]) == 43, q
    assert len(q["state"]) >= 43 and len(q["nonce"]) >= 43, q
    for absent in ("login_hint", "prompt", "id_token_hint"):
        assert absent not in q, (absent, q)
    # a registration callback must carry the issued client id
    assert callback(port, code="auth-code-1", state=q["state"]) == 400
    assert (
        callback(port, code="auth-code-1", state="WRONG", client_id=ISSUED_CLIENT)
        == 400
    )
    time.sleep(0.3)
    assert p.poll() is None, "login exited on a rejected callback"
    assert (
        callback(port, code="auth-code-1", state=q["state"], client_id=ISSUED_CLIENT)
        == 200
    )
    rc, out, err = finish(p)
    assert rc == 0, err
    assert f"Signed in with ChatGPT as {EMAIL}" in out, out
    assert "chatgpt.com/settings/usage" in out, out
    for secret in ("plan-access-1", "plan-refresh-1", "auth-code-1", "eyJ"):
        assert secret not in out + err, f"{secret!r} printed"
    ctype, form = Mock.st["token_calls"][-1]
    assert ctype == "application/x-www-form-urlencoded", ctype
    assert (
        form["grant_type"] == "authorization_code"
        and form["client_id"] == ISSUED_CLIENT
    )
    assert form["redirect_uri"] == q["redirect_uri"], form
    st = os.stat(store_path(home))
    assert stat.S_IMODE(st.st_mode) == 0o600, oct(st.st_mode)
    rec = load_store(home)
    assert rec["client_id"] == ISSUED_CLIENT and rec["subject"] == SUBJECT, rec
    assert (
        rec["plan_usage"] is True and rec["ext_agent_host_id"] == q["ext_agent_host_id"]
    )
    assert rec["access_token"] == "plan-access-1", "store holds the access token"
    host_id = open(os.path.join(home, ".tny", "openai-host-id")).read().strip()
    assert host_id == q["ext_agent_host_id"]
    print(
        "ok  register: dynamic client, host id, 127.0.0.1 redirect, PKCE, nonce, 0600 store"
    )
    return q


def case_ask(env, ws, home):
    Mock.reset(issuer=Mock.st["issuer"])
    r = ask(env, ws, "--provider", "openai")
    assert r.returncode == 0, r.stderr.decode()
    out = json.loads(r.stdout.decode())
    assert out.get("provider") == "openai" and "PLAN-OK" in out.get("output", ""), out
    path, auth, req = Mock.st["api_calls"][-1]
    assert path == "/v1/responses" and auth == "Bearer plan-access-1", (path, auth)
    assert req["model"] == "gpt-6.1-sol", req["model"]
    assert isinstance(req.get("instructions"), str) and req["instructions"], req
    no_secret(r, "plan-access-1", "plan-refresh-1")
    # no --provider: a stored sign-in auto-selects openai
    r = ask(env, ws)
    assert r.returncode == 0, r.stderr.decode()
    assert json.loads(r.stdout.decode()).get("provider") == "openai"
    print("ok  ask: Bearer plan token, store:false, no system items/unsupported fields")


def case_status_models(env, ws):
    r = run(env, ws, "--provider", "openai", "status", "--json")
    assert r.returncode == 0, r.stderr.decode()
    s = json.loads(r.stdout.decode())
    assert s["auth"] == "ok" and s["chatgpt_plan"] == {
        "account": EMAIL,
        "manage_usage_url": "https://chatgpt.com/settings/usage",
    }, s
    r = run(env, ws, "--provider", "openai", "models", "--json")
    assert r.returncode == 0, r.stderr.decode()
    ids = [m.get("id") for m in json.loads(r.stdout.decode())["models"]]
    assert "gpt-6.1-sol" in ids and "gpt-hidden" not in ids, ids
    r = run(env, ws, "--provider", "openai", "doctor")
    assert "Using ChatGPT plan" in r.stdout.decode(), r.stdout.decode()
    print("ok  status/models/doctor: plan account, usage link, {models:[…]} catalog")


def case_guards(env, ws):
    # an API key for the endpoint wins over the stored sign-in
    Mock.reset(issuer=Mock.st["issuer"])
    keyed = dict(
        env, OPENAI_API_KEY="sk-test-key", OPENAI_BASE_URL=Mock.st["issuer"] + "/key/v1"
    )
    r = ask(keyed, ws, "--provider", "openai")
    path, auth, req = Mock.st["api_calls"][-1]
    assert path == "/key/v1/responses" and auth == "Bearer sk-test-key", (path, auth)
    # a moved endpoint never sees the plan token, flag or env
    for flags, extra in (
        (("--base-url", Mock.st["issuer"] + "/moved/v1"), {}),
        ((), {"OPENAI_BASE_URL": Mock.st["issuer"] + "/moved/v1"}),
    ):
        Mock.reset(issuer=Mock.st["issuer"])
        ask(dict(env, **extra), ws, "--provider", "openai", *flags)
        calls = Mock.st["api_calls"]
        assert calls and all(c[0] == "/moved/v1/responses" for c in calls), calls
        assert all(c[1] is None for c in calls), "plan token sent to a moved endpoint"
    # the chat wire is not the plan route either
    Mock.reset(issuer=Mock.st["issuer"])
    r = ask(env, ws, "--provider", "openai", "--wire-api", "chat")
    calls = Mock.st["api_calls"]
    assert all(c[0].endswith("/chat/completions") for c in calls), calls
    assert not any(c[1] for c in calls), "plan token sent on the chat wire"
    print(
        "ok  guards: API key wins; moved base URL / chat wire never carry the plan token"
    )


def case_refresh(env, ws, home, issuer):
    seed_store(home, issuer, expires_in=60)  # inside the 180 s early window
    Mock.reset(issuer=issuer)
    r = ask(env, ws, "--provider", "openai")
    assert r.returncode == 0, r.stderr.decode()
    ctype, form = Mock.st["token_calls"][-1]
    assert form == {
        "grant_type": "refresh_token",
        "client_id": ISSUED_CLIENT,
        "refresh_token": "plan-refresh-1",
        "resource": "https://api.openai.com/v1",
    }, form
    assert Mock.st["api_calls"][-1][1] == "Bearer plan-access-refreshed"
    rec = load_store(home)
    assert rec["access_token"] == "plan-access-refreshed"
    assert rec["refresh_token"] == "plan-refresh-2" and rec["plan_usage"] is True, rec
    no_secret(r, "plan-access-refreshed", "plan-refresh-2")
    print("ok  refresh: near-expiry form refresh, rotated tokens, scopes kept")

    seed_store(home, issuer, expires_in=-10)
    Mock.reset(issuer=issuer, refresh_error="invalid_grant")
    r = ask(env, ws, "--provider", "openai")
    assert r.returncode != 0
    assert "tny --provider openai login" in r.stderr.decode(), r.stderr.decode()
    # api.openai.com would stop at connect (no key); this http:// loopback
    # stand-in lets the keyless request through, which must carry nothing
    assert all(c[1] is None for c in Mock.st["api_calls"]), Mock.st["api_calls"]
    rec = load_store(home)
    assert rec["client_id"] == ISSUED_CLIENT and rec["subject"] == SUBJECT, rec
    assert "access_token" not in rec and "refresh_token" not in rec, rec
    print("ok  refresh: terminal invalid_grant clears tokens, keeps the registration")


def case_reauth(env, ws, home, issuer):
    Mock.reset(issuer=issuer)
    p, port, q = start_login(env, ws)
    assert q["client_id"] == ISSUED_CLIENT and "agent_name_hint" not in q, q
    assert q["login_hint"] == EMAIL and "prompt" not in q, q
    # this attempt's callback naming another client id ends the attempt
    assert (
        callback(port, code="auth-code-1", state=q["state"], client_id="oaiapp_other")
        == 400
    )
    rc, out, err = finish(p)
    assert rc != 0 and "client_id_mismatch" in err, err
    assert not Mock.st["token_calls"], "a mismatched client id must not be exchanged"
    p, port, q = start_login(env, ws)
    assert callback(port, code="auth-code-1", state=q["state"]) == 200  # id optional
    rc, out, err = finish(p)
    assert rc == 0, err
    assert load_store(home)["access_token"] == "plan-access-1"
    # a different account on reauthorization is refused
    Mock.reset(issuer=issuer, subject="someone-else", next_access="other-access")
    p, port, q = start_login(env, ws)
    assert callback(port, code="auth-code-1", state=q["state"]) == 200
    rc, out, err = finish(p)
    assert rc != 0 and "different ChatGPT account" in err, err
    assert "logout --forget" in err
    assert load_store(home)["subject"] == SUBJECT
    print("ok  reauth: saved client id + login_hint; other client id / account refused")


def case_access_denied(env, ws):
    Mock.reset(issuer=Mock.st["issuer"])
    p, port, q = start_login(env, ws)
    assert (
        callback(port, error="access_denied", state=q["state"]) == 400
    )  # refusal page
    rc, out, err = finish(p)
    assert rc != 0 and "access_denied" in err, err
    assert not Mock.st["token_calls"], "nothing may be exchanged after access_denied"
    print("ok  access_denied stops the attempt; nothing exchanged")


def case_consent(env, ws, home, issuer):
    Mock.reset(
        issuer=issuer, scope="openid profile email offline_access resource.invoke"
    )
    p, port, q = start_login(env, ws)
    assert callback(port, code="auth-code-1", state=q["state"]) == 200
    rc, out, err = finish(p)
    assert rc != 0 and "plan use was not granted" in out, out + err
    rec = load_store(home)
    assert rec["plan_usage"] is False and rec["client_id"] == ISSUED_CLIENT, rec
    r = ask(env, ws, "--provider", "openai")
    assert r.returncode != 0 and "plan usage was not allowed" in r.stderr.decode(), (
        r.stderr.decode()
    )
    Mock.reset(issuer=issuer)
    p, port, q = start_login(env, ws)
    assert q.get("prompt") == "consent", q
    assert callback(port, code="auth-code-1", state=q["state"]) == 200
    rc, out, err = finish(p)
    assert rc == 0 and load_store(home)["plan_usage"] is True, err
    print(
        "ok  consent: declined plan scope kept as declined; next login asks prompt=consent"
    )


def case_errors(env, ws, home, issuer):
    seed_store(home, issuer)
    Mock.reset(
        issuer=issuer,
        api_error=(
            429,
            {
                "error": {
                    "code": "subscription_sharing_usage_limit_exceeded",
                    "message": "m",
                }
            },
        ),
    )
    r = ask(env, ws, "--provider", "openai")
    err = r.stderr.decode() + r.stdout.decode()
    assert r.returncode != 0 and "chatgpt.com/settings/usage" in err, err
    assert len(Mock.st["api_calls"]) == 1, "usage limit must not be retried"
    Mock.reset(
        issuer=issuer,
        api_error=(
            400,
            {
                "error": {
                    "code": "subscription_sharing_unsupported_capability",
                    "param": "tools[0].type",
                }
            },
        ),
    )
    r = ask(env, ws, "--provider", "openai")
    err = r.stderr.decode() + r.stdout.decode()
    assert "`tools[0].type`" in err, err
    assert len(Mock.st["api_calls"]) == 1
    Mock.reset(issuer=issuer, api_error=(401, {"detail": "identity not accepted"}))
    r = ask(env, ws, "--provider", "openai")
    err = r.stderr.decode() + r.stdout.decode()
    assert (
        "tny --provider openai login" in err and "identity not accepted" not in err
    ), err
    print(
        "ok  errors: usage limit (no retry, usage link), unsupported param, detail body"
    )


def case_logout(env, ws, home, issuer):
    seed_store(home, issuer)
    Mock.reset(issuer=issuer)
    r = run(env, ws, "--provider", "openai", "logout")
    assert r.returncode == 0, r.stderr.decode()
    ctype, form = Mock.st["revoke_calls"][-1]
    assert ctype == "application/x-www-form-urlencoded", ctype
    assert form == {
        "token": "plan-refresh-1",
        "token_type_hint": "refresh_token",
        "client_id": ISSUED_CLIENT,
    }, form
    rec = load_store(home)
    assert rec["client_id"] == ISSUED_CLIENT and "refresh_token" not in rec, rec
    r = run(env, ws, "--provider", "openai", "logout", "--forget")
    assert r.returncode == 0 and not os.path.exists(store_path(home)), r.stderr.decode()
    if not IS_WASM:
        assert os.path.exists(os.path.join(home, ".tny", "openai-host-id"))
    print("ok  logout: revoke form, registration kept; --forget deletes the store")


def main():
    if not os.path.exists(TNY):
        print(f"skip: {TNY} not built")
        return 0
    server, issuer = start_mock()
    try:
        with tempfile.TemporaryDirectory() as home, tempfile.TemporaryDirectory() as ws:
            env = base_env(home, issuer)
            Mock.reset(issuer=issuer)
            if not IS_WASM:
                case_register(env, ws, home, issuer)
            else:
                seed_store(home, issuer)
            case_ask(env, ws, home)
            case_status_models(env, ws)
            case_guards(env, ws)
            case_refresh(env, ws, home, issuer)
            if not IS_WASM:
                case_reauth(env, ws, home, issuer)
                case_access_denied(env, ws)
                case_consent(env, ws, home, issuer)
            case_errors(env, ws, home, issuer)
            case_logout(env, ws, home, issuer)
    finally:
        server.shutdown()
    print("PASS test_openai_signin")
    return 0


if __name__ == "__main__":
    sys.exit(main())
