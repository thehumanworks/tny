#!/usr/bin/env python3
"""End-to-end ephemeral CLI and ACP surface checks. Stdlib only."""
import json
import os
import subprocess
import sys
import tempfile
import time

TNY = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", "build/tny")


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def base_cmd():
    # A loopback URL satisfies ACP's credential preflight without making a
    # network request in the initialize/load-only scenario below.
    return [TNY, "--provider", "openai", "--base-url",
            "http://127.0.0.1:9/v1"]


def request(proc, mid, method, params):
    proc.stdin.write(json.dumps({
        "jsonrpc": "2.0", "id": mid, "method": method, "params": params,
    }) + "\n")
    proc.stdin.flush()
    deadline = time.time() + 10
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            raise Fail(f"ACP server closed stdout while waiting for {method}")
        msg = json.loads(line)
        if msg.get("id") == mid:
            return msg
    raise Fail(f"timed out waiting for {method}")


def run():
    with tempfile.TemporaryDirectory(prefix="tny-ephemeral-integration-") as tmp:
        home = os.path.join(tmp, "home")
        workspace = os.path.join(tmp, "workspace")
        os.makedirs(home)
        os.makedirs(workspace)
        env = dict(os.environ)
        env["HOME"] = home
        for key in list(env):
            if key.endswith("_API_KEY") or key.endswith("_BASE_URL"):
                env.pop(key)

        # Leading global mode reaches the ACP surface. The server must not
        # claim saved-session support and must reject an attempted load.
        proc = subprocess.Popen(
            base_cmd() + ["--ephemeral", "acp"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1,
            cwd=workspace, env=env,
        )
        try:
            init = request(proc, 1, "initialize", {
                "protocolVersion": 1,
                "clientCapabilities": {},
                "clientInfo": {"name": "ephemeral-test", "version": "0"},
            })
            check("result" in init, f"initialize failed: {init}")
            caps = init["result"]["agentCapabilities"]
            check(caps.get("loadSession") is False,
                  f"ephemeral ACP advertised loadSession: {init}")

            load = request(proc, 2, "session/load", {"sessionId": "saved-id"})
            check("error" in load, f"ephemeral session/load succeeded: {load}")
            check("ephemeral" in load["error"].get("message", "").lower(),
                  f"session/load error did not explain the mode: {load}")
        finally:
            try:
                proc.stdin.close()
            except OSError:
                pass
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        check(proc.returncode == 0, proc.stderr.read())

        # Saved-state entry points fail before any backend connection.
        resumed = subprocess.run(
            base_cmd() + ["--ephemeral", "resume", "last"],
            cwd=workspace, env=env, text=True, capture_output=True,
        )
        check(resumed.returncode == 1,
              f"ephemeral resume exit {resumed.returncode}: {resumed.stderr}")
        check("cannot resume" in resumed.stderr.lower(), resumed.stderr)

        ask = subprocess.run(
            base_cmd() + ["ask", "--ephemeral", "--resume", "last", "hello"],
            cwd=workspace, env=env, text=True, capture_output=True,
        )
        check(ask.returncode == 1,
              f"ephemeral ask/resume exit {ask.returncode}: {ask.stderr}")
        check("incompatible" in ask.stderr.lower(), ask.stderr)

        # None of these surfaces may materialize a conversation store.
        sessions = os.path.join(home, ".tny", "sessions")
        history = os.path.join(home, ".tny", "history")
        check(not os.path.exists(sessions), f"created session store: {sessions}")
        check(not os.path.exists(history), f"created prompt history: {history}")

    print("ok  ephemeral: ACP capability/load guard and CLI resume guards")


if __name__ == "__main__":
    try:
        run()
    except Fail as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
