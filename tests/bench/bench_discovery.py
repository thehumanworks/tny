#!/usr/bin/env python3
"""Measure discovery output bytes through production code cells and a local provider.

Compare the old full-catalog discovery workflow with bounded search followed by
the same complete read_file schema. No paid inference, token estimator, or claim
of model quality/latency improvement. Both binaries run with synthetic credentials
and private HOME/workspaces through the existing integration fixture.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "integration"))
import test_execution_code_mode as fixture  # noqa: E402


def measure(binary: Path, discovery: str) -> dict:
    fixture.TNY = str(binary.resolve(strict=True))
    case = fixture.ExecutionCodeMode()
    try:
        case.setUp()
        output = case.run_code(
            f"discovery = {discovery}\n"
            "schema = tools.describe('read_file')\n"
            "assert json.loads(schema)['function']['name'] == 'read_file'\n"
            "print(json.dumps({'discovery': discovery, 'schema': schema}))\n"
        )
        result = json.loads(output)
        return {
            "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
            "discovery_utf8_bytes": len(result["discovery"].encode()),
            "selected_schema_utf8_bytes": len(result["schema"].encode()),
            "selected_schema_sha256": hashlib.sha256(
                result["schema"].encode()
            ).hexdigest(),
            "workflow_utf8_bytes": len(result["discovery"].encode())
            + len(result["schema"].encode()),
        }
    finally:
        case.doCleanups()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    before = measure(args.baseline, "tools.list()")
    after = measure(args.candidate, "tools.search('read_file')")
    if before["selected_schema_sha256"] != after["selected_schema_sha256"]:
        raise RuntimeError("selected schema changed; comparison is not equivalent")
    report = {
        "metric": "UTF-8 output bytes, not tokenizer counts or model quality",
        "baseline_workflow": "tools.list() + tools.describe('read_file')",
        "candidate_workflow": "tools.search('read_file') + tools.describe('read_file')",
        "baseline": before,
        "candidate": after,
        "reduction_percent": 100
        * (1 - after["workflow_utf8_bytes"] / before["workflow_utf8_bytes"]),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
