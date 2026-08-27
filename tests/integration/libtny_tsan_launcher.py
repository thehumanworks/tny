#!/usr/bin/env python3
"""Launch strict loopback providers around the native libtny TSan host."""

from __future__ import annotations

import os
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MOCK = ROOT / "tests/integration/mock_openai.py"


def bounded(name: str, default: int, minimum: int, maximum: int) -> int:
    raw = os.environ.get(name, str(default))
    if not raw.isdecimal():
        raise ValueError(f"{name} must be an integer from {minimum} to {maximum}")
    value = int(raw)
    if not minimum <= value <= maximum:
        raise ValueError(f"{name} must be an integer from {minimum} to {maximum}")
    return value


def free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def start_mock() -> tuple[subprocess.Popen[bytes], str]:
    port = free_port()
    process = subprocess.Popen(
        [sys.executable, os.fspath(MOCK), str(port)],
        cwd=ROOT,
        env=dict(
            os.environ,
            MOCK_EXPECT_WIRE="responses",
            MOCK_SLOW_MS="1000",
            MOCK_CONNECTION_CLOSE="1",
            MOCK_CHUNK_WIDTH="1048576",
        ),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    assert process.stdout is not None
    ready = process.stdout.readline()
    if b"ready" not in ready:
        raise RuntimeError(
            f"TSan strict mock failed before ready (rc={process.poll()})"
        )
    return process, f"http://127.0.0.1:{port}/v1"


def stop_mock(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
    if process.stdout is not None:
        process.stdout.close()


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} TSAN_HOST", file=sys.stderr)
        return 2
    host = Path(sys.argv[1]).resolve(strict=True)
    workers = bounded("TNY_TSAN_WORKERS", 4, 2, 16)
    cycles = bounded("TNY_TSAN_CYCLES", 12, 1, 1000)
    mocks: list[subprocess.Popen[bytes]] = []
    try:
        urls: list[str] = []
        for _ in range(workers):
            process, url = start_mock()
            mocks.append(process)
            urls.append(url)
        with tempfile.TemporaryDirectory(prefix="tny-tsan-") as temporary:
            root = Path(temporary)
            for cycle in range(cycles):
                cycle_root = root / f"cycle-{cycle}"
                for worker in range(workers):
                    (cycle_root / f"workspace-{worker}").mkdir(parents=True)
                    (cycle_root / f"state-{worker}").mkdir()
            options = os.environ.get("TSAN_OPTIONS", "")
            required = "halt_on_error=1:exitcode=66:history_size=7"
            environment = dict(
                os.environ,
                TNY_TSAN_CYCLES=str(cycles),
                TSAN_OPTIONS=f"{options}:{required}" if options else required,
            )
            completed = subprocess.run(
                [os.fspath(host), os.fspath(root), *urls],
                cwd=ROOT,
                env=environment,
                timeout=180,
                check=False,
            )
            return completed.returncode
    finally:
        for process in mocks:
            stop_mock(process)


if __name__ == "__main__":
    raise SystemExit(main())
