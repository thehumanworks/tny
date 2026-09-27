#!/usr/bin/env python3
"""Translate the production code-cell gates (src/core/code_policy.c) to Lean.

A deliberately small, fail-closed Clang JSON-AST translator accepts only the
loop-free integer/Boolean C these gates use. Unsigned casts and arithmetic are
modeled with explicit modular wrap-around; any construct outside the whitelist
is an error, never skipped. The generated definitions are proved by
tests/formal/code_policy/Proofs.lean with Lean's kernel.

Trusted base: Clang's parser/semantic analysis, this translator, Lean. Not
proved: the interpreter, frame transport, allocator wiring, OS sandbox, or
that callers use these gates (tests and review cover that).

  check_code_policy.py [--lean LEAN] [--source FILE] [--emit FILE]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/core/code_policy.c"
PROOFS = ROOT / "tests/formal/code_policy/Proofs.lean"
CLANG = os.environ.get("TNY_FORMAL_CLANG", "clang")
FUNCTIONS = (
    "tny_code_timeout_admit",
    "tny_code_source_admit",
    "tny_code_call_admit",
    "tny_code_result_admit",
    "tny_code_output_admit",
    "tny_code_memory_admit",
    "tny_code_frame_admit",
    "tny_code_json_kind",
)
# qualType -> (signed, bits); _Bool is handled separately.
INTEGERS = {
    "int": (True, 32),
    "unsigned int": (False, 32),
    "int64_t": (True, 64),
    "long": (True, 64),
    "uint64_t": (False, 64),
    "unsigned long": (False, 64),
    "unsigned long long": (False, 64),
}


class Unsupported(Exception):
    pass


def qual(node: dict) -> str:
    return node.get("type", {}).get("desugaredQualType") or node.get("type", {}).get(
        "qualType", ""
    )


def kind_of(type_name: str) -> str:
    if type_name == "_Bool":
        return "bool"
    if type_name in INTEGERS:
        return "int"
    raise Unsupported(f"type {type_name!r}")


def as_bool(expr: str, kind: str) -> str:
    return expr if kind == "bool" else f"(decide ({expr} ≠ 0))"


def as_int(expr: str, kind: str) -> str:
    return expr if kind == "int" else f"(if {expr} then (1 : Int) else 0)"


def wrap(expr: str, type_name: str) -> str:
    signed, bits = INTEGERS[type_name]
    if signed:
        return expr  # signed overflow is UB in C; these gates perform none (asserted below)
    return f"(({expr}) % {2**bits})"


def translate(node: dict) -> tuple[str, str]:
    k = node.get("kind")
    inner = node.get("inner", [])
    if k in ("ParenExpr",):
        return translate(inner[0])
    if k == "ImplicitCastExpr" or k == "CStyleCastExpr":
        cast = node.get("castKind")
        expr, ek = translate(inner[0])
        target = qual(node)
        if cast in ("LValueToRValue", "NoOp"):
            return expr, ek
        if cast == "IntegralToBoolean":
            return as_bool(expr, ek), "bool"
        if cast == "IntegralCast":
            if target == "_Bool":
                return as_bool(expr, ek), "bool"
            source = qual(inner[0])
            if source == "_Bool" or ek == "bool":
                return as_int(expr, ek), "int"
            s_signed, s_bits = INTEGERS[source]
            t_signed, t_bits = INTEGERS[target]
            if t_signed and (not s_signed and s_bits >= t_bits):
                raise Unsupported(f"implementation-defined cast {source} -> {target}")
            if t_signed and s_signed and s_bits > t_bits:
                raise Unsupported(f"narrowing cast {source} -> {target}")
            return (wrap(expr, target) if not t_signed else expr), "int"
        raise Unsupported(f"cast {cast}")
    if k == "IntegerLiteral":
        return f"({int(node['value'])} : Int)", "int"
    if k == "CXXBoolLiteralExpr":
        return ("true" if node.get("value") else "false"), "bool"
    if k == "DeclRefExpr":
        ref = node.get("referencedDecl", {})
        if ref.get("kind") != "ParmVarDecl":
            raise Unsupported(f"reference to {ref.get('kind')}")
        return ref["name"], kind_of(qual(node))
    if k == "UnaryOperator":
        op = node.get("opcode")
        expr, ek = translate(inner[0])
        if op == "!":
            return f"(!{as_bool(expr, ek)})", "bool"
        raise Unsupported(f"unary {op}")
    if k == "BinaryOperator":
        op = node.get("opcode")
        (a, ak), (b, bk) = translate(inner[0]), translate(inner[1])
        if op in ("&&", "||"):
            return f"({as_bool(a, ak)} {op} {as_bool(b, bk)})", "bool"
        lean_cmp = {"<": "<", "<=": "≤", ">": ">", ">=": "≥", "==": "=", "!=": "≠"}
        if op in lean_cmp:
            if ak == "bool" and bk == "bool":
                return f"(decide ({a} {lean_cmp[op]} {b}))", "bool"
            return f"(decide ({as_int(a, ak)} {lean_cmp[op]} {as_int(b, bk)}))", "bool"
        if op in ("+", "-", "*"):
            result = qual(node)
            if INTEGERS[result][0]:
                raise Unsupported(f"signed arithmetic {op} (overflow not modeled)")
            return wrap(f"{as_int(a, ak)} {op} {as_int(b, bk)}", result), "int"
        raise Unsupported(f"binary {op}")
    if k == "ConditionalOperator":
        (c, ck), (t, tk), (e, ek) = (translate(x) for x in inner)
        if tk != ek:
            raise Unsupported("mixed conditional branches")
        return f"(if {as_bool(c, ck)} then {t} else {e})", tk
    raise Unsupported(f"node {k}")


def ast(source: Path) -> list[dict]:
    command = [
        CLANG,
        "-Xclang",
        "-ast-dump=json",
        "-fsyntax-only",
        "-std=c11",
        f"-I{ROOT / 'src'}",
        f"-I{ROOT / 'third_party'}",
        str(source),
    ]
    text = subprocess.run(
        command, capture_output=True, text=True, check=True, timeout=60
    ).stdout
    root = json.loads(text)
    return [n for n in root.get("inner", []) if n.get("kind") == "FunctionDecl"]


def lean_for(source: Path) -> str:
    functions = {}
    for decl in ast(source):
        name = decl.get("name")
        body = [c for c in decl.get("inner", []) if c.get("kind") == "CompoundStmt"]
        if name not in FUNCTIONS or not body:
            continue
        statements = body[0].get("inner", [])
        if len(statements) != 1 or statements[0].get("kind") != "ReturnStmt":
            raise Unsupported(f"{name}: body must be a single return")
        params = []
        for p in decl.get("inner", []):
            if p.get("kind") == "ParmVarDecl":
                t = qual(p)
                params.append(
                    f"({p['name']} : {'Bool' if kind_of(t) == 'bool' else 'Int'})"
                )
        result_type = decl["type"]["qualType"].split("(")[0].strip()
        expr, ek = translate(statements[0]["inner"][0])
        lean_type = "Bool" if result_type == "_Bool" else "Int"
        if lean_type == "Bool":
            expr = as_bool(expr, ek)
        elif ek != "int":
            expr = as_int(expr, ek)
        functions[name] = f"def {name} {' '.join(params)} : {lean_type} :=\n  {expr}\n"
    missing = [f for f in FUNCTIONS if f not in functions]
    if missing:
        raise Unsupported(f"missing definitions: {missing}")
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    header = (
        f"-- Generated by tests/formal/check_code_policy.py from {source.name}\n"
        f"-- source sha256 {digest}; do not edit.\n"
        "namespace TnyCodePolicy\n\n"
    )
    return header + "\n".join(functions[f] for f in FUNCTIONS) + "\nend TnyCodePolicy\n"


def check(lean: str, source: Path, emit: Path | None) -> int:
    generated = lean_for(source)
    proofs = PROOFS.read_text()
    with tempfile.TemporaryDirectory(prefix="tny-code-policy-") as tmp:
        combined = Path(tmp) / "CodePolicy.lean"
        combined.write_text(generated + "\n" + proofs)
        if emit:
            emit.write_text(combined.read_text())
        # cwd selects tests/formal/code_policy/lean-toolchain under elan.
        done = subprocess.run(
            [lean, str(combined)],
            capture_output=True,
            text=True,
            timeout=600,
            cwd=PROOFS.parent,
        )
    sys.stdout.write(done.stdout[-4000:])
    sys.stderr.write(done.stderr[-4000:])
    if "sorry" in done.stdout + done.stderr:
        print("error: a proof depends on sorry", file=sys.stderr)
        return 1
    if done.returncode == 0:
        text = (generated + proofs).encode()
        print(
            f"code policy: {len(FUNCTIONS)} source-linked definitions proved; "
            f"combined sha256 {hashlib.sha256(text).hexdigest()}"
        )
    return done.returncode


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lean", default=os.environ.get("LEAN", "lean"))
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--emit", type=Path)
    args = parser.parse_args()
    try:
        raise SystemExit(check(args.lean, args.source, args.emit))
    except Unsupported as error:
        raise SystemExit(
            f"unsupported construct, refusing to check: {error}"
        ) from error


if __name__ == "__main__":
    main()
