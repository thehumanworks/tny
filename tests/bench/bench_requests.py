#!/usr/bin/env python3
"""Compare native request admission using identical fixture bytes and flags.

Omit --candidate for baseline validation only. Builds must use fresh directories.
Each batch is a fresh process with isolated HOME/state and fixed-size sessions;
all warmup and measured per-request observations are retained in report.json.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import shlex
import statistics
import subprocess
import tempfile
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
HARNESS_FILES = ("bench_requests.c", "bench_requests.py", "bench_requests.mk")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checked(command: list[str], cwd: Path, env: dict[str, str], log: Path) -> str:
    result = subprocess.run(
        command, cwd=cwd, env=env, capture_output=True, text=True, timeout=900
    )
    with log.open("a", encoding="utf-8") as stream:
        stream.write(
            f"$ {shlex.join(command)}\n{result.stdout}{result.stderr}\nexit={result.returncode}\n"
        )
    if result.returncode or result.stderr:
        raise RuntimeError(f"command failed or emitted diagnostics; see {log}")
    return result.stdout


def isolated_env(home: Path) -> dict[str, str]:
    env = {key: os.environ[key] for key in ("PATH",) if key in os.environ}
    env.update(
        HOME=str(home),
        TMPDIR=str(home),
        XDG_CONFIG_HOME=str(home / "config"),
        XDG_STATE_HOME=str(home / "state"),
        XDG_CACHE_HOME=str(home / "cache"),
        LC_ALL="C",
        TZ="UTC",
        NO_COLOR="1",
    )
    return env


def source_manifest(root: Path, sources: list[str]) -> dict[str, str]:
    paths = {root / name for name in sources}
    paths.add(root / "Makefile")
    for directory in ("src", "include", "third_party"):
        paths.update(
            path
            for path in (root / directory).rglob("*")
            if path.suffix in (".h", ".hpp") and path.is_file()
        )
    return {str(path.relative_to(root)): digest(path) for path in sorted(paths)}


def build(root: Path, out: Path, args: argparse.Namespace, log: Path) -> dict[str, Any]:
    out.mkdir(parents=True, exist_ok=False)
    home = out / "build-home"
    home.mkdir()
    env = isolated_env(home)
    command = [
        args.make,
        "--no-print-directory",
        "-f",
        "Makefile",
        "-f",
        str(HERE / "bench_requests.mk"),
        f"BUILD={out}",
        f"REQUEST_HARNESS={HERE / 'bench_requests.c'}",
        "SANITIZE=0",
        f"CC={args.cc}",
        f"CXX={args.cxx}",
    ]
    config = dict(
        line.split("=", 1)
        for line in checked(
            [*command, "-s", "request-benchmark-config"], root, env, log
        ).splitlines()
    )
    sources = shlex.split(config["SOURCES"])
    manifest = source_manifest(root, sources)
    revision = checked(["git", "rev-parse", "HEAD"], root, env, log).strip()
    dirty = checked(["git", "diff", "--name-only", "HEAD"], root, env, log).splitlines()
    checked([*command, f"-j{args.jobs}", "request-benchmark"], root, env, log)
    if manifest != source_manifest(root, sources):
        raise RuntimeError("sources changed during compilation")
    binary = out / "bench-requests"
    compilers = {
        name: checked([*shlex.split(config[name]), "--version"], root, env, log)
        for name in ("CC", "CXX")
    }
    normalized = {
        key: value.replace(str(out), "<BUILD>")
        for key, value in config.items()
        if key != "SOURCES"
    }
    dependency_command = (
        ["otool", "-L", str(binary)]
        if platform.system() == "Darwin"
        else ["ldd", str(binary)]
    )
    return {
        "source_root": str(root),
        "revision": revision,
        "dirty_tracked_files": dirty,
        "source_manifest": manifest,
        "sources": sources,
        "configuration": config,
        "normalized_flags": normalized,
        "compiler_versions": compilers,
        "make_command": command,
        "build_log": str(log),
        "binary": str(binary),
        "binary_sha256": digest(binary),
        "binary_bytes": binary.stat().st_size,
        "object_manifest": {
            str(path.relative_to(out)): digest(path)
            for path in sorted(out.rglob("*.o"))
        },
        "generated_header_sha256": digest(out / "generated" / "tny_version.h"),
        "dependencies": checked(dependency_command, root, env, log),
    }


def validate(
    samples: list[dict[str, Any]], wire: str, iterations: int, warmups: int
) -> None:
    if len(samples) != iterations + warmups:
        raise ValueError("missing samples")
    for index, row in enumerate(samples):
        if (
            row["wire"] != wire
            or row["index"] != index
            or row["warmup"] != (index < warmups)
            or row["history_messages"] != 16
            or row["message_bytes"] != 4096
            or row["request_controls"] != 1
            or row["turn_ends"] != 1
            or row["errors"] != 0
            or row["http_bytes"] != 0
            or row["owners_before"] != row["owners_after"]
            or row["fds_before"] != row["fds_after"]
        ):
            raise ValueError("sample identity/resource oracle failed")
        for key in ("nanoseconds", "allocations", "peak_rss_bytes"):
            if type(row[key]) is not int or row[key] <= 0:
                raise ValueError(f"invalid measurement: {key}")


def summarize(rows: list[dict[str, Any]]) -> dict[str, Any]:
    measured = [row for row in rows if not row["warmup"]]
    result: dict[str, Any] = {"count": len(measured)}
    for key in ("nanoseconds", "allocations", "peak_rss_bytes"):
        values = sorted(row[key] for row in measured)
        if key == "peak_rss_bytes":
            # One high-water value per process; iterations share the same peak.
            values = sorted(
                max(row[key] for row in measured if row["batch"] == batch)
                for batch in {row["batch"] for row in measured}
            )
        result[key] = {
            "median": statistics.median(values),
            "p95": values[math.ceil(len(values) * 0.95) - 1],
            "min": values[0],
            "max": values[-1],
        }
    result["batch_median_nanoseconds"] = [
        statistics.median(
            row["nanoseconds"] for row in measured if row["batch"] == batch
        )
        for batch in sorted({row["batch"] for row in measured})
    ]
    result["owners_prepared"] = sorted({row["owners_prepared"] for row in measured})
    result["owners_boundary"] = sorted({row["owners_boundary"] for row in measured})
    return result


def execute(args: argparse.Namespace) -> dict[str, Any]:
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report: dict[str, Any] = {
        "schema": 1,
        "scope": "send to provider request hook; warmed connection; ephemeral session; no HTTP write",
        "host": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "iterations_per_batch": args.iterations,
        "warmups_per_batch": args.warmups,
        "batches": args.batches,
        "rss_scope": "process lifetime high-water including fixture setup, preflight and warmups; summary uses one final peak per batch",
        "allocation_scope": "instrumented allocation attempts, not bytes or all libc allocations",
        "owner_scope": "instrumented C++ owners/containers, not all C buffers",
        "harness_sha256": {name: digest(HERE / name) for name in HARNESS_FILES},
        "builds": {},
        "samples": {},
        "execution_order": [],
        "summaries": {},
        "comparisons": {},
        "valid": False,
    }
    labels = ("baseline", "candidate") if args.candidate else ("baseline",)
    try:
        for label in labels:
            report["builds"][label] = build(
                getattr(args, label).resolve(),
                getattr(args, f"{label}_build").resolve(),
                args,
                output / f"{label}-build.log",
            )
        if len(labels) == 2:
            before, after = (report["builds"][label] for label in labels)
            if before["normalized_flags"] != after["normalized_flags"]:
                raise ValueError("effective build flags differ between artifacts")
            if before["compiler_versions"] != after["compiler_versions"]:
                raise ValueError("compiler identities differ between artifacts")
        for wire in ("responses", "chat"):
            report["samples"][wire] = {label: [] for label in labels}
            for batch in range(args.batches):
                order = labels if batch % 2 == 0 else labels[::-1]
                for label in order:
                    with tempfile.TemporaryDirectory(
                        prefix="request-", dir=output
                    ) as temp:
                        root = Path(temp)
                        home, workspace, state = (
                            root / name for name in ("home", "workspace", "state")
                        )
                        for path in (home, workspace, state):
                            path.mkdir()
                        binary = report["builds"][label]["binary"]
                        command = [
                            binary,
                            wire,
                            str(args.iterations),
                            str(args.warmups),
                            str(workspace),
                            str(state),
                        ]
                        raw = checked(
                            command, workspace, isolated_env(home), output / "runs.log"
                        )
                        rows = [json.loads(line) for line in raw.splitlines()]
                    validate(rows, wire, args.iterations, args.warmups)
                    for row in rows:
                        row["batch"] = batch
                    report["samples"][wire][label].extend(rows)
                    report["execution_order"].append(
                        {"wire": wire, "batch": batch, "label": label}
                    )
            report["summaries"][wire] = {
                label: summarize(report["samples"][wire][label]) for label in labels
            }
            if len(labels) == 2:
                before, after = (report["summaries"][wire][label] for label in labels)
                report["comparisons"][wire] = {
                    f"{key}_candidate_over_baseline": after[key]["median"]
                    / before[key]["median"]
                    for key in ("nanoseconds", "allocations", "peak_rss_bytes")
                }
        for metadata in report["builds"].values():
            if digest(Path(metadata["binary"])) != metadata["binary_sha256"]:
                raise ValueError("artifact changed during measurement")
            if (
                source_manifest(Path(metadata["source_root"]), metadata["sources"])
                != metadata["source_manifest"]
            ):
                raise ValueError("source changed during measurement")
        if report["harness_sha256"] != {
            name: digest(HERE / name) for name in HARNESS_FILES
        }:
            raise ValueError("harness changed during build/measurement")
        report["valid"] = True
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
    (output / "report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--baseline-build", type=Path, required=True)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--candidate-build", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--warmups", type=int, default=10)
    parser.add_argument("--batches", type=int, default=6)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--make", default="make")
    args = parser.parse_args()
    if bool(args.candidate) != bool(args.candidate_build):
        parser.error("--candidate and --candidate-build must be supplied together")
    if not (
        1 <= args.iterations <= 100000
        and 0 <= args.warmups <= 100000
        and 1 <= args.batches <= 100
        and 1 <= args.jobs <= 64
    ):
        parser.error("invalid iteration, warmup, batch, or job count")
    try:
        report = execute(args)
    except OSError as exc:
        parser.error(str(exc))
    print(
        json.dumps(
            {
                "valid": report["valid"],
                "error": report.get("error"),
                "comparisons": report["comparisons"],
            },
            indent=2,
        )
    )
    print(f"report: {args.output / 'report.json'}")
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
