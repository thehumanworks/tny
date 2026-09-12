#!/usr/bin/env python3
"""Subagent children against a localhost provider fixture (docs/adr/0087).

Every parent runs with TNY_TOOLS=all and drives the real `subagent` tool; each
child is a real `tny ask` process talking to the same fixture. Covered here:
subagent is advertised; automatic create returns a durable stored id; named
and existing-hex ids on create are rejected before any child request; message
continues the same child; inspect/lifecycle report persisted state; the
resolved provider, model, wire, base URL and env/flag/profile/ChatGPT
credentials reach the child without appearing on its argv; permission and
tool-profile ceilings hold; TNY_ISOLATE=0 and ephemeral parents work; the
parent's own environment is not mutated. Stable diagnostics, busy/missing ids,
cancellation and secret-echo cases are in test_subagent_diagnostics.py, which
reuses this fixture. Stdlib only; explicit dummy secrets; no live keys.
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TNY = os.path.abspath(
    sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", "build/tny")
)
ENV_KEY = "dummy-env-key-subagent"
FLAG_KEY = "dummy-flag-key-SENTINELFLAG"
PROFILE_KEY = "dummy-profile-key-subagent"
CHATGPT_TOKEN = "dummy-chatgpt-token-SENTINELCGT"
CHATGPT_ACCOUNT = "acct-dummy-subagent"
INJECTION_ID = "x; touch injected-canary; true"
ID_RE = re.compile(r"subagent ([0-9a-f]{16}) finished")
CREATE_EXAMPLE = '{"action":"create","prompt":"..."}'
E_CREATE_ID = (
    "error: SUBAGENT_INVALID_ARGUMENT: create allocates the child id; omit id. "
    f"Valid: {CREATE_EXAMPLE}, then pass the returned id to message, inspect or "
    "lifecycle"
)


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def child_processes():
    """(pid, argv) of every live subagent child and its forked runner."""
    out = subprocess.run(
        ["ps", "-A", "-ww", "-o", "pid=,args="],
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    ).stdout
    procs = []
    for line in out.splitlines():
        pid, _, args = line.strip().partition(" ")
        if " ask --json --stdin" in args and args.startswith(
            (TNY, os.path.realpath(TNY))
        ):
            procs.append((int(pid), args))
    return procs


def user_texts(body, wire):
    if wire == "chat":
        items = [m for m in body.get("messages", []) if m.get("role") == "user"]
    else:
        items = [
            i
            for i in body.get("input", [])
            if isinstance(i, dict) and i.get("role") == "user"
        ]
    texts = []
    for item in items:
        content = item.get("content")
        if isinstance(content, list):
            content = " ".join(
                part.get("text", "") for part in content if isinstance(part, dict)
            )
        texts.append(content if isinstance(content, str) else "")
    return texts


def tool_outputs(body, wire):
    if wire == "chat":
        return [
            m.get("content")
            for m in body.get("messages", [])
            if m.get("role") == "tool"
        ]
    return [
        i.get("output")
        for i in body.get("input", [])
        if isinstance(i, dict) and i.get("type") == "function_call_output"
    ]


def chat_frames(text=None, call=None):
    if call:
        cid, name, args = call
        delta = {
            "role": "assistant",
            "tool_calls": [
                {
                    "index": 0,
                    "id": cid,
                    "type": "function",
                    "function": {"name": name, "arguments": args},
                }
            ],
        }
        frames = [{"choices": [{"index": 0, "delta": delta}]}]
        finish = "tool_calls"
    else:
        frames = [{"choices": [{"index": 0, "delta": {"content": text}}]}]
        finish = "stop"
    frames.append(
        {
            "choices": [{"index": 0, "delta": {}, "finish_reason": finish}],
            "usage": {"prompt_tokens": 10, "completion_tokens": 2},
        }
    )
    wire = b"".join(f"data: {json.dumps(f)}\n\n".encode() for f in frames)
    return wire + b"data: [DONE]\n\n"


def responses_frames(text=None, call=None):
    events = [{"type": "response.created", "response": {"status": "in_progress"}}]
    if call:
        cid, name, args = call
        item = {
            "type": "function_call",
            "id": "fc_" + cid,
            "call_id": cid,
            "name": name,
            "arguments": args,
        }
        events += [
            {"type": "response.output_item.added", "output_index": 0, "item": item},
            {
                "type": "response.output_item.done",
                "output_index": 0,
                "item": dict(item, status="completed"),
            },
        ]
    else:
        msg = {"type": "message", "id": "msg_1", "role": "assistant", "content": []}
        events += [
            {"type": "response.output_item.added", "output_index": 0, "item": msg},
            {
                "type": "response.output_text.delta",
                "item_id": "msg_1",
                "output_index": 0,
                "delta": text,
            },
            {
                "type": "response.output_text.done",
                "item_id": "msg_1",
                "output_index": 0,
                "text": text,
            },
        ]
    events.append(
        {
            "type": "response.completed",
            "response": {
                "status": "completed",
                "usage": {"input_tokens": 10, "output_tokens": 2},
            },
        }
    )
    return b"".join(
        f"event: {e['type']}\ndata: {json.dumps(e)}\n\n".encode() for e in events
    )


class Provider:
    """One localhost OpenAI-compatible fixture (chat and Responses wires).

    A parent's first user message is `scenario:NAME`; its Nth request after
    N tool results answers with plan step N (a tool call) or PARENT-OK. A
    child's latest user message is `child-task:TAG ...`; TAG picks the reply:
    `fail*` HTTP 401 echoing the credential and URL, `hold*` blocks until
    released, `touch*` tries a terminal write, anything else CHILD-OK TAG.
    """

    def __init__(self, echo_marker="SENTINELECHO"):
        self.lock = threading.Lock()
        self.echo_marker = echo_marker
        self.plans, self.results, self.requests = {}, {}, []
        self.holds, self.arrived = {}, {}
        provider = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def do_GET(self):
                self._send(200, "application/json", b'{"data":[],"models":[]}')

            def do_POST(self):
                raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                provider.handle(self, json.loads(raw or b"{}"))

            def _send(self, status, ctype, data):
                self.send_response(status)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                self.wfile.flush()

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.port = self.server.server_address[1]

    def url(self, prefix="/v1"):
        return f"http://127.0.0.1:{self.port}{prefix}"

    def close(self):
        for event in self.holds.values():
            event.set()
        self.server.shutdown()
        self.server.server_close()

    def plan(self, scenario, *steps):
        """steps: (tool, args) with args a dict or a zero-arg callable."""
        self.plans[scenario] = list(steps)
        self.results[scenario] = []

    def hold(self, tag):
        self.holds[tag] = threading.Event()
        self.arrived[tag] = threading.Event()

    def created_id(self, scenario, index=0):
        match = ID_RE.search(self.results[scenario][index])
        check(match, f"{scenario}: no child id in {self.results[scenario][index]!r}")
        return match.group(1)

    def child_requests(self, tag):
        with self.lock:
            return [r for r in self.requests if r.get("tag") == tag]

    def parent_requests(self, scenario):
        with self.lock:
            return [r for r in self.requests if r.get("scenario") == scenario]

    def handle(self, h, body):
        path = h.path
        wire = (
            "responses"
            if path.endswith("/responses")
            else "chat"
            if path.endswith("/chat/completions")
            else None
        )
        texts = user_texts(body, wire) if wire else []
        outputs = tool_outputs(body, wire) if wire else []
        rec = {
            "path": path,
            "wire": wire,
            "auth": h.headers.get("Authorization"),
            "account": h.headers.get("chatgpt-account-id"),
            "model": body.get("model"),
            "tools": [
                t.get("function", t).get("name") for t in body.get("tools") or []
            ],
        }
        frames = chat_frames if wire == "chat" else responses_frames
        ctype = "text/event-stream"
        if wire is None or "/wrong" in path:
            with self.lock:
                self.requests.append(dict(rec, tag="<wrong>"))
            h._send(500, "application/json", b'{"error":{"message":"wrong route"}}')
            return
        first = texts[0] if texts else ""
        if "scenario:" in first:
            scenario = first.split("scenario:", 1)[1].split()[0]
            with self.lock:
                self.requests.append(dict(rec, scenario=scenario, role="parent"))
                done = self.results.setdefault(scenario, [])
                if len(done) < len(outputs):
                    done.append(outputs[-1])
                steps = self.plans.get(scenario, [])
            if len(outputs) < len(steps):
                tool, args = steps[len(outputs)]
                args = args() if callable(args) else args
                cid = f"call_{scenario}_{len(outputs)}"
                h._send(200, ctype, frames(call=(cid, tool, json.dumps(args))))
            else:
                h._send(200, ctype, frames(text="PARENT-OK"))
            return
        latest = texts[-1] if texts else ""
        tag = (
            latest.split("child-task:", 1)[1].split()[0]
            if "child-task:" in latest
            else "?"
        )
        rec.update(tag=tag, role="child", argv=child_processes())
        with self.lock:
            self.requests.append(rec)
        if tag.startswith("fail"):
            echo = {
                "error": {
                    "message": f"rejected {rec['auth']} at {path} {self.echo_marker}",
                    "type": "invalid_request_error",
                    "code": "invalid_api_key",
                }
            }
            h._send(401, "application/json", json.dumps(echo).encode())
            return
        if tag.startswith("hold"):
            self.arrived[tag].set()
            self.holds[tag].wait(90)
        if tag.startswith("touch") and not outputs:
            call = (
                "child_touch",
                "terminal",
                json.dumps({"command": "touch child-canary"}),
            )
            h._send(200, ctype, frames(call=call))
            return
        if outputs:
            with self.lock:
                self.results.setdefault("child:" + tag, []).append(outputs[-1])
        h._send(200, ctype, frames(text=f"CHILD-OK {tag}"))


def base_env(home, provider, **extra):
    """A clean parent environment: no ambient provider, tny or ChatGPT state."""
    env = {
        k: v
        for k, v in os.environ.items()
        if not (
            k.endswith("_API_KEY")
            or k.endswith("_BASE_URL")
            or k.startswith(("TNY_", "OPENAI_", "CHATGPT_", "CODEX_"))
        )
    }
    env.update(
        {
            "HOME": home,
            "TNY_TOOLS": "all",
            "OPENAI_API_KEY": ENV_KEY,
            "OPENAI_BASE_URL": provider.url(),
            "OPENAI_WIRE_API": "chat",
            "OPENAI_DEFAULT_MODEL": "mock-model",
            "CODEX_HOME": os.path.join(home, "codex-home"),
            "TNY_CODEX_BIN": os.path.join(home, "no-such-codex"),
        }
    )
    env.update(extra)
    return env


DEFAULT_FLAGS = ("--provider", "openai", "--wire-api", "chat")


def run_parent(env, workspace, scenario, flags=DEFAULT_FLAGS, expect_exit=0):
    run = subprocess.run(
        [TNY, *flags, "ask", "--json", f"scenario:{scenario} go"],
        cwd=workspace,
        env=env,
        text=True,
        capture_output=True,
        timeout=120,
        check=False,
    )
    check(
        run.returncode == expect_exit,
        f"{scenario}: parent exit {run.returncode}: {run.stderr}\n{run.stdout}",
    )
    try:
        payload = json.loads(run.stdout)
    except json.JSONDecodeError as exc:
        raise Fail(f"{scenario}: stdout was not JSON: {run.stdout!r}") from exc
    payload["_stderr"] = run.stderr
    return payload


def session_dirs(home):
    root = os.path.join(home, ".tny", "sessions")
    found = {}
    if os.path.isdir(root):
        for ws in os.listdir(root):
            for sid in os.listdir(os.path.join(root, ws)):
                found[sid] = os.path.join(root, ws, sid)
    return found


def session_doc(home, sid):
    with open(os.path.join(session_dirs(home)[sid], "session.json"), "rb") as f:
        return json.loads(f.read())


def session_bytes(home, sid):
    with open(os.path.join(session_dirs(home)[sid], "session.json"), "rb") as f:
        return f.read()


def statuses(payload):
    return [(c["name"], c["status"]) for c in payload.get("tool_calls", [])]


def success_text(sid, output):
    return (
        f"subagent {sid} finished.\nid: {sid} (use action=message id={sid} to "
        f"continue)\nresult:\n{output}"
    )


def assert_child_argv(provider, tag, present=(), absent=()):
    reqs = provider.child_requests(tag)
    check(reqs, f"{tag}: the child never reached the provider")
    lines = [args for _pid, args in reqs[0]["argv"]]
    check(lines, f"{tag}: no live child process was observed")
    for line in lines:
        for token in present:
            check(token in line, f"{tag}: {token!r} missing from child argv {line!r}")
        for token in absent:
            check(
                token not in line, f"{tag}: {token!r} leaked onto child argv {line!r}"
            )
    return lines


def scenario_reproduce(provider, home, workspace):
    """The #123 reproduction: all-tools parent, automatic create, then
    message/inspect/lifecycle on the same durable child."""
    env = base_env(home, provider)
    s = "reproduce"

    def child():
        return provider.created_id(s)

    provider.plan(
        s,
        ("subagent", {"action": "create", "prompt": "child-task:ok1 write a haiku"}),
        ("terminal", {"command": "env"}),
        (
            "subagent",
            lambda: {
                "action": "message",
                "id": child(),
                "prompt": "child-task:ok2 again",
            },
        ),
        ("subagent", lambda: {"action": "inspect", "id": child()}),
        ("subagent", lambda: {"action": "lifecycle", "id": child()}),
    )
    before = set(session_dirs(home))
    payload = run_parent(env, workspace, s)
    check(payload.get("output") == "PARENT-OK", payload)
    first = provider.parent_requests(s)[0]
    check("subagent" in first["tools"], f"subagent not advertised: {first['tools']}")
    check(
        statuses(payload)
        == [("subagent", "success"), ("terminal", "success")]
        + [("subagent", "success")] * 3,
        payload,
    )
    sid = child()
    results = provider.results[s]
    check(results[0] == success_text(sid, "CHILD-OK ok1"), results[0])
    check("TNY_SUBAGENT_" not in results[1], "parent environment gained a carrier")
    check(results[2] == success_text(sid, "CHILD-OK ok2"), results[2])
    check(results[3].startswith(f"subagent {sid}\ntitle: "), results[3])
    for line in ("turns: 2", "status: done", "exit_code: 0", "running: false"):
        check(f"\n{line}\n" in results[3], f"inspect lacks {line!r}: {results[3]!r}")
    check(results[3].endswith("resumable: true\nresult:\nCHILD-OK ok2"), results[3])
    check(
        results[4]
        == f"subagent {sid}\nstatus: done\nexit_code: 0\nrunning: false\nresumable: true",
        results[4],
    )
    created = set(session_dirs(home)) - before
    check(sid in created and len(created) == 2, f"sessions created: {created}")
    doc = session_doc(home, sid)
    check(doc.get("turns") == 2 and doc.get("status") == "done", doc)
    users = [m["content"] for m in doc["messages"] if m["role"] == "user"]
    check([u.split()[0] for u in users] == ["child-task:ok1", "child-task:ok2"], users)
    for tag in ("ok1", "ok2"):
        req = provider.child_requests(tag)[0]
        check(req["auth"] == f"Bearer {ENV_KEY}", req)
        check(
            req["model"] == "mock-model" and req["path"] == "/v1/chat/completions", req
        )
    assert_child_argv(
        provider,
        "ok1",
        present=(
            "--provider openai",
            "--api-key-env TNY_SUBAGENT_API_KEY",
            "--base-url-env TNY_SUBAGENT_BASE_URL",
            "--wire-api chat",
            "--model mock-model",
            "--permission-mode yolo",
        ),
        absent=(ENV_KEY, f"127.0.0.1:{provider.port}", "--resume-id"),
    )
    assert_child_argv(
        provider, "ok2", present=(f"--resume-id {sid}",), absent=(ENV_KEY,)
    )
    return sid


def scenario_rejected_ids(provider, home, workspace, existing):
    """Named and existing-hex ids on create never start a child or touch
    the existing session; a shell-metacharacter id is refused as data."""
    env = base_env(home, provider)
    s = "rejected"
    provider.plan(
        s,
        (
            "subagent",
            {
                "action": "create",
                "prompt": "child-task:named x",
                "id": "wallpaper-blue",
            },
        ),
        (
            "subagent",
            {"action": "create", "prompt": "child-task:hexed x", "id": existing},
        ),
        (
            "subagent",
            {"action": "message", "id": INJECTION_ID, "prompt": "child-task:inj x"},
        ),
    )
    before_ids = set(session_dirs(home))
    before_bytes = session_bytes(home, existing)
    payload = run_parent(env, workspace, s)
    check(statuses(payload) == [("subagent", "error")] * 3, payload)
    results = provider.results[s]
    check(results[0] == E_CREATE_ID and results[1] == E_CREATE_ID, results)
    check(
        results[2]
        == "error: SUBAGENT_INVALID_ARGUMENT: message needs the 16-character lowercase "
        "hex id returned by create. Example: "
        '{"action":"message","id":"<id from create>","prompt":"..."}',
        results[2],
    )
    for tag in ("named", "hexed", "inj"):
        check(
            not provider.child_requests(tag), f"{tag}: a rejected call reached a child"
        )
    added = set(session_dirs(home)) - before_ids
    check(len(added) == 1, f"only the parent session may be created: {added}")
    check(session_bytes(home, existing) == before_bytes, "existing child was modified")
    check(
        not os.path.exists(os.path.join(workspace, "injected-canary")), "injection ran"
    )


def scenario_flag_credentials(provider, home, workspace):
    """--api-key-env and a secret-bearing --base-url reach the child through
    its private environment; the ambient OPENAI_* values are not used."""
    env = base_env(
        home,
        provider,
        SUBAGENT_FLAG_KEY=FLAG_KEY,
        OPENAI_BASE_URL=provider.url("/wrong/v1"),
    )
    s = "flagcreds"
    provider.plan(
        s, ("subagent", {"action": "create", "prompt": "child-task:okflag x"})
    )
    flags = (
        *DEFAULT_FLAGS,
        "--api-key-env",
        "SUBAGENT_FLAG_KEY",
        "--base-url",
        provider.url("/gw-SENTINELURL-7c1/v1"),
    )
    payload = run_parent(env, workspace, s, flags=flags)
    check(statuses(payload) == [("subagent", "success")], payload)
    req = provider.child_requests("okflag")[0]
    check(req["path"] == "/gw-SENTINELURL-7c1/v1/chat/completions", req)
    check(req["auth"] == f"Bearer {FLAG_KEY}", req)
    check(not provider.child_requests("<wrong>"), "a child used the ambient base URL")
    assert_child_argv(provider, "okflag", absent=(FLAG_KEY, "SENTINELURL", ENV_KEY))


def scenario_profile(provider, home, workspace):
    """A settings profile's resolved URL, key and flag-chosen model."""
    settings = os.path.join(home, ".tny", "settings.json")
    with open(settings, encoding="utf-8") as f:
        saved = f.read()
    with open(settings, "w", encoding="utf-8") as f:
        json.dump(
            {
                "last_provider": "codex",
                "gw": {
                    "base_url": provider.url("/profile/v1"),
                    "api_key_env": "GW_PROFILE_KEY",
                    "wire_api": "chat",
                },
            },
            f,
        )
    try:
        env = base_env(home, provider, GW_PROFILE_KEY=PROFILE_KEY)
        s = "profile"
        provider.plan(
            s, ("subagent", {"action": "create", "prompt": "child-task:okprof x"})
        )
        payload = run_parent(
            env, workspace, s, flags=("--provider", "gw", "--model", "profile-model")
        )
        check(statuses(payload) == [("subagent", "success")], payload)
        req = provider.child_requests("okprof")[0]
        check(req["path"] == "/profile/v1/chat/completions", req)
        check(
            req["auth"] == f"Bearer {PROFILE_KEY}" and req["model"] == "profile-model",
            req,
        )
        assert_child_argv(
            provider,
            "okprof",
            present=("--provider gw", "--model profile-model"),
            absent=(PROFILE_KEY, "/profile/v1"),
        )
    finally:
        with open(settings, "w", encoding="utf-8") as f:
            f.write(saved)


