#!/usr/bin/env python3
"""bench_ttft.py — before/after latency benchmarks for tny's
time-to-first-token work (docs/adr/0002, docs/adr/0004), against the repo's
strict mock OpenAI provider (tests/integration/mock_openai.py, which also
stands in for the codex profile's ChatGPT backend — docs/adr/0065).

Benches (all fresh HOME per iteration, medians over --iters):
  tui        pty-driven: Enter -> first streamed output, with the mock's
             first response delayed by --rpc-delay ms (MOCK_SLOW_MS).
             Measures what the pre-warm thread removes from Enter.
  ask-stdin  wall time of (sleep --rpc-delay; echo prompt) | tny ask with
             the mock's first response delayed by --rpc-delay ms. Measures
             what the stdin/connect overlap hides.

Usage: bench_ttft.py --tny BIN --repo ROOT --bench tui|ask-stdin
                     [--iters N] [--rpc-delay MS] [--label TEXT]
Prints one line: LABEL BENCH median=Xms min=Xms runs=[..]
"""

import argparse
import os
import pty
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
    env["MOCK_EXPECT_WIRE"] = "responses"
    env.update(env_extra)
    port = free_port()
    out = open(os.path.join(tmp, "mock.out"), "w+")
    p = subprocess.Popen(
        [
            sys.executable,
            os.path.join(repo, "tests/integration/mock_openai.py"),
            str(port),
        ],
        stdout=out,
        stderr=subprocess.DEVNULL,
        env=env,
    )
    deadline = time.time() + 10
    while time.time() < deadline:
        out.flush()
        with open(out.name) as f:
            if "ready" in f.read():
                return p, port
        time.sleep(0.05)
    p.kill()
    raise RuntimeError("mock never became ready")


def base_env(home, port):
    env = dict(os.environ)
    for k in list(env):
        if k.endswith("_API_KEY") or k.endswith("_BASE_URL"):
            env.pop(k)
    env["HOME"] = home
    env["OPENAI_API_KEY"] = "bench"
    env["OPENAI_BASE_URL"] = "http://127.0.0.1:%d/v1" % port
    return env


def bench_tui(tny, repo, tmp, delay_ms):
    mock, port = start_mock(repo, tmp, {"MOCK_SLOW_MS": str(delay_ms)})
    home = os.path.join(tmp, "home")
    os.makedirs(home)
    ws = os.path.join(tmp, "ws")
    os.makedirs(ws)
    master, slave = pty.openpty()
    p = subprocess.Popen(
        [tny, "--cwd", ws, "--provider", "openai", "--yolo"],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=base_env(home, port),
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
        time.sleep(1.0)  # let the pre-warm finish
        os.write(master, b"hi\r")
        t0 = time.time()
        raw = b""  # only post-Enter bytes
        read_until(b"list_files", 20 + delay_ms / 1000.0)
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
    mock, port = start_mock(repo, tmp, {"MOCK_SLOW_MS": str(delay_ms)})
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
        [tny, "--cwd", ws, "--provider", "openai", "ask", "--yolo", "--no-save"],
        stdin=prod.stdout,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=base_env(home, port),
    )
    prod.stdout.close()
    rc = p.wait(timeout=60)
    dt = (time.time() - t0) * 1000.0
    mock.kill()
    if rc != 0:
        raise RuntimeError("ask exited %d" % rc)
    return dt


BENCHES = {
    "tui": bench_tui,
    "ask-stdin": bench_ask_stdin,
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
