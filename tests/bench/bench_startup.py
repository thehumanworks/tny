#!/usr/bin/env python3
"""Compare fresh-process startup, including the first fully painted PTY prompt.

No backend is started: each sample uses an empty HOME/workspace, the lazy
OpenAI provider, no MCP settings, and no submitted turn. This is not TTFT.
Run with --baseline /absolute/tny --candidate /absolute/tny --output report.json.
The defaults satisfy issues #137-#139 (102 CLI samples in three alternating
batches, plus 20 PTY launches per artifact). Raw samples and artifact hashes
are always written, including on a failed performance gate.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import select
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

PROMPT_END = b"\r\x1b[2C\x1b[?7h"
MODES = ("version", "help", "first-prompt")


def summary(samples: list[float]) -> dict[str, float | int]:
    """Use the nearest-rank p95; retain raw samples separately in the report."""
    if not samples or any(not math.isfinite(x) or x < 0 for x in samples):
        raise ValueError("latency samples must be nonempty, finite and nonnegative")
    ordered = sorted(samples)
    return {
        "count": len(samples),
        "median_ms": statistics.median(samples),
        "p95_ms": ordered[math.ceil(len(samples) * 0.95) - 1],
        "min_ms": ordered[0],
        "max_ms": ordered[-1],
    }


def compare(mode: str, baseline: list[float], candidate: list[float]) -> dict[str, Any]:
    if mode not in MODES:
        raise ValueError(f"unknown benchmark mode: {mode}")
    before, after = summary(baseline), summary(candidate)
    absolute = 10.0 if mode == "first-prompt" else 5.0
    floor = 0.5 if mode == "first-prompt" else 0.25
    allowance = max(floor, float(before["median_ms"]) * 0.1)
    delta = float(after["median_ms"]) - float(before["median_ms"])
    return {
        "baseline": before,
        "candidate": after,
        "delta_median_ms": delta,
        "absolute_limit_ms": absolute,
        "allowed_added_ms": allowance,
        "baseline_below_absolute_limit": float(before["median_ms"]) < absolute,
        "passed": float(after["median_ms"]) < absolute and delta <= allowance,
    }


def batch_sizes(samples: int, batches: int) -> list[int]:
    if samples < 1 or batches < 1 or batches > samples:
        raise ValueError("require 1 <= batches <= samples")
    quotient, remainder = divmod(samples, batches)
    return [quotient + (i < remainder) for i in range(batches)]


def isolated_env(home: Path) -> dict[str, str]:
    # Whitelisting excludes host credentials, provider overrides, live session
    # roots and user-specific loader/injection variables from the measurement.
    env = {
        k: os.environ[k] for k in ("PATH", "SYSTEMROOT", "WINDIR") if k in os.environ
    }
    env.update(
        HOME=str(home),
        XDG_CONFIG_HOME=str(home / "config"),
        XDG_CACHE_HOME=str(home / "cache"),
        XDG_STATE_HOME=str(home / "state"),
        TMPDIR=str(home),
        TERM="xterm-256color",
        LC_ALL="C",
        NO_COLOR="1",
        TNY_ISOLATE="0",
        OPENAI_API_KEY="startup-fixture-not-a-secret",
        OPENAI_BASE_URL="http://127.0.0.1:1/v1",
    )
    return env


def prompt_ready(raw: bytes) -> bool:
    """Do not mistake raw-mode setup or the banner for a painted composer."""
    banner = raw.find(b"/help for commands")
    block = raw.find(b"\x1b[?7l", max(banner, 0))
    prompt = raw.find(b"> ", max(block, 0))
    end = raw.find(PROMPT_END, max(prompt, 0))
    return 0 <= banner < block < prompt < end


def pty_sample(binary: Path, home: Path, workspace: Path, timeout: float) -> float:
    # Imports remain local so arithmetic/contract self-tests also run on MSYS
    # installations without the full POSIX PTY Python modules.
    import fcntl
    import pty
    import struct
    import termios

    master, slave = pty.openpty()
    process = None
    try:
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        started = time.perf_counter_ns()
        process = subprocess.Popen(
            [str(binary), "--cwd", str(workspace), "--provider", "openai", "--yolo"],
            stdin=slave,
            stdout=slave,
            stderr=slave,
            cwd=workspace,
            env=isolated_env(home),
            close_fds=True,
            start_new_session=True,
        )
        os.close(slave)
        slave = -1
        raw = bytearray()
        deadline = time.monotonic() + timeout
        while not prompt_ready(raw):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f"first prompt timeout; trailing output: {bytes(raw[-320:])!r}"
                )
            ready, _, _ = select.select([master], [], [], remaining)
            if not ready:
                continue
            try:
                chunk = os.read(master, 65536)
            except OSError as exc:
                raise RuntimeError("PTY closed before the first prompt") from exc
            if not chunk:
                raise RuntimeError("PTY EOF before the first prompt")
            raw.extend(chunk)
            if len(raw) > 1024 * 1024:
                raise RuntimeError("unexpectedly large pre-prompt output")
        elapsed = (time.perf_counter_ns() - started) / 1_000_000
        # No turn was submitted. Ctrl-D exits the empty composer. Continue
        # draining the PTY: termios restoration may wait for output consumption.
        os.write(master, b"\x04")
        deadline = time.monotonic() + timeout
        while process.poll() is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("benchmark TUI did not exit after Ctrl-D")
            ready, _, _ = select.select([master], [], [], min(remaining, 0.05))
            if ready:
                try:
                    os.read(master, 65536)
                except OSError:
                    # macOS PTYs report EIO once the slave closes. Still reap
                    # and check the exact child rather than treating EOF as success.
                    pass
        rc = process.wait()
        if rc != 0:
            raise RuntimeError(f"benchmark TUI exited with status {rc}")
        return elapsed
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if slave >= 0:
            os.close(slave)
        os.close(master)


def measure(binary: Path, mode: str, timeout: float) -> float:
    with tempfile.TemporaryDirectory(prefix="tny-startup-") as temp:
        home = Path(temp) / "home"
        workspace = Path(temp) / "workspace"
        home.mkdir()
        workspace.mkdir()
        if mode == "first-prompt":
            return pty_sample(binary, home, workspace, timeout)
        command = [str(binary)] + (
            ["--version"] if mode == "version" else ["ask", "--help"]
        )
        started = time.perf_counter_ns()
        result = subprocess.run(
            command,
            cwd=workspace,
            env=isolated_env(home),
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        elapsed = (time.perf_counter_ns() - started) / 1_000_000
        if result.returncode != 0 or result.stderr or not result.stdout:
            raise RuntimeError(
                f"{mode} failed: status={result.returncode}, stderr={result.stderr[:320]!r}"
            )
        return elapsed


def artifact(binary: Path) -> dict[str, Any]:
    data = binary.read_bytes()
    return {
        "path": str(binary),
        "sha256": hashlib.sha256(data).hexdigest(),
        "bytes": len(data),
    }


def run(args: argparse.Namespace) -> dict[str, Any]:
    binaries = {
        name: getattr(args, name).resolve(strict=True)
        for name in ("baseline", "candidate")
    }
    before = {name: artifact(binary) for name, binary in binaries.items()}
    report: dict[str, Any] = {
        "schema": 1,
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python": sys.version,
        },
        "artifacts": before,
        "configuration": {
            "cli_samples": args.samples,
            "pty_samples": args.pty_samples,
            "batches": args.batches,
            "timeout_seconds": args.timeout,
            "fresh_home_and_workspace": True,
            "provider": "openai (lazy, no submitted turn)",
            "first_prompt_marker_hex": PROMPT_END.hex(),
            "clock": "perf_counter_ns; process launch through completed output/paint",
        },
        "samples": {},
        "comparisons": {},
        "passed": False,
    }
    try:
        for mode in MODES:
            size = args.pty_samples if mode == "first-prompt" else args.samples
            values: dict[str, list[float]] = {name: [] for name in binaries}
            samples: list[dict[str, Any]] = []
            report["samples"][mode] = samples
            for batch, count in enumerate(batch_sizes(size, args.batches)):
                # Alternate which artifact goes first to expose drift, while
                # keeping the required paired batches reproducible.
                order = (
                    ("baseline", "candidate")
                    if batch % 2 == 0
                    else ("candidate", "baseline")
                )
                for name in order:
                    for iteration in range(count):
                        elapsed = measure(binaries[name], mode, args.timeout)
                        values[name].append(elapsed)
                        samples.append(
                            {
                                "artifact": name,
                                "batch": batch,
                                "iteration": iteration,
                                "ms": elapsed,
                            }
                        )
            report["comparisons"][mode] = compare(
                mode, values["baseline"], values["candidate"]
            )
        if any(artifact(binary) != before[name] for name, binary in binaries.items()):
            raise RuntimeError("a measured artifact changed during the benchmark")
        report["passed"] = all(
            item["passed"] for item in report["comparisons"].values()
        )
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=102)
    parser.add_argument("--pty-samples", type=int, default=20)
    parser.add_argument("--batches", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=5.0)
    args = parser.parse_args()
    if args.samples < 100 or args.pty_samples < 20 or args.batches != 3:
        parser.error(
            "the contract requires >=100 CLI samples, >=20 PTY samples, and exactly 3 batches"
        )
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    try:
        report = run(args)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for mode, result in report["comparisons"].items():
        print(
            f"{mode}: baseline={result['baseline']['median_ms']:.3f}ms "
            f"candidate={result['candidate']['median_ms']:.3f}ms "
            f"delta={result['delta_median_ms']:+.3f}ms "
            f"{'PASS' if result['passed'] else 'FAIL'}"
        )
    if "error" in report:
        print(f"benchmark error: {report['error']}", file=sys.stderr)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
