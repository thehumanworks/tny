#!/usr/bin/env python3
"""Run SMT-LIB proof obligations; unknown/error/missing solver always fails."""

import re
import subprocess
from pathlib import Path

specs = sorted(Path(__file__).parent.glob("*.smt2"))
if not specs:
    raise SystemExit("no SMT-LIB proof obligations found")

for spec in specs:
    expected = len(re.findall(r"^\(check-sat\)$", spec.read_text(), re.M))
    assert expected, f"{spec}: no proof obligations"
    run = subprocess.run(["z3", str(spec)], capture_output=True, text=True, timeout=30)
    actual = run.stdout.splitlines()
    if run.returncode or actual != ["unsat"] * expected or run.stderr:
        raise SystemExit(
            f"{spec}: expected {expected} unsat results; got {actual!r}, "
            f"stderr={run.stderr!r}, status={run.returncode}"
        )
    print(f"{spec}: {expected} obligations proved")
