"""Serial native initialization/process timings using only trusted controls."""

from __future__ import annotations

import json
import os
import statistics
import subprocess
import time

from cases import CATALOG, dump
from execute import BUILD
from run import atomic_json


def main() -> None:
    languages = ("empty", "lua", "javascript", "python")
    payload = dump(
        {"code": "", "catalog": dump(CATALOG), "fixture": {"files": {}}}
    ).encode()
    observations = {language: [] for language in languages}
    for repetition in range(55):
        for offset in range(4):
            language = languages[(repetition + offset) % 4]
            start = time.perf_counter_ns()
            result = subprocess.run(
                [str(BUILD / language)],
                input=payload,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=True,
                timeout=5,
                env={
                    "PATH": "/usr/bin:/bin",
                    "HOME": "/nonexistent",
                    "LANG": "C.UTF-8",
                },
            )
            wall = time.perf_counter_ns() - start
            measured = json.loads(result.stdout)
            if (
                measured["runtime_ok"] is not True
                or measured["calls"]
                or measured["writes"]
            ):
                raise ValueError("native empty-cell control failed")
            if repetition >= 5:
                observations[language].append(
                    {"runtime_ns": measured["runtime_ns"], "process_wall_ns": wall}
                )
    report = {
        "protocol": "50 serial, rotated, empty-cell native processes per arm after 5 warmup blocks. No model-generated code; no bwrap overhead. Warm OS page cache, not a cold-machine benchmark.",
        "platform": dict(
            zip(
                ("sysname", "nodename", "release", "version", "machine"),
                os.uname(),
                strict=True,
            )
        ),
        "observations": observations,
        "summary": {
            language: {
                key + "_median": statistics.median(row[key] for row in values)
                for key in ("runtime_ns", "process_wall_ns")
            }
            for language, values in observations.items()
        },
    }
    atomic_json(BUILD / "native-timings.json", report)
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
