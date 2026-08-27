#!/usr/bin/env python3
"""bench_ttft.py — before/after latency benchmarks for tny's three
time-to-first-token changes, against the repo's scripted codex mock.

Benches (all fresh HOME per iteration, medians over --iters):
  tui        pty-driven: Enter -> first streamed output, with the mock's
             thread/start delayed by --rpc-delay ms. Measures what #3
             (create_or_resume on the pre-warm thread) removes from Enter.
  ask-stdin  wall time of (sleep --rpc-delay; echo prompt) | tny ask with the
             mock's initialize delayed by --rpc-delay ms. Measures what #4
             (stdin/connect overlap) hides.
  ask-spawn  wall time of tny ask spawning a stub codex (TNY_CODEX_BIN).
  ask-attach wall time of tny ask attaching via ~/.tny/codex-host.json.
             ask-spawn minus ask-attach is what #5 saves per one-shot.

Usage: bench_ttft.py --tny BIN --repo ROOT --bench tui|ask-stdin|ask-spawn|ask-attach
                     [--iters N] [--rpc-delay MS] [--label TEXT]
Prints one line: LABEL BENCH median=Xms min=Xms runs=[..]
"""

import argparse
import json
import os
import pty
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def start_mock(repo, tmp, env_extra):
    env = dict(os.environ)
    env.pop("MOCK_TOKEN", None)
    env["MOCK_CONNECTIONS"] = "9"
    env.update(env_extra)
    out = open(os.path.join(tmp, "mock.out"), "w+")
    p = subprocess.Popen(
        [sys.executable, os.path.join(repo, "tests/integration/mock_codex_ws.py"), "0"],
        stdout=out,
        stderr=subprocess.DEVNULL,
        env=env,
    )
    port = None
    deadline = time.time() + 10
    while time.time() < deadline:
        out.flush()
        with open(out.name) as f:
            m = re.search(r"ready on (\d+)", f.read())
        if m:
            port = int(m.group(1))
            break
        time.sleep(0.05)
    if not port:
        p.kill()
        raise RuntimeError("mock never became ready")
    return p, port


def base_env(home):
    env = dict(os.environ)
    env["HOME"] = home
    for k in ("OPENAI_API_KEY", "CODEX_REMOTE_TOKEN", "TNY_CODEX_WS", "CURSOR_API_KEY"):
        env.pop(k, None)
    return env


def bench_tui(tny, repo, tmp, delay_ms):
    mock, port = start_mock(repo, tmp, {"MOCK_THREAD_DELAY_MS": str(delay_ms)})
    home = os.path.join(tmp, "home")
    os.makedirs(home)
    ws = os.path.join(tmp, "ws")
    os.makedirs(ws)
    master, slave = pty.openpty()
    p = subprocess.Popen(
        [
            tny,
            "--cwd",
            ws,
            "--backend",
            "codex",
            "--codex-ws",
            "ws://127.0.0.1:%d" % port,
        ],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=base_env(home),
        close_fds=True,
    )
    os.close(slave)
    raw = b""

    def read_until(needle, timeout):
        nonlocal raw
        deadline = time.time() + timeout
        while needle not in raw:
            if time.time() > deadline:
                raise RuntimeError(
                    "timeout waiting for %r; got %r" % (needle, raw[-400:])
                )
            r, _, _ = select.select([master], [], [], 0.05)
            if r:
                try:
                    raw += os.read(master, 65536)
                except OSError:
                    break
        return raw

    try:
        read_until(b"/help for commands", 15)
        # let the pre-warm (connect [+ thread/start on the new binary]) finish
        time.sleep((delay_ms / 1000.0) + 1.0)
        os.write(master, b"hi\r")
        t0 = time.time()
        raw = b""  # only post-Enter bytes: the streamed marker is interleaved
        # with block redraws, but the tool line lands as one contiguous write
        read_until(b"commandExecution", 20)
        dt = (time.time() - t0) * 1000.0
    finally:
        try:
            os.write(master, b"\x03\x03")
        except OSError:
            pass
        try:
            p.wait(timeout=3)
        except subprocess.TimeoutExpired:
            p.kill()
        os.close(master)
        mock.kill()
    return dt


