#!/usr/bin/env python3
"""test_acp_server.py — drive `tny acp` as an ACP client.

Spawns tests/integration/mock_openai.py as the provider, then speaks
JSON-RPC 2.0 over the server's stdio: initialize, session/new, session/prompt,
session/load, plus the unsupported-content and no-credential error paths.
Stdlib only.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TNY = os.environ.get("TNY", os.path.join(ROOT, "build-acp", "tny"))
MOCK = os.path.join(ROOT, "tests", "integration", "mock_openai.py")


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Client:
    """One ACP connection to a `tny acp` child."""

    def __init__(self, env, cwd, errpath):
        self.errfh = open(errpath, "w")
        self.proc = subprocess.Popen(
            [TNY, "acp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.errfh, cwd=cwd, env=env, text=True, bufsize=1)
        self.next_id = 1
        self.updates = []

    def send(self, obj):
        self.proc.stdin.write(json.dumps(obj) + "\n")
        self.proc.stdin.flush()

    def request(self, method, params, timeout=60):
        mid = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": mid, "method": method, "params": params})
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise Fail(f"server closed stdout while waiting for {method}")
            msg = json.loads(line)
            if msg.get("method") == "session/update":
                self.updates.append(msg["params"]["update"])
                continue
            if msg.get("method") == "session/request_permission":
                # allow anything the loop asks for; keeps the turn moving
                self.send({"jsonrpc": "2.0", "id": msg["id"],
                           "result": {"outcome": {"outcome": "selected",
                                                  "optionId": "allow"}}})
                continue
            if msg.get("id") == mid:
                return msg
        raise Fail(f"timed out waiting for {method}")

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.errfh.close()


def base_env(home, port=None):
    env = dict(os.environ)
    env["HOME"] = home
    env["OPENAI_API_KEY"] = "test-key-not-a-real-secret"
    env.pop("TNY_PERMISSION_MODE", None)
    if port:
        env["OPENAI_BASE_URL"] = f"http://127.0.0.1:{port}/v1"
    return env


def run(tmp):
    port = free_port()
    # stderr is dropped: cancelling a turn closes the HTTP stream mid-response
    # and the stdlib mock logs a broken pipe for it.
    mock = subprocess.Popen([sys.executable, MOCK, str(port)],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True)
    ready = mock.stdout.readline()
    check("ready" in ready, f"mock provider did not start: {ready!r}")

    home = os.path.join(tmp, "home")
    ws = os.path.join(tmp, "ws")
    os.makedirs(home)
    os.makedirs(ws)
    with open(os.path.join(ws, "hello.txt"), "w") as fh:
        fh.write("hi\n")

    try:
        c = Client(base_env(home, port), ws, os.path.join(tmp, "server.err"))

        # ---- initialize ----
        r = c.request("initialize", {
            "protocolVersion": 1,
            "clientCapabilities": {"fs": {"readTextFile": False,
                                          "writeTextFile": False}},
            "clientInfo": {"name": "test-client", "version": "0"},
        })
        check("result" in r, f"initialize failed: {r}")
        check(r["result"]["protocolVersion"] == 1, f"bad protocolVersion: {r}")
        check(r["result"]["agentCapabilities"]["loadSession"] is True,
              f"loadSession not advertised: {r}")
        print("ok  initialize: protocolVersion 1, loadSession advertised")

        # ---- session/new ----
        r = c.request("session/new", {"cwd": ws, "mcpServers": []})
        check("result" in r, f"session/new failed: {r}")
        sid = r["result"]["sessionId"]
        check(bool(sid), "session/new returned no sessionId")
        print(f"ok  session/new: {sid}")

        # ---- session/prompt ----
        c.updates = []
        r = c.request("session/prompt", {
            "sessionId": sid,
            "prompt": [{"type": "text", "text": "list the workspace"}],
        })
        check("result" in r, f"session/prompt failed: {r}")
        check(r["result"]["stopReason"] == "end_turn",
              f"stopReason is {r['result']}")
        text = "".join(u["content"]["text"] for u in c.updates
                       if u["sessionUpdate"] == "agent_message_chunk")
        check("MOCK-OK" in text, f"no MOCK-OK in streamed text: {text!r}")
        starts = [u for u in c.updates if u["sessionUpdate"] == "tool_call"]
        ends = [u for u in c.updates if u["sessionUpdate"] == "tool_call_update"]
        check(starts and ends, f"no tool_call updates: {c.updates}")
        check(starts[0]["kind"] == "read", f"bad tool kind: {starts[0]}")
        check(ends[0]["status"] == "completed", f"bad tool status: {ends[0]}")
        print(f"ok  session/prompt: end_turn, {len(c.updates)} updates, "
              f"tool_call {starts[0]['title']}")

        # ---- session/cancel: never wedges the connection ----
        c.updates = []
        mid = c.next_id
        c.next_id += 1
        c.send({"jsonrpc": "2.0", "id": mid, "method": "session/prompt",
                "params": {"sessionId": sid,
                           "prompt": [{"type": "text", "text": "cancel me"}]}})
        c.send({"jsonrpc": "2.0", "method": "session/cancel",
                "params": {"sessionId": sid}})
        deadline = time.time() + 60
        resp = None
        while resp is None and time.time() < deadline:
            line = c.proc.stdout.readline()
            check(bool(line), "server closed stdout during the cancel turn")
            msg = json.loads(line)
            if msg.get("id") == mid:
                resp = msg
            elif msg.get("method") == "session/request_permission":
                c.send({"jsonrpc": "2.0", "id": msg["id"],
                        "result": {"outcome": {"outcome": "cancelled"}}})
        check(resp is not None, "no answer to the cancelled prompt")
        check("result" in resp, f"cancelled prompt errored: {resp}")
        # the mock provider answers in milliseconds, so either outcome is legal
        check(resp["result"]["stopReason"] in ("cancelled", "end_turn"),
              f"bad stopReason after cancel: {resp}")
        print(f"ok  session/cancel: stopReason {resp['result']['stopReason']}, "
              f"connection still live")

        # ---- images are refused cleanly ----
        r = c.request("session/prompt", {
            "sessionId": sid,
            "prompt": [{"type": "image", "mimeType": "image/png", "data": "AAAA"}],
        })
        check("error" in r, f"image prompt should fail: {r}")
        check(r["error"]["code"] == -32602, f"wrong error code: {r}")
        check("image" in r["error"]["message"], f"unhelpful message: {r}")
        print("ok  session/prompt: image blocks refused with -32602")

        # ---- unknown method ----
        r = c.request("nonsense/method", {})
        check(r.get("error", {}).get("code") == -32601, f"want -32601: {r}")
        print("ok  unknown method: -32601")

        # ---- session/load replays and rebinds ----
        c.updates = []
        r = c.request("session/load", {"sessionId": sid, "cwd": ws,
                                       "mcpServers": []})
        check("result" in r, f"session/load failed: {r}")
        kinds = [u["sessionUpdate"] for u in c.updates]
        check("user_message_chunk" in kinds,
              f"history was not replayed: {kinds}")
        print(f"ok  session/load: replayed {len(c.updates)} history chunks")

        r = c.request("session/load", {"sessionId": "does-not-exist"})
        check(r.get("error", {}).get("code") == -32602, f"want -32602: {r}")
        print("ok  session/load: unknown sessionId rejected")

        c.close()

        # ---- no credential: initialize must fail, not limp ----
        env = base_env(home)
        env.pop("OPENAI_API_KEY")
        env.pop("OPENAI_BASE_URL", None)
        c2 = Client(env, ws, os.path.join(tmp, "server2.err"))
        r = c2.request("initialize", {"protocolVersion": 1,
                                      "clientCapabilities": {}})
        check("error" in r, f"initialize without a key should fail: {r}")
        check("OPENAI_API_KEY" in r["error"]["message"], f"unhelpful: {r}")
        print("ok  initialize: fails closed without a provider credential")
        c2.close()
    finally:
        mock.terminate()
        mock.wait()


def main():
    if not os.access(TNY, os.X_OK):
        print(f"test_acp_server: no binary at {TNY} "
              f"(make BUILD=build-acp release)", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="tny-acp-srv.") as tmp:
        try:
            run(tmp)
        except Fail as exc:
            print(f"FAIL: {exc}", file=sys.stderr)
            for name in ("server.err", "server2.err"):
                path = os.path.join(tmp, name)
                if os.path.exists(path):
                    with open(path) as fh:
                        body = fh.read().strip()
                    if body:
                        print(f"--- {name} ---\n{body}", file=sys.stderr)
            return 1
    print("PASS test_acp_server.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
