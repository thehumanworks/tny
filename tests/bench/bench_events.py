#!/usr/bin/env python3
"""Compare retained event ownership through the actual private runtime.

Both builds use the same callback corpus and release flags. Reuses the parser
benchmark's compiler/provenance runner and strict performance comparison;
backend callbacks overwrite their inputs immediately, and the C driver checks
retained bytes, ordering, exactly one terminal, and logical payload accounting.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import tempfile
from pathlib import Path
from typing import Any

import bench_parsers
import bench_startup


def execute(args: argparse.Namespace) -> dict[str, Any]:
    output = args.work_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    for key in ("TNY_TOOLS", "TNY_TEST_ALLOC_SCOPE", "TNY_TEST_ALLOC_FAIL_AT"):
        env.pop(key, None)
    harness = Path(__file__).with_suffix(".c").resolve()
    report: dict[str, Any] = {
        "schema": 1,
        "host": platform.platform(),
        "iterations": args.iterations,
        "batches": 3,
        "builds": {},
        "samples": {"baseline": [], "candidate": []},
        "passed": False,
    }
    try:
        for name in ("baseline", "candidate"):
            report["builds"][name] = bench_parsers.build(
                getattr(args, name).resolve(),
                output / name,
                harness,
                args,
                env,
                runtime=True,
            )
        for batch in range(3):
            order = (
                ("baseline", "candidate")
                if batch % 2 == 0
                else ("candidate", "baseline")
            )
            for name in order:
                with tempfile.TemporaryDirectory(prefix="tny-events-") as temp:
                    home = Path(temp) / "home"
                    workspace = Path(temp) / "workspace"
                    home.mkdir()
                    workspace.mkdir()
                    command = [
                        report["builds"][name]["binary"],
                        str(args.iterations),
                        str(workspace),
                    ]
                    sample = json.loads(
                        bench_parsers.checked(
                            command,
                            workspace,
                            bench_startup.isolated_env(home),
                            output / "runs.log",
                        )
                    )
                    sample["batch"] = batch
                    report["samples"][name].append(sample)
        report["comparison"] = bench_parsers.compare(
            report["samples"]["baseline"],
            report["samples"]["candidate"],
        )
        before_payload = {
            sample["peak_logical_payload_bytes"]
            for sample in report["samples"]["baseline"]
        }
        after_payload = {
            sample["peak_logical_payload_bytes"]
            for sample in report["samples"]["candidate"]
        }
        if before_payload != after_payload or len(before_payload) != 1:
            raise RuntimeError("logical event payload accounting changed")
        for metadata in report["builds"].values():
            if bench_parsers.digest(Path(metadata["binary"])) != metadata["sha256"]:
                raise RuntimeError(
                    "event benchmark artifact changed during measurement"
                )
        report["passed"] = report["comparison"]["passed"]
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
    (output / "report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=2000)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--make", default="make")
    args = parser.parse_args()
    if not 1 <= args.iterations <= 1000000:
        parser.error("--iterations must be between 1 and 1000000")
    try:
        report = execute(args)
    except OSError as exc:
        parser.error(str(exc))
    if "comparison" in report:
        value = report["comparison"]
        print(
            f"events: time={value['time_ratio']:.3f}x rss={value['rss_ratio']:.3f}x "
            f"{'PASS' if value['passed'] else 'FAIL'}"
        )
    if "error" in report:
        print(f"event benchmark failed: {report['error']}")
    print(f"report: {args.work_dir / 'report.json'}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
