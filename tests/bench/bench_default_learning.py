#!/usr/bin/env python3
"""Measure automatic native learning, not a manually selected preset.

Real tny executes real file tools against a deterministic mock model. Its policy
retries blindly until learned advice actually reaches its request. Results prove
plumbing and this replay policy, NOT live-model/general coding improvements.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import platform
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURE_PATH = ROOT / "tests/integration/test_default_learning.py"
SPEC = importlib.util.spec_from_file_location("default_learning_fixture", FIXTURE_PATH)
FIXTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURE)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(binary, baseline, cases):
    with tempfile.TemporaryDirectory(prefix="tny-auto-learning-benchmark-") as temp:
        raw = []
        for profile in ("all", "terminal", "terminal+edit"):
            arms = [("default", binary, False), ("disabled", binary, True)]
            if baseline:
                arms.append(("pre_change", baseline, False))
            for arm, executable, disabled in arms:
                fixture = FIXTURE.Fixture(
                    Path(temp) / profile / arm, executable, profile
                )
                try:
                    warmup = [
                        fixture.run(f"warm-{i}", disable=disabled) for i in range(2)
                    ]
                    before = fixture.state()
                    evaluation = [
                        fixture.run(f"evaluation-{i}", disable=disabled)
                        for i in range(cases)
                    ]
                    after = fixture.state()
                    if not all(
                        r["passed"] and r["task_preset"] is None
                        for r in warmup + evaluation
                    ):
                        raise ValueError(
                            "fixture failed independent file checks or selected a preset"
                        )
                    if any(r["guided_at_start"] for r in warmup):
                        raise ValueError(
                            "cold workspace unexpectedly had learned advice"
                        )
                    expected = arm == "default"
                    if any(r["guided_at_start"] != expected for r in evaluation):
                        raise ValueError(
                            "default/disabled/pre-change guidance mismatch"
                        )
                    raw.append(
                        {
                            "profile": profile,
                            "arm": arm,
                            "warmup": warmup,
                            "evaluation": evaluation,
                            "state_before_holdout": before,
                            "state_after_holdout": after,
                        }
                    )
                finally:
                    fixture.close()
        totals = {}
        for arm in ("default", "disabled", "pre_change"):
            rows = [r for r in raw if r["arm"] == arm]
            if not rows:
                continue
            totals[arm] = {}
            for split in ("warmup", "evaluation"):
                examples = [x for row in rows for x in row[split]]
                totals[arm][split] = {
                    "cases": len(examples),
                    "passed": sum(x["passed"] for x in examples),
                    **{
                        key: sum(x[key] for x in examples)
                        for key in ("tool_calls", "provider_requests", "failed_edits")
                    },
                }
        sources = [
            "src/core/learning.c",
            "src/core/learning.h",
            "src/util/learning_store.c",
            "src/util/learning_store.h",
            "src/core/tools.c",
            "src/core/tools_fs.c",
            "src/core/tools_shell.c",
            "src/core/intercept.c",
            "src/backends/openai/openai.c",
            "tests/integration/test_default_learning.py",
            "tests/bench/bench_default_learning.py",
        ]
        return {
            "kind": "default_native_learning_replay",
            "schema_version": 1,
            "environment": {"python": sys.version, "platform": platform.platform()},
            "binary": {
                "sha256": digest(binary),
                "bytes": Path(binary).stat().st_size,
                "version": subprocess.check_output(
                    [str(binary), "--version"], text=True
                ).strip(),
            },
            "baseline_binary": (
                {
                    "sha256": digest(baseline),
                    "bytes": Path(baseline).stat().st_size,
                    "version": subprocess.check_output(
                        [str(baseline), "--version"], text=True
                    ).strip(),
                }
                if baseline
                else None
            ),
            "sources_sha256": {p: digest(ROOT / p) for p in sources},
            "summary": totals,
            "runs": raw,
            "limitations": [
                "Deterministic local mock policy, not live model inference or general coding gains.",
                "All arms run two ordinary warmup tasks per profile; default alone learns automatically.",
                "The fixture follows advice when present; a real model may ignore it.",
                "Evaluation is online: default continues learning after each case; this is not a frozen held-out generalization study.",
                "Counts are actual native tool calls and mock HTTP requests, not latency/tokens/dollars.",
                "Setup and independent oracle file reads are excluded; warmup costs are reported separately.",
                "No --task preset, extension, Python optimization controller, or manual promotion was used.",
            ],
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tny", type=Path, default=ROOT / "build/tny")
    parser.add_argument(
        "--baseline", type=Path, help="optional pre-change native binary"
    )
    parser.add_argument(
        "--cases", type=int, default=4, help="online evaluation cases per tool profile"
    )
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.cases <= 20:
        parser.error("cases must be 1..20")
    if args.out.exists():
        parser.error("output must be a new file")
    report = run(
        args.tny.resolve(),
        args.baseline.resolve() if args.baseline else None,
        args.cases,
    )
    with args.out.open("x") as output:
        json.dump(report, output, indent=2, sort_keys=True)
        output.write("\n")
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
