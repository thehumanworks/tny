"""Fail-closed translation of production benchmark policy AST into Lean 4.

Trusted base: Python AST/parser, this small whitelist translator, Lean's kernel,
and correspondence of Python bool/unbounded int with Lean Bool/Int. This is not
a proof of the interpreters, OS, scorer's inputs, statistical estimator, or C code.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / "tests/bench/code_mode/policy.py"
PROOFS = ROOT / "tests/formal/code_mode_language/Proofs.lean"
EXPECTED = {
    "accept": [("execution_ok", "bool"), ("output_ok", "bool"), ("trace_ok", "bool")],
    "promote": [
        ("complete", "bool"),
        ("confirmed_gain", "bool"),
        ("parity", "bool"),
        ("first_candidate", "int"),
        ("first_baseline", "int"),
        ("final_candidate", "int"),
        ("final_baseline", "int"),
    ],
}


def translate(source: str) -> str:
    tree = ast.parse(source)
    definitions = []
    found = []
    for node in tree.body:
        if (
            isinstance(node, ast.Expr)
            and isinstance(node.value, ast.Constant)
            and isinstance(node.value.value, str)
        ):
            continue
        if not isinstance(node, ast.FunctionDef) or node.name not in EXPECTED:
            raise ValueError("unsupported top-level policy syntax")
        found.append(node.name)
        signature = [
            (a.arg, a.annotation.id if isinstance(a.annotation, ast.Name) else "")
            for a in node.args.args
        ]
        if (
            signature != EXPECTED[node.name]
            or not isinstance(node.returns, ast.Name)
            or node.returns.id != "bool"
        ):
            raise ValueError("unexpected policy type signature")
        if (
            node.decorator_list
            or node.args.defaults
            or node.args.kwonlyargs
            or node.args.posonlyargs
            or node.args.vararg
            or node.args.kwarg
            or len(node.body) != 1
            or not isinstance(node.body[0], ast.Return)
        ):
            raise ValueError("only plain, single-return pure definitions are supported")
        names = dict(signature)

        def expression(value: ast.expr) -> tuple[str, str]:
            if isinstance(value, ast.Name) and value.id in names:
                return value.id, names[value.id]
            if isinstance(value, ast.BoolOp) and isinstance(value.op, ast.And):
                parts = [expression(v) for v in value.values]
                if any(kind != "bool" for _, kind in parts):
                    raise ValueError("non-Boolean and is unsupported")
                return "(" + " && ".join(text for text, _ in parts) + ")", "bool"
            if (
                isinstance(value, ast.Compare)
                and len(value.ops) == 1
                and isinstance(value.ops[0], ast.GtE)
            ):
                left, lt = expression(value.left)
                right, rt = expression(value.comparators[0])
                if lt != "int" or rt != "int":
                    raise ValueError("comparison requires exact unbounded integers")
                return f"(decide ({left} ≥ {right}))", "bool"
            raise ValueError(f"unsupported policy expression: {type(value).__name__}")

        result, kind = expression(node.body[0].value)
        if kind != "bool":
            raise ValueError("return is not Boolean")
        arguments = " ".join(
            f"({name} : {'Bool' if typ == 'bool' else 'Int'})"
            for name, typ in signature
        )
        definitions.append(f"def {node.name} {arguments} : Bool := {result}")
    if sorted(found) != sorted(EXPECTED):
        raise ValueError("missing or duplicate policy definitions")
    return (
        "import Std\nnamespace CodeMode\n"
        + "\n\n".join(definitions)
        + "\nend CodeMode\n"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=POLICY,
        help="Mutation-test input only; default is the actual gate",
    )
    parser.add_argument("--lean", default=os.environ.get("LEAN", "lean"))
    args = parser.parse_args()
    source = args.source.read_text()
    generated = translate(source)
    proofs = PROOFS.read_text()
    if "sorry" in proofs or "axiom " in proofs:
        raise SystemExit("Untrusted proof placeholder or axiom declaration")
    evidence = json.loads(
        (ROOT / "docs/verification/code-mode-language/data/decision.json").read_text()
    )
    candidates = evidence["candidates"]
    if evidence["selected"] != "lua" or sorted(c["language"] for c in candidates) != [
        "javascript",
        "python",
    ]:
        raise ValueError("unexpected observed-decision cohort")
    examples = []
    for candidate in candidates:
        flags = [candidate[key] for key in ("complete", "confirmed_gain", "parity")]
        counts = [
            candidate[key]
            for key in (
                "first_candidate",
                "first_baseline",
                "final_candidate",
                "final_baseline",
            )
        ]
        if any(type(x) is not bool for x in flags) or any(
            type(x) is not int or x < 0 for x in counts
        ):
            raise ValueError("invalid observed-decision types")
        if candidate["promote"] is not False:
            raise ValueError("retention receipt contains an admitted candidate")
        arguments = " ".join(
            [*(str(flag).lower() for flag in flags), *(str(count) for count in counts)]
        )
        examples.append(f"example : CodeMode.promote {arguments} = false := by decide")
    proofs += "\n" + "\n".join(examples) + "\n"
    version = subprocess.check_output([args.lean, "--version"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix="tny-code-mode-lean-") as temporary:
        path = Path(temporary) / "Checked.lean"
        path.write_text(generated + "\n" + proofs)
        subprocess.run([args.lean, str(path)], check=True, timeout=60)
    print(version)
    print(
        "Lean verified 18 source-linked theorems and both observed retention decisions"
    )
    print("policy SHA256:", hashlib.sha256(source.encode()).hexdigest())
    print("proof SHA256:", hashlib.sha256(proofs.encode()).hexdigest())


if __name__ == "__main__":
    main()
