#!/usr/bin/env python3
"""Compile baseline/candidate production encoders with identical flags and corpus.

Measures session-state wire bytes and encoder CPU time, not tokens, model quality,
whole-cell latency, durable writes or provider caching. No inference or network.
"""

import argparse
import hashlib
import json
import os
import platform
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def checked(command, **kwargs):
    return subprocess.run(
        command, check=True, capture_output=True, text=True, **kwargs
    ).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--baseline", default="60c429e62822b5db1692af88cdb7b518f244ba58"
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    compiler = os.environ.get("CC", "cc")
    flags = [
        "-O2",
        "-std=c11",
        "-D_GNU_SOURCE",
        "-ffunction-sections",
        "-fdata-sections",
    ]
    strip = "-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections"
    sources = [
        "tests/bench/bench_execution_snapshot.c",
        "src/util/util.c",
        "src/util/alloc.c",
        "src/json/json.c",
        "third_party/yyjson/yyjson.c",
    ]
    report = {
        "metric": "Session-state JSON bytes and encoder CPU ms; not full-cell latency or tokens",
        "platform": platform.platform(),
        "compiler": checked([compiler, "--version"]).splitlines()[0],
        "flags": flags,
        "baseline_revision": args.baseline,
        "shared_source_sha256": {
            name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
            for name in sources
        },
    }
    with tempfile.TemporaryDirectory(prefix="tny-snapshot-bench-") as temporary:
        work = Path(temporary)
        baseline = checked(
            ["git", "show", f"{args.baseline}:src/core/execution_state.c"], cwd=ROOT
        )
        candidate = (ROOT / "src/core/execution_state.c").read_text()
        for name, source in (("baseline", baseline), ("candidate", candidate)):
            path = work / f"{name}.c"
            path.write_text(source)
            binary = work / name
            checked(
                [
                    compiler,
                    *flags,
                    "-Iinclude",
                    "-Isrc",
                    "-Ithird_party/yyjson",
                    str(path),
                    *sources,
                    strip,
                    "-pthread",
                    "-lm",
                    "-o",
                    str(binary),
                ],
                cwd=ROOT,
            )
            report[name] = {
                "encoder_sha256": hashlib.sha256(source.encode()).hexdigest(),
                "samples": [
                    json.loads(checked([str(binary), str(turns)]))
                    for turns in (10, 100, 1000)
                ],
            }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
