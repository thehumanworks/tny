"""Serial empty-cell timings for probe executors (trusted empty source only).

Rotates executor order per round after warmup; records in-process runtime_ns
(initialize + evaluate + destroy) and whole-process wall time. Warm page cache;
not bubblewrapped; not a cold-machine or generated-program measurement.
"""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import time
from pathlib import Path

BUILD = Path(__file__).resolve().parents[3] / "build/python-runtime-bench"


def once(binary: Path) -> tuple[float, float]:
    wire = json.dumps({"code": "", "catalog": "[]", "fixture": {"files": {}}}).encode()
    start = time.perf_counter_ns()
    done = subprocess.run([str(binary)], input=wire, capture_output=True, check=True)
    wall = time.perf_counter_ns() - start
    result = json.loads(done.stdout)
    if result["runtime_ok"] is not True:
        raise SystemExit(f"{binary.name}: empty cell failed")
    return result["runtime_ns"] / 1e6, wall / 1e6


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executor", action="append", required=True)
    parser.add_argument("--rounds", type=int, default=50)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    names = args.executor
    for _ in range(5):
        for name in names:
            once(BUILD / name)
    samples: dict[str, list[tuple[float, float]]] = {name: [] for name in names}
    for index in range(args.rounds):
        for offset in range(len(names)):
            name = names[(index + offset) % len(names)]
            samples[name].append(once(BUILD / name))
    report = {
        name: {
            "rounds": len(values),
            "runtime_ms_median": statistics.median(v[0] for v in values),
            "process_ms_median": statistics.median(v[1] for v in values),
            "runtime_ms": [v[0] for v in values],
            "process_ms": [v[1] for v in values],
        }
        for name, values in samples.items()
    }
    args.output.write_text(json.dumps(report, indent=1) + "\n")
    for name, row in report.items():
        print(f"{name}: runtime {row['runtime_ms_median']:.3f} ms, process {row['process_ms_median']:.3f} ms")


if __name__ == "__main__":
    main()
