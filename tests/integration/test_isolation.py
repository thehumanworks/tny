#!/usr/bin/env python3
"""End-to-end: forked-turn isolation (docs/adr/0053).

Every native turn runs in a detached session runner; the caller is only a
renderer. Covered here, against the mock OpenAI provider:

  - a foreground `tny ask` streams through the runner and its answer,
    exit code, and --json bytes are unchanged;
  - SIGKILLing the foreground client mid-turn does NOT kill the agent:
    the runner finishes the turn, finalizes status/result into the
    session, and the session then resumes normally;
  - the same survival holds for an interactive TUI (dumb mode over pipes)
    killed mid-turn;
  - `tny session attach <id>` streams a live -B turn (snapshot + events)
    and reports the turn finishing;
  - TNY_ISOLATE=0 keeps the in-process path (no runner pid file).

With TNY_TEST_EXPECT_WASM=1 the isolation layer is absent by design
(no fork, decision 10): this suite only checks that a plain foreground
ask still works in-process there — and other suites already cover that —
so it exits trivially.
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TNY = (
    sys.argv[1]
    if len(sys.argv) > 1
    else os.environ.get("TNY", os.path.join(ROOT, "build", "tny"))
)
MOCK = os.path.join(ROOT, "tests", "integration", "mock_openai.py")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start_mock(**extra):
    port = free_port()
    m = subprocess.Popen(
        [sys.executable, MOCK, str(port)],
        env=dict(os.environ, MOCK_EXPECT_WIRE="responses", **extra),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    line = m.stdout.readline().decode()
    assert "ready" in line, f"mock did not start: {line!r}"
    return m, port


def poll(pred, timeout_s, what):
    """Bounded condition wait — never a bare sleep (flake policy)."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        v = pred()
        if v:
            return v
        time.sleep(0.05)
    raise AssertionError(f"timed out after {timeout_s}s waiting for {what}")


class Ctx:
    def __init__(self, home):
        self.home = home
        self.ws = os.path.join(home, "ws")
        os.makedirs(self.ws)
        for name in ("a.txt", "b.txt", "c.txt"):
            open(os.path.join(self.ws, name), "w").write("x\n")

    def env(self, port, **kw):
        return dict(
            os.environ,
            HOME=self.home,
            OPENAI_BASE_URL=f"http://127.0.0.1:{port}/v1",
            OPENAI_API_KEY="test-key-not-real",
            **kw,
        )

    def sdirs(self):
        import glob

        return glob.glob(os.path.join(self.home, ".tny", "sessions", "*", "*"))

    def newest_doc(self):
        docs = []
        for d in self.sdirs():
            p = os.path.join(d, "session.json")
            if os.path.exists(p):
                docs.append((os.path.getmtime(p), d, p))
        if not docs:
            return None, None
        docs.sort()
        _, d, p = docs[-1]
        return d, json.load(open(p))


def test_foreground_streams_and_finishes(ctx, port):
    r = subprocess.run(
        [TNY, "--cwd", ctx.ws, "ask", "list files in ."],
        env=ctx.env(port),
        capture_output=True,
        timeout=30,
    )
    assert r.returncode == 0, f"exit {r.returncode}: {r.stderr.decode()}"
    assert b"MOCK-OK" in r.stdout, r.stdout
    assert "⏺ list_files" in r.stderr.decode(), r.stderr
    sdir, doc = ctx.newest_doc()
    assert doc is not None
    # 0053: foreground turns now record status/result like -B turns
    assert doc["status"] == "done" and doc["exit_code"] == 0, doc
    assert "MOCK-OK" in doc["result"]["output"], doc["result"]
    # the runner exited: lock free, pid recorded but process gone
    assert os.path.exists(os.path.join(sdir, "pid")), "runner never wrote pid"
    print("ok: foreground streams through the runner and finalizes")


def test_foreground_json_shape(ctx, port):
    r = subprocess.run(
        [TNY, "--cwd", ctx.ws, "ask", "--json", "list files in ."],
        env=ctx.env(port),
        capture_output=True,
        timeout=30,
    )
    assert r.returncode == 0, r.stderr.decode()
    fg = json.loads(r.stdout)
    assert "MOCK-OK" in fg["output"], fg
    assert [t["name"] for t in fg["tool_calls"]] == ["list_files", "glob_files"], fg
    _, doc = ctx.newest_doc()
    assert {k: v for k, v in doc["result"].items() if k != "session_id"} == {
        k: v for k, v in fg.items() if k != "session_id"
    }, "stored result != foreground --json"
    print("ok: --json bytes unchanged and mirrored into the session")


