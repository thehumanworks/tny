"""Source mutations must be rejected by the actual Lean proof, not a hash."""

from __future__ import annotations

import ast
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
source = (ROOT / "tests/bench/code_mode/policy.py").read_text()


def mutate(function: str, operand: str) -> str:
    tree = ast.parse(source)
    definition = next(
        n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == function
    )
    expression = definition.body[0].value
    if not isinstance(expression, ast.BoolOp):
        raise ValueError("mutation expected a conjunction")
    if operand == "reverse-first-pass":
        comparison = next(n for n in expression.values if isinstance(n, ast.Compare))
        comparison.left, comparison.comparators[0] = (
            comparison.comparators[0],
            comparison.left,
        )
    else:
        original = len(expression.values)
        expression.values = [
            n
            for n in expression.values
            if not (isinstance(n, ast.Name) and n.id == operand)
        ]
        if len(expression.values) != original - 1:
            raise ValueError("mutation expected exactly one matching operand")
    return ast.unparse(tree) + "\n"


mutants = {
    "ignore-output": mutate("accept", "output_ok"),
    "reverse-first-pass": mutate("promote", "reverse-first-pass"),
    "ignore-completeness": mutate("promote", "complete"),
    "ignore-parity": mutate("promote", "parity"),
}
with tempfile.TemporaryDirectory(prefix="tny-code-mode-mutations-") as temporary:
    for name, content in mutants.items():
        path = Path(temporary) / f"{name}.py"
        path.write_text(content)
        result = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tests/formal/check_code_mode_language.py"),
                "--source",
                str(path),
            ],
            env=os.environ,
            capture_output=True,
            text=True,
            timeout=65,
        )
        if result.returncode == 0 or "error:" not in result.stdout:
            print(result.stdout, result.stderr)
            raise SystemExit(f"mutation not rejected by Lean: {name}")
        print(f"Lean rejected {name}")