def scenario_chatgpt_flag(provider, home, workspace):
    """codex profile with the file-less --chatgpt-token/--chatgpt-account-id
    source: the child sends the same bearer and account on the Responses wire."""
    env = base_env(home, provider, TNY_CODEX_BASE_URL=provider.url("/codex-SENTINELCX"))
    s = "chatgpt"
    provider.plan(s, ("subagent", {"action": "create", "prompt": "child-task:okcgt x"}))
    flags = (
        "--provider",
        "codex",
        "--chatgpt-token",
        CHATGPT_TOKEN,
        "--chatgpt-account-id",
        CHATGPT_ACCOUNT,
        "--model",
        "mock-codex",
    )
    payload = run_parent(env, workspace, s, flags=flags)
    check(statuses(payload) == [("subagent", "success")], payload)
    parent = provider.parent_requests(s)[0]
    check(
        parent["wire"] == "responses" and parent["account"] == CHATGPT_ACCOUNT, parent
    )
    req = provider.child_requests("okcgt")[0]
    check(
        req["wire"] == "responses" and req["path"] == "/codex-SENTINELCX/responses", req
    )
    check(
        req["auth"] == f"Bearer {CHATGPT_TOKEN}" and req["account"] == CHATGPT_ACCOUNT,
        req,
    )
    check(req["model"] == "mock-codex", req)
    assert_child_argv(
        provider,
        "okcgt",
        present=("--provider codex",),
        absent=(CHATGPT_TOKEN, "SENTINELCX"),
    )