def test_client_sigkill_mid_turn_survives(ctx, slow_port, fast_port):
    before = set(ctx.sdirs())
    p = subprocess.Popen(
        [TNY, "--cwd", ctx.ws, "ask", "slow one"],
        env=ctx.env(slow_port),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    sdir = poll(lambda: next(iter(set(ctx.sdirs()) - before), None), 10, "session dir")
    # kill the *client* the moment the runner holds the turn
    poll(lambda: os.path.exists(os.path.join(sdir, "pid")), 10, "runner pid file")
    p.kill()
    p.wait()
    doc = poll(
        lambda: (lambda d: d if d.get("status") not in (None, "running") else None)(
            json.load(open(os.path.join(sdir, "session.json")))
            if os.path.exists(os.path.join(sdir, "session.json"))
            else {}
        ),
        20,
        "runner finalize after client SIGKILL",
    )
    assert doc["status"] == "done", doc
    assert "MOCK-OK" in doc["result"]["output"], doc["result"]
    sid = os.path.basename(sdir)
    # and the surviving session resumes normally
    r = subprocess.run(
        [TNY, "--cwd", ctx.ws, "ask", "--resume", sid, "list files in ."],
        env=ctx.env(fast_port),
        capture_output=True,
        timeout=30,
    )
    assert r.returncode == 0, r.stderr.decode()
    assert b"MOCK-OK" in r.stdout, r.stdout
    print("ok: client SIGKILL mid-turn — the agent finished and resumed")


def test_tui_sigkill_mid_turn_survives(ctx, slow_port):
    before = set(ctx.sdirs())
    p = subprocess.Popen(
        [TNY, "--cwd", ctx.ws],
        env=ctx.env(slow_port),
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    p.stdin.write(b"go slowly\n")
    p.stdin.flush()
    sdir = poll(
        lambda: next(iter(set(ctx.sdirs()) - before), None), 15, "TUI session dir"
    )
    poll(lambda: os.path.exists(os.path.join(sdir, "pid")), 15, "TUI runner pid")
    p.kill()
    p.wait()
    doc = poll(
        lambda: (lambda d: d if d.get("status") not in (None, "running") else None)(
            json.load(open(os.path.join(sdir, "session.json")))
            if os.path.exists(os.path.join(sdir, "session.json"))
            else {}
        ),
        20,
        "runner finalize after TUI SIGKILL",
    )
    assert doc["status"] == "done", doc
    assert "MOCK-OK" in doc["result"]["output"], doc["result"]
    print("ok: TUI SIGKILL mid-turn — the agent finished into the session")


def test_attach_streams_live_turn(ctx, slow_port):
    r = subprocess.run(
        [TNY, "--cwd", ctx.ws, "ask", "-B", "slow attach target"],
        env=ctx.env(slow_port),
        capture_output=True,
        timeout=15,
    )
    assert r.returncode == 0, r.stderr.decode()
    sid = r.stdout.decode().strip()
    att = subprocess.run(
        [TNY, "--cwd", ctx.ws, "session", "attach", sid],
        env=ctx.env(slow_port),
        capture_output=True,
        timeout=30,
    )
    out, err = att.stdout.decode(), att.stderr.decode()
    assert f"attached to session {sid}" in err, err
    assert "MOCK-OK" in out, (out, err)
    assert "turn finished (exit 0)" in err, err
    assert att.returncode == 0, att.returncode
    # attach to a finished session reports cleanly
    att2 = subprocess.run(
        [TNY, "--cwd", ctx.ws, "session", "attach", sid],
        env=ctx.env(slow_port),
        capture_output=True,
        timeout=15,
    )
    assert att2.returncode == 1, att2
    assert "no live turn" in att2.stderr.decode(), att2.stderr
    print("ok: session attach streamed the live -B turn to its end")


def test_escape_hatch_runs_in_process(ctx, port):
    before = set(ctx.sdirs())
    r = subprocess.run(
        [TNY, "--cwd", ctx.ws, "ask", "list files in ."],
        env=ctx.env(port, TNY_ISOLATE="0"),
        capture_output=True,
        timeout=30,
    )
    assert r.returncode == 0, r.stderr.decode()
    assert b"MOCK-OK" in r.stdout, r.stdout
    fresh = set(ctx.sdirs()) - before
    assert len(fresh) == 1, fresh
    sdir = fresh.pop()
    # in-process: no runner artifacts
    assert not os.path.exists(os.path.join(sdir, "pid")), "unexpected runner pid"
    assert not os.path.exists(os.path.join(sdir, "sock")), "unexpected runner sock"
    print("ok: TNY_ISOLATE=0 keeps the in-process path")


def main():
    if os.environ.get("TNY_TEST_EXPECT_WASM") == "1":
        # decision 10: no fork in the browser — isolation is absent by
        # design and the in-process paths are covered by the other suites.
        print("test_isolation: wasm build — isolation intentionally absent")
        return
    fast, fport = start_mock()
    slow_a = slow_b = slow_c = None
    try:
        with tempfile.TemporaryDirectory() as home:
            ctx = Ctx(home)
            test_foreground_streams_and_finishes(ctx, fport)
            test_foreground_json_shape(ctx, fport)
            # one slow mock per hanging scenario: its HTTPServer is
            # single-threaded and a sleeping handler blocks later requests
            slow_a, aport = start_mock(MOCK_SLOW_MS="4000")
            test_client_sigkill_mid_turn_survives(ctx, aport, fport)
            slow_b, bport = start_mock(MOCK_SLOW_MS="4000")
            test_tui_sigkill_mid_turn_survives(ctx, bport)
            slow_c, cport = start_mock(MOCK_SLOW_MS="3000")
            test_attach_streams_live_turn(ctx, cport)
            test_escape_hatch_runs_in_process(ctx, fport)
    finally:
        for m in (fast, slow_a, slow_b, slow_c):
            if m is not None:
                m.terminate()
    print("test_isolation: all assertions passed")


if __name__ == "__main__":
    main()