def bench_ask_stdin(tny, repo, tmp, delay_ms):
    mock, port = start_mock(repo, tmp, {"MOCK_INIT_DELAY_MS": str(delay_ms)})
    home = os.path.join(tmp, "home")
    os.makedirs(home)
    ws = os.path.join(tmp, "ws")
    os.makedirs(ws)
    t0 = time.time()
    prod = subprocess.Popen(
        [
            sys.executable,
            "-c",
            "import time,sys; time.sleep(%f); print('hi')" % (delay_ms / 1000.0),
        ],
        stdout=subprocess.PIPE,
    )
    p = subprocess.Popen(
        [
            tny,
            "--cwd",
            ws,
            "--backend",
            "codex",
            "--codex-ws",
            "ws://127.0.0.1:%d" % port,
            "ask",
            "--yolo",
        ],
        stdin=prod.stdout,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=base_env(home),
    )
    prod.stdout.close()
    rc = p.wait(timeout=30)
    dt = (time.time() - t0) * 1000.0
    mock.kill()
    if rc != 0:
        raise RuntimeError("ask exited %d" % rc)
    return dt


def make_stub(tmp, repo):
    """A stand-in `codex` that serves app-server itself: parses --listen,
    execs the repo mock on that port. Ready when the port accepts."""
    stub = os.path.join(tmp, "codex-stub")
    with open(stub, "w") as f:
        f.write(
            """#!/bin/sh
# args: app-server --listen ws://127.0.0.1:PORT
port=$(printf '%%s' "$3" | sed 's/.*://')
export HOME=%s
exec %s %s "$port"
"""
            % (
                os.environ.get("HOME", "/tmp"),
                sys.executable,
                os.path.join(repo, "tests/integration/mock_codex_ws.py"),
            )
        )
    os.chmod(stub, 0o755)
    return stub


def bench_ask_spawn(tny, repo, tmp, delay_ms):
    home = os.path.join(tmp, "home")
    os.makedirs(home)
    ws = os.path.join(tmp, "ws")
    os.makedirs(ws)
    env = base_env(home)
    env["TNY_CODEX_BIN"] = make_stub(tmp, repo)
    env["MOCK_CONNECTIONS"] = "1"
    t0 = time.time()
    rc = subprocess.run(
        [tny, "--cwd", ws, "--backend", "codex", "ask", "--yolo", "hi"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=env,
        timeout=30,
    ).returncode
    dt = (time.time() - t0) * 1000.0
    if rc != 0:
        raise RuntimeError("ask (spawn) exited %d" % rc)
    return dt


def bench_ask_attach(tny, repo, tmp, delay_ms):
    mock, port = start_mock(repo, tmp, {})
    home = os.path.join(tmp, "home")
    os.makedirs(home)
    ws = os.path.join(tmp, "ws")
    os.makedirs(ws)
    tnydir = os.path.join(home, ".tny")
    os.makedirs(tnydir)
    with open(os.path.join(tnydir, "codex-host.json"), "w") as f:
        json.dump({"ws": "ws://127.0.0.1:%d" % port, "pid": mock.pid}, f)
    t0 = time.time()
    rc = subprocess.run(
        [tny, "--cwd", ws, "--backend", "codex", "ask", "--yolo", "hi"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=base_env(home),
        timeout=30,
    ).returncode
    dt = (time.time() - t0) * 1000.0
    mock.kill()
    if rc != 0:
        raise RuntimeError("ask (attach) exited %d" % rc)
    return dt


BENCHES = {
    "tui": bench_tui,
    "ask-stdin": bench_ask_stdin,
    "ask-spawn": bench_ask_spawn,
    "ask-attach": bench_ask_attach,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tny", required=True)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--bench", required=True, choices=sorted(BENCHES))
    ap.add_argument("--iters", type=int, default=5)
    ap.add_argument("--rpc-delay", type=int, default=400)
    ap.add_argument("--label", default="tny")
    a = ap.parse_args()

    runs = []
    for _i in range(a.iters):
        tmp = tempfile.mkdtemp(prefix="tnybench.")
        try:
            runs.append(BENCHES[a.bench](a.tny, a.repo, tmp, a.rpc_delay))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    runs.sort()
    med = runs[len(runs) // 2]
    print(
        "%-10s %-10s median=%7.1fms  min=%7.1fms  runs=%s"
        % (
            a.label,
            a.bench,
            med,
            runs[0],
            "[" + ", ".join("%.0f" % r for r in runs) + "]",
        )
    )


if __name__ == "__main__":
    main()