def scenario_permission_ceiling(provider, home, workspace):
    """An auto parent's child stays in auto even though settings allow the
    parent's subagent and the environment asks for yolo: its terminal write is
    denied, and `tny ask` ends a denied turn with exit 2 (CHILD_FAILED)."""
    settings = os.path.join(home, ".tny", "settings.json")
    with open(settings, encoding="utf-8") as f:
        saved = f.read()
    with open(settings, "w", encoding="utf-8") as f:
        json.dump({"permission": {"subagent": "allow"}, "permission_mode": "yolo"}, f)
    try:
        env = base_env(home, provider, TNY_PERMISSION_MODE="yolo")
        s = "ceiling"
        provider.plan(
            s, ("subagent", {"action": "create", "prompt": "child-task:touch x"})
        )
        payload = run_parent(
            env, workspace, s, flags=(*DEFAULT_FLAGS, "--permission-mode", "auto")
        )
        check(statuses(payload) == [("subagent", "error")], payload)
        check(
            not os.path.exists(os.path.join(workspace, "child-canary")), "child widened"
        )
        result = provider.results[s][0]
        sid = re.search(r"child ([0-9a-f]{16}) failed", result)
        check(sid, result)
        sid = sid.group(1)
        check(
            result
            == f"error: SUBAGENT_CHILD_FAILED: child {sid} failed (exit 2); its session "
            "keeps the details. Check "
            f'{{"action":"lifecycle","id":"{sid}"}} and retry with action=message',
            result,
        )
        doc = session_doc(home, sid)
        check(doc.get("status") == "error" and doc.get("exit_code") == 2, doc)
        denied = [m["content"] for m in doc["messages"] if m["role"] == "tool"]
        check(denied and "permission" in denied[0].lower(), f"not denied: {denied!r}")
        assert_child_argv(provider, "touch", present=("--permission-mode auto",))
    finally:
        with open(settings, "w", encoding="utf-8") as f:
            f.write(saved)


