#!/usr/bin/env python3
"""Prove the preregistered Python runtime-selection gate from its actual AST.

tests/bench/python_runtime/policy.py is read, never edited. A fail-closed
whitelist translator emits Lean 4 definitions over Int (Python's unbounded
int) and Bool; RuntimeSelection.lean proves independent properties of them.
The recorded held-out decision is replayed through the imported Python
function and through the Lean kernel as a cross-check.

Trusted base: CPython's ast module, this translator, the Lean 4 kernel and
its standard axioms, and the annotated argument types (Python does not
enforce annotations; `and` returns an operand, which is a bool here only
because every operand is a comparison or a bool-annotated parameter). This
is not evidence about the stochastic trial, interpreters, or OS containment.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import importlib.util
import itertools
import json
import os
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
POLICY = ROOT / "tests/bench/python_runtime/policy.py"
EVIDENCE = ROOT / "docs/verification/python-code-mode/data/heldout-analysis.json"
PROOFS = HERE / "RuntimeSelection.lean"
TOOLCHAIN = HERE / "lean-toolchain"
FUNCTION = "select_lighter"
SIGNATURE = [
    ("corpus_passed", "int"),
    ("corpus_total", "int"),
    ("trials_complete", "bool"),
    ("first_lighter", "int"),
    ("first_cpython", "int"),
    ("final_lighter", "int"),
    ("final_cpython", "int"),
    ("tokens_lighter", "int"),
    ("solved_lighter", "int"),
    ("tokens_cpython", "int"),
    ("solved_cpython", "int"),
    ("semantics_ok", "bool"),
    ("bytes_lighter", "int"),
    ("bytes_cpython", "int"),
]
REQUIRED = (
    "select_iff",
    "corpus_must_fully_pass",
    "empty_corpus_rejected",
    "incomplete_trials_rejected",
    "no_first_pass_regression",
    "no_final_regression",
    "unsolved_rejected",
    "token_regression_rejected",
    "token_criterion_scale_invariant",
    "semantics_required",
    "size_gate",
    "selected_is_smaller",
    "nonvacuity",
)
STANDARD_AXIOMS = {"propext", "Classical.choice", "Quot.sound"}
FORBIDDEN = re.compile(
    r"\b(sorry|admit|axiom|native_decide|bv_decide|implemented_by|extern|unsafe|"
    r"opaque|partial|ofReduceBool|debug\.skipKernelTC)\b|#exit|\bnamespace\s+PyPolicy\b"
)
COMPARE = {ast.Gt: ">", ast.GtE: "≥", ast.Lt: "<", ast.LtE: "≤", ast.Eq: "=", ast.NotEq: "≠"}
ARITH = {ast.Add: "+", ast.Sub: "-", ast.Mult: "*"}


class Unsupported(ValueError):
    """policy.py left the translator's whitelist."""


