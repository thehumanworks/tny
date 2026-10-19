#!/usr/bin/env python3
"""Measure model-facing instruction stability, not paid cache hits or tokens.

Runs actual edit/recovery code cells against the existing local provider fixture.
Two warmup tasks promote learning. Later tasks preserve the same eligible advice
while increasing its private evidence counters. No real provider is contacted.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "integration"))
from test_default_learning import Fixture  # noqa: E402


def measure(binary: Path) -> dict:
    binary = binary.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="tny-prefix-") as temporary:
        fixture = Fixture(Path(temporary), binary, wire="responses")
        try:
            prompts = []
            outcomes = []
            for turn in range(5):
                outcomes.append(fixture.run(str(turn)))
                prompts.append(fixture.requests[0]["instructions"].encode())
            eligible = prompts[2:]
            common = len(os.path.commonprefix(eligible))
            if not all(row["passed"] for row in outcomes):
                raise RuntimeError("fixture edit failed")
            return {
                "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "eligible_prompt_bytes": [len(p) for p in eligible],
                "eligible_common_prefix_bytes": common,
                "identical_eligible_prompts": len(set(eligible)) == 1,
                "passed_tasks": len(outcomes),
                "provider_requests": [row["provider_requests"] for row in outcomes],
                "tool_calls": [row["tool_calls"] for row in outcomes],
            }
        finally:
            fixture.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = {
        "metric": "Exact UTF-8 instruction prefix bytes; not model tokens or provider cache hits",
        "baseline": measure(args.baseline),
        "candidate": measure(args.candidate),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