def scenario_in_process(provider, home, workspace):
    """TNY_ISOLATE=0: in-process parent and child; the child never records a
    status, so lifecycle reports unknown/null rather than success."""
    env = base_env(home, provider, TNY_ISOLATE="0")
    s = "inproc"
    provider.plan(
        s,
        ("subagent", {"action": "create", "prompt": "child-task:okiso1 x"}),
        (
            "subagent",
            lambda: {
                "action": "message",
                "id": provider.created_id(s),
                "prompt": "child-task:okiso2 x",
            },
        ),
        ("subagent", lambda: {"action": "lifecycle", "id": provider.created_id(s)}),
        ("subagent", lambda: {"action": "inspect", "id": provider.created_id(s)}),
    )
    payload = run_parent(env, workspace, s)
    check(statuses(payload) == [("subagent", "success")] * 4, payload)
    sid = provider.created_id(s)
    results = provider.results[s]
    check(results[1] == success_text(sid, "CHILD-OK okiso2"), results[1])
    check(
        results[2]
        == f"subagent {sid}\nstatus: unknown\nexit_code: null\nrunning: false\nresumable: true",
        results[2],
    )
    check(
        "\nturns: 2\n" in results[3] and "\nstatus: unknown\n" in results[3], results[3]
    )
    check(session_doc(home, sid).get("turns") == 2, "in-process child lost a turn")


