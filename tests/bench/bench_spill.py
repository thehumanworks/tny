#!/usr/bin/env python3
"""Replay large local tool outputs through the mock Responses provider."""

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MOCK = ROOT / "tests/integration/mock_openai.py"
SCENARIOS = ("log", "failure", "read_file", "straddle", "progress", "minified")
RANDOM_HANDLE = re.compile(rb'Full output stored as handle \\"[0-9a-f]{16}\\"')


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def measure(binary, scenario, enabled, root):
    home = root / f"{scenario}-{'on' if enabled else 'off'}"
    if home.exists():
        shutil.rmtree(home)
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
    elif scenario == "straddle":
        tool, arguments = (
            "terminal",
            {
                "command": "python3 -c 'import os,time; "
                '[(os.write(1,b"x"*1000),time.sleep(.001)) for _ in range(530)]\''
            },
        )
    elif scenario == "progress":
        tool, arguments = (
            "terminal",
            {
                "command": "python3 -c 'import os; "
                'os.write(1,b".\\r"*32000+b"ERROR: build failed at step 42")\''
            },
        )
    elif scenario == "minified":
        (ws / "fixture.txt").write_text("x" * 40000)
        tool, arguments = "read_file", {"path": "fixture.txt"}
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
        MOCK_REQUEST_BODY_DIR=str(home / "requests"),
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
            (
                (key, value)
                for key, value in os.environ.items()
                if not key.startswith("TNY_")
            ),
            HOME=str(home),
            OPENAI_BASE_URL=f"http://127.0.0.1:{port}/v1",
            OPENAI_API_KEY="synthetic-test-key",
            TNY_TOOLS="all",
            TNY_SELF_IMPROVE="0",
            TNY_EXP_SPILL="1" if enabled else "0",
        )
        for name in (
            "TNY_EXP_SPILL_BYTES",
            "TNY_EXP_SPILL_HEAD_PCT",
            "TNY_EXP_SPILL_LINE_BYTES",
            "TNY_EXP_READ_BYTES",
            "TNY_EXP_READ_LINENO",
        ):
            if name in os.environ:
                env[name] = os.environ[name]
        env["PATH"] = f"{Path(sys.executable).parent}:{env.get('PATH', '')}"
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


def replay(binary, root):
    return {
        scenario: {
            arm: measure(binary, scenario, arm == "on", root) for arm in ("off", "on")
        }
        for scenario in SCENARIOS
    }


def off_artifacts(root):
    artifacts = {}
    for scenario in SCENARIOS:
        home = root / f"{scenario}-off"
        requests = tuple(
            RANDOM_HANDLE.sub(
                b'Full output stored as handle \\"0000000000000000\\"',
                (home / "requests" / f"{index:04d}.json").read_bytes(),
            )
            for index in (0, 1)
        )
        saved = tuple(sorted(path.read_bytes() for path in home.rglob("results/*.txt")))
        artifacts[scenario] = (requests, saved)
    return artifacts


def verify_outputs(root):
    for scenario, fragment in (
        ("progress", "ERROR: build failed at step 42"),
        ("minified", "continue with offset=-16384"),
    ):
        request = json.loads((root / f"{scenario}-on/requests/0001.json").read_text())
        outputs = [
            item.get("output", "")
            for item in request.get("input", [])
            if item.get("type") == "function_call_output"
        ]
        if len(outputs) != 1 or fragment not in outputs[0]:
            raise AssertionError(f"{scenario}: expected preview fragment is absent")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=ROOT / "build/tny")
    parser.add_argument(
        "--baseline-binary",
        type=Path,
        help="compare complete flag-off requests and saved results",
    )
    parser.add_argument(
        "--root", type=Path, help="keep request bodies under this directory"
    )
    args = parser.parse_args()

    def run(root):
        baseline = None
        if args.baseline_binary:
            replay(args.baseline_binary.resolve(), root)
            baseline = off_artifacts(root)
        results = replay(args.binary.resolve(), root)
        verify_outputs(root)
        if baseline is not None:
            current = off_artifacts(root)
            for scenario in SCENARIOS:
                if baseline[scenario] != current[scenario]:
                    raise AssertionError(
                        f"flag-off body or saved result differs: {scenario}"
                    )
            results["flag_off_full_body_and_saved_results_match_baseline"] = True
        print(json.dumps(results, indent=2))

    if args.root:
        args.root.mkdir(parents=True, exist_ok=True)
        run(args.root.resolve())
    else:
        with tempfile.TemporaryDirectory() as temporary:
            run(Path(temporary))


if __name__ == "__main__":
    main()