def translate(source: str) -> tuple[str, list[tuple[str, str]]]:
    tree = ast.parse(source)
    found = []
    for node in tree.body:
        if (
            isinstance(node, ast.Expr)
            and isinstance(node.value, ast.Constant)
            and isinstance(node.value.value, str)
        ):
            continue
        if not isinstance(node, ast.FunctionDef) or node.name != FUNCTION:
            raise Unsupported(f"unsupported top-level statement: {ast.dump(node)[:80]}")
        found.append(node)
    if len(found) != 1:
        raise Unsupported(f"expected exactly one {FUNCTION} definition")
    node = found[0]
    arguments = node.args
    signature = [
        (a.arg, a.annotation.id if isinstance(a.annotation, ast.Name) else "")
        for a in arguments.args
    ]
    if signature != SIGNATURE:
        raise Unsupported("the preregistered signature changed")
    if (
        node.decorator_list
        or arguments.defaults
        or arguments.kwonlyargs
        or arguments.posonlyargs
        or arguments.vararg
        or arguments.kwarg
        or getattr(node, "type_params", [])
        or not isinstance(node.returns, ast.Name)
        or node.returns.id != "bool"
        or len(node.body) != 1
        or not isinstance(node.body[0], ast.Return)
        or node.body[0].value is None
    ):
        raise Unsupported("only a plain single-return bool definition is supported")
    types = dict(signature)

    def expression(value: ast.expr) -> tuple[str, str]:
        if isinstance(value, ast.Name) and value.id in types:
            return f"«{value.id}»", types[value.id]
        if (
            isinstance(value, ast.Constant)
            and type(value.value) is int
            and value.value >= 0
        ):
            return f"({value.value} : Int)", "int"
        if isinstance(value, ast.BoolOp):
            parts = [expression(v) for v in value.values]
            if any(kind != "bool" for _, kind in parts):
                raise Unsupported("and/or over a non-bool operand returns that operand")
            op = " && " if isinstance(value.op, ast.And) else " || "
            return "(" + op.join(text for text, _ in parts) + ")", "bool"
        if isinstance(value, ast.UnaryOp) and isinstance(value.op, ast.Not):
            text, kind = expression(value.operand)
            if kind != "bool":
                raise Unsupported("not over a non-bool operand")
            return f"(!{text})", "bool"
        if isinstance(value, ast.Compare):
            if len(value.ops) != 1 or type(value.ops[0]) not in COMPARE:
                raise Unsupported("only single int comparisons are supported")
            left, lt = expression(value.left)
            right, rt = expression(value.comparators[0])
            if lt != "int" or rt != "int":
                raise Unsupported("comparison requires int operands")
            return f"(decide ({left} {COMPARE[type(value.ops[0])]} {right}))", "bool"
        if isinstance(value, ast.BinOp) and type(value.op) in ARITH:
            left, lt = expression(value.left)
            right, rt = expression(value.right)
            if lt != "int" or rt != "int":
                raise Unsupported("arithmetic requires int operands")
            return f"({left} {ARITH[type(value.op)]} {right})", "int"
        raise Unsupported(f"unsupported expression: {type(value).__name__}")

    body, kind = expression(node.body[0].value)
    if kind != "bool":
        raise Unsupported("return value is not bool")
    binders = " ".join(
        f"(«{name}» : {'Bool' if typ == 'bool' else 'Int'})" for name, typ in signature
    )
    lean = (
        "-- GENERATED by check_runtime_selection.py from the AST of\n"
        "-- tests/bench/python_runtime/policy.py. Do not edit; do not commit.\n"
        f"namespace PyPolicy\ndef {FUNCTION} {binders} : Bool :=\n  {body}\nend PyPolicy\n"
    )
    return lean, signature