def scenario_ephemeral(provider, home):
    """An ephemeral parent's child is one-shot and stores nothing."""
    eph_home = os.path.join(home, "ephemeral-home")
    workspace = os.path.join(eph_home, "ws")
    os.makedirs(workspace)
    env = base_env(eph_home, provider)
    s = "ephemeral"
    provider.plan(s, ("subagent", {"action": "create", "prompt": "child-task:okeph x"}))
    payload = run_parent(env, workspace, s, flags=(*DEFAULT_FLAGS, "--ephemeral"))
    check(statuses(payload) == [("subagent", "success")], payload)
    check(
        provider.results[s][0]
        == "ephemeral subagent finished; no resumable id was stored.\nresult:\nCHILD-OK okeph",
        provider.results[s][0],
    )
    assert_child_argv(provider, "okeph", present=("--ephemeral",))
    check(
        not os.path.exists(os.path.join(eph_home, ".tny", "sessions")),
        "ephemeral stored",
    )


def make_home(tmp):
    home = os.path.join(tmp, "home")
    workspace = os.path.join(tmp, "ws")
    os.makedirs(os.path.join(home, ".tny"))
    os.makedirs(workspace)
    # An earlier host-provider chat left last_provider behind; the child must
    # still run the parent's resolved native provider.
    with open(os.path.join(home, ".tny", "settings.json"), "w", encoding="utf-8") as f:
        json.dump({"last_provider": "codex"}, f)
    return home, workspace


def run():
    provider = Provider()
    try:
        with tempfile.TemporaryDirectory(prefix="tny-subagent-integration-") as tmp:
            home, workspace = make_home(tmp)
            sid = scenario_reproduce(provider, home, workspace)
            scenario_rejected_ids(provider, home, workspace, sid)
            scenario_flag_credentials(provider, home, workspace)
            scenario_profile(provider, home, workspace)
            scenario_chatgpt_flag(provider, home, workspace)
            scenario_permission_ceiling(provider, home, workspace)
            scenario_in_process(provider, home, workspace)
            scenario_ephemeral(provider, home)
    finally:
        provider.close()
    print(
        "ok  subagent: all-tools create/message/inspect/lifecycle, rejected ids, "
        "private config inheritance, ceilings, in-process and ephemeral parents"
    )


if __name__ == "__main__":
    try:
        run()
    except Fail as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        sys.exit(1)
