"""Measure paired native startup and exact release artifact closure, without a model."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inspect(path: Path) -> dict:
    return {
        "path": str(path),
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
        "version": subprocess.check_output([str(path), "--version"], text=True).strip(),
        "dependencies": subprocess.check_output(["ldd", str(path)], text=True),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--rounds", type=int, default=50)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.rounds < 10 or args.rounds > 1000:
        parser.error("rounds must be 10..1000")
    paths = {"lua": args.baseline.resolve(), "python": args.candidate.resolve()}
    artifacts = {arm: inspect(path) for arm, path in paths.items()}
    if artifacts["lua"]["version"] != artifacts["python"]["version"]:
        parser.error("same-target comparison requires identical version labels")
    measures = {}
    load_start = os.getloadavg()
    for argv in (["--version"], ["ask", "--help"]):
        values = {arm: [] for arm in paths}
        for iteration in range(args.rounds + 5):
            order = ("lua", "python") if iteration % 2 == 0 else ("python", "lua")
            for arm in order:
                start = time.perf_counter_ns()
                result = subprocess.run(
                    [str(paths[arm]), *argv],
                    env={
                        "PATH": "/usr/bin:/bin",
                        "HOME": "/nonexistent",
                        "LANG": "C.UTF-8",
                    },
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=5,
                    check=True,
                )
                elapsed = (time.perf_counter_ns() - start) / 1000000
                if not result.stdout:
                    raise RuntimeError(f"{arm} produced no output for {argv}")
                if iteration >= 5:
                    values[arm].append(elapsed)
        measures[" ".join(argv)] = {
            arm: {
                "median_ms": statistics.median(times),
                "min_ms": min(times),
                "samples_ms": times,
            }
            for arm, times in values.items()
        }
    for arm, path in paths.items():
        if sha256(path) != artifacts[arm]["sha256"]:
            raise RuntimeError("artifact changed while measurements were running")
    inputs = [
        "src/core/code_runtime.c",
        "src/core/code_python.c",
        "src/core/code_policy.c",
        "src/util/code_sandbox.c",
        "scripts/cpython_runtime.sh",
        "src/core/code_runtime.h",
    ]
    report = {
        "scope": "Serial paired native startup after five warmup pairs; rotated order, warm page cache, shared host; no provider inference. Version label is a common comparison label, not a published version.",
        "source_revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "source_sha256": {name: sha256(ROOT / name) for name in inputs},
        "platform": platform.platform(),
        "load_average_start": load_start,
        "load_average_end": os.getloadavg(),
        "rounds": args.rounds,
        "binaries": artifacts,
        "startup": measures,
        "size_delta_bytes": artifacts["python"]["bytes"] - artifacts["lua"]["bytes"],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(".tmp")
    temporary.write_text(json.dumps(report, indent=2) + "\n")
    temporary.replace(args.output)
    print(
        json.dumps(
            {
                "bytes": {a: d["bytes"] for a, d in artifacts.items()},
                "median_ms": {
                    cmd: {a: d["median_ms"] for a, d in arms.items()}
                    for cmd, arms in measures.items()
                },
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