def load_policy(path: Path):
    spec = importlib.util.spec_from_file_location("tny_selection_policy", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.select_lighter


def lean_argument(value: object, typ: str) -> str:
    if typ == "bool":
        if type(value) is not bool:
            raise ValueError(f"expected bool, got {value!r}")
        return "true" if value else "false"
    if type(value) is not int:
        raise ValueError(f"expected int (not bool), got {value!r}")
    return f"({value} : Int)"


def vectors() -> list[dict]:
    # A selected base, every single-field perturbation, and seeded random rows.
    base = {
        "corpus_passed": 36,
        "corpus_total": 36,
        "trials_complete": True,
        "first_lighter": 35,
        "first_cpython": 35,
        "final_lighter": 36,
        "final_cpython": 35,
        "tokens_lighter": 9000,
        "solved_lighter": 36,
        "tokens_cpython": 8750,
        "solved_cpython": 35,
        "semantics_ok": True,
        "bytes_lighter": 4000000,
        "bytes_cpython": 4668312,
    }
    rows = [dict(base)]
    for name, typ in SIGNATURE:
        choices = [False, True] if typ == "bool" else [base[name] + d for d in (-1, 1)] + [0, -1]
        for value in choices:
            rows.append({**base, name: value})
    rows.append({**base, "tokens_lighter": 8750 * 36 // 35 + 1})
    rows.append({**base, "tokens_lighter": 9000, "tokens_cpython": 8750})
    generator = random.Random(20260927)
    for _ in range(300):
        rows.append(
            {
                name: generator.random() < 0.8
                if typ == "bool"
                else generator.choice([-1, 0, 1, 2, 3, 35, 36, 1000])
                for name, typ in SIGNATURE
            }
        )
    for a, b in itertools.product(range(0, 4), repeat=2):
        rows.append({**base, "tokens_lighter": a, "solved_lighter": b, "tokens_cpython": b, "solved_cpython": a})
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=POLICY, help="mutation tests only")
    parser.add_argument("--lean", default=os.environ.get("LEAN", "lean"))
    parser.add_argument("--emit", type=Path, help="also write the checked Lean file here")
    args = parser.parse_args()
    source = args.source.read_text()
    proofs = PROOFS.read_text()
    stripped = re.sub(r"--[^\n]*|/-.*?-/", "", proofs, flags=re.S)
    if match := FORBIDDEN.search(stripped):
        raise SystemExit(f"forbidden token in proof file: {match.group(0)!r}")
    version = subprocess.run(
        [args.lean, "--version"], capture_output=True, text=True, check=True
    ).stdout.strip()
    pinned = TOOLCHAIN.read_text().strip().rsplit(":v", 1)[-1]
    if f"version {pinned}," not in version:
        raise SystemExit(f"Lean {pinned} required, found: {version}")
    try:
        generated, signature = translate(source)
    except (Unsupported, SyntaxError) as error:
        print(f"translator: unsupported source: {error}", file=sys.stderr)
        raise SystemExit(3) from error
    select = load_policy(args.source)
    # Replay: recorded held-out inputs, and a cross-check against Python.
    evidence = json.loads(EVIDENCE.read_text())["selection"]
    inputs = evidence["inputs"]
    if sorted(inputs) != sorted(n for n, _ in signature):
        raise SystemExit("recorded inputs do not match the gate signature")
    rows = [inputs, *vectors()]
    replay = ["namespace PyPolicy.Replay", "open PyPolicy"]
    for index, row in enumerate(rows):
        expected = select(**row)
        if type(expected) is not bool:
            raise SystemExit(f"policy returned non-bool {expected!r}")
        arguments = " ".join(lean_argument(row[n], t) for n, t in signature)
        replay.append(
            f"example : {FUNCTION} {arguments} = {str(expected).lower()} := by decide"
        )
        if index == 0 and expected is not evidence["select_lighter"]:
            raise SystemExit("recorded decision differs from the current gate")
    # Which proved theorems alone explain the recorded rejection.
    recorded = " ".join(lean_argument(inputs[n], t) for n, t in signature)
    reasons = []
    if inputs["bytes_lighter"] >= inputs["bytes_cpython"]:
        reasons.append("size_gate")
        replay.append(
            f"example : {FUNCTION} {recorded} = false :=\n"
            "  RuntimeSelection.size_gate _ _ _ _ _ _ _ _ _ _ _ _ _ _ (by decide)"
        )
    if inputs["semantics_ok"] is False:
        reasons.append("semantics_required")
        replay.append(
            f"example : {FUNCTION} {recorded} = false :=\n"
            "  RuntimeSelection.semantics_required _ _ _ _ _ _ _ _ _ _ _ _ _"
        )
    selected = sum(bool(select(**row)) for row in rows)
    replay.append("end PyPolicy.Replay\n")
    names = [f"RuntimeSelection.{n}" for n in REQUIRED]
    report = "\n".join(f"#print axioms {n}" for n in names)
    checked = "\n".join([generated, proofs, "\n".join(replay), report, ""])
    if args.emit:
        args.emit.write_text(checked)
    with tempfile.TemporaryDirectory(prefix="tny-runtime-selection-") as temporary:
        path = Path(temporary) / "RuntimeSelectionChecked.lean"
        path.write_text(checked)
        result = subprocess.run(
            [args.lean, str(path)], capture_output=True, text=True, timeout=300, check=False
        )
    if result.returncode or "error:" in result.stdout:
        print(result.stdout[-8000:], result.stderr[-4000:])
        print("Lean rejected the proof of the translated selection gate")
        raise SystemExit(1)
    axioms: dict[str, list[str]] = {}
    for line in result.stdout.splitlines():
        if m := re.match(r"'([^']+)' depends on axioms: \[(.*)\]$", line):
            axioms[m.group(1)] = [a.strip() for a in m.group(2).split(",")]
        elif m := re.match(r"'([^']+)' does not depend on any axioms$", line):
            axioms[m.group(1)] = []
    if missing := [n for n in names if n not in axioms]:
        raise SystemExit(f"missing axiom report for: {missing}")
    if bad := {n: a for n, a in axioms.items() if not set(a) <= STANDARD_AXIOMS}:
        raise SystemExit(f"non-standard axioms: {bad}")
    if args.source.read_text() != source:
        raise SystemExit("policy changed during verification")
    print(version)
    print(f"Lean proved {len(REQUIRED)} theorems about the translated {FUNCTION}")
    print(f"Axioms used (union): {', '.join(sorted({a for v in axioms.values() for a in v})) or 'none'}")
    print(
        f"Replay (not proof): recorded held-out decision {evidence['select_lighter']} and "
        f"{len(rows) - 1} vectors ({selected} selected) agree between Python and Lean"
    )
    if reasons:
        print(f"Recorded rejection follows from proved theorem(s): {', '.join(reasons)}")
    print(f"policy.py SHA256: {hashlib.sha256(source.encode()).hexdigest()}")
    print(f"RuntimeSelection.lean SHA256: {hashlib.sha256(proofs.encode()).hexdigest()}")


if __name__ == "__main__":
    main()
