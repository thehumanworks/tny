#!/usr/bin/env python3
"""Targeted Python mutations for instruction evolution, without editing sources.

The C mutation harness does not load Python. Each child loads one changed module
in memory and runs the existing focused integration assertions. Compile/import
errors and timeouts are invalid checks, not killed mutants. No provider I/O.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import subprocess
import sys
import types
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "python/tny_improve.py"
TESTS = ROOT / "tests/integration/test_instruction_improvement.py"
MUTANTS = {
    "skip-validation": (
        'for split in ("train", "validation"):',
        'for split in ("train",):',
        "test_pareto_gate_regressions_inflation_ties",
    ),
    "allow-pass-regression": (
        'if any(old["passed"] and not new["passed"] for old, new in zip(before, after)):',
        'if False and any(old["passed"] and not new["passed"] for old, new in zip(before, after)):',
        "test_pareto_gate_regressions_inflation_ties",
    ),
    "allow-cost-inflation": (
        "if _total(after) > _total(before):",
        "if False:",
        "test_pareto_gate_regressions_inflation_ties",
    ),
    "allow-ties": (
        "if improved or _total(new) < _total(old):",
        "if True:",
        "test_pareto_gate_regressions_inflation_ties",
    ),
    "discard-accepted-parent": (
        "parent, scores = body, candidate",
        "parent, scores = baseline, scores",
        "test_multiround_parent_reuse_and_holdout_isolation",
    ),
    "skip-archive-hash": (
        '_require(_sha(_read(root / name)) == digest, "archive hash mismatch: " + name)',
        '_require(True, "archive hash mismatch: " + name)',
        "test_archive_tampering_and_missing_evidence",
    ),
}


def child(name: str) -> int:
    source = SOURCE.read_text()
    if name != "baseline":
        old, new, test = MUTANTS[name]
        if source.count(old) != 1:
            raise ValueError(f"mutation anchor is not unique: {name}")
        source = source.replace(old, new, 1)
    code = compile(source, str(SOURCE), "exec")
    module = types.ModuleType("tny_improve")
    module.__file__ = str(SOURCE)
    exec(code, module.__dict__)
    sys.modules["tny_improve"] = module
    spec = importlib.util.spec_from_file_location("improvement_checks", TESTS)
    tests = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tests)
    loader = unittest.TestLoader()
    suite = (
        loader.loadTestsFromModule(tests)
        if name == "baseline"
        else loader.loadTestsFromName("ImprovementTests." + test, tests)
    )
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    if not result.testsRun:
        return 3
    return 0 if result.wasSuccessful() else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path)
    parser.add_argument(
        "--child", choices=["baseline", *MUTANTS], help=argparse.SUPPRESS
    )
    args = parser.parse_args()
    if args.child:
        try:
            return child(args.child)
        except Exception as exc:
            print(f"invalid mutation check: {exc}", file=sys.stderr)
            return 3
    rows = []
    for name in ["baseline", *MUTANTS]:
        run = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()), "--child", name],
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        expected = 0 if name == "baseline" else 1
        rows.append(
            {
                "name": name,
                "exit_code": run.returncode,
                "expected_exit_code": expected,
                "passed": run.returncode == expected,
                "stderr": run.stderr,
            }
        )
        if name == "baseline" and run.returncode:
            break
    report = {
        "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        "tests_sha256": hashlib.sha256(TESTS.read_bytes()).hexdigest(),
        "results": rows,
        "passed": len(rows) == len(MUTANTS) + 1 and all(r["passed"] for r in rows),
    }
    text = json.dumps(report, indent=2) + "\n"
    if args.out:
        args.out.write_text(text)
    print(text)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
