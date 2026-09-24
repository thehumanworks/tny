#!/usr/bin/env python3
"""Replay large local tool outputs through the mock Responses provider."""

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MOCK = ROOT / "tests/integration/mock_openai.py"


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def measure(binary, scenario, enabled, root):
    home = root / f"{scenario}-{'on' if enabled else 'off'}"
    ws = home / "ws"
    ws.mkdir(parents=True)
    if scenario == "log":
        (ws / "fixture.txt").write_text(
            "".join(f"log {i:05d}: ordinary output\n" for i in range(40000))
        )
        tool, arguments = "terminal", {"command": "cat fixture.txt"}
    elif scenario == "failure":
        (ws / "fixture.txt").write_text(
            "".join(f"test {i:05d}: detail\n" for i in range(5000))
            + "FAILED: final assertion\n"
        )
        tool, arguments = "terminal", {"command": "cat fixture.txt; exit 1"}
    else:
        (ws / "fixture.txt").write_text(
            "".join(f"source {i:05d}: a line of content\n" for i in range(12000))
        )
        tool, arguments = "read_file", {"path": "fixture.txt"}
    port = free_port()
    log = home / "sizes.jsonl"
    mock_env = dict(
        os.environ,
        MOCK_EXPECT_WIRE="responses",
        MOCK_CUSTOM_TOOL=tool,
        MOCK_CUSTOM_ARGUMENTS=json.dumps(arguments),
        MOCK_REQUEST_SIZE_LOG=str(log),
    )
    mock = subprocess.Popen(
        [sys.executable, str(MOCK), str(port)],
        env=mock_env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        assert mock.stdout and b"ready" in mock.stdout.readline()
        env = dict(
            os.environ,
            HOME=str(home),
            OPENAI_BASE_URL=f"http://127.0.0.1:{port}/v1",
            OPENAI_API_KEY="synthetic-test-key",
            TNY_TOOLS="all",
            TNY_SELF_IMPROVE="0",
            TNY_EXP_SPILL="1" if enabled else "0",
        )
        run = subprocess.run(
            [str(binary), "--cwd", str(ws), "ask", "--json", "measure output"],
            env=env,
            capture_output=True,
            timeout=120,
            check=False,
        )
        if run.returncode:
            raise RuntimeError(
                f"{scenario} enabled={enabled}: {run.returncode}: "
                f"{run.stderr.decode(errors='replace')}"
            )
        rows = [json.loads(line) for line in log.read_text().splitlines()]
        if len(rows) != 2:
            raise RuntimeError(f"expected 2 requests, got {rows!r}")
        return rows
    finally:
        mock.terminate()
        mock.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=ROOT / "build/tny")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        results = {
            scenario: {
                arm: measure(args.binary.resolve(), scenario, arm == "on", root)
                for arm in ("off", "on")
            }
            for scenario in ("log", "failure", "read_file")
        }
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
