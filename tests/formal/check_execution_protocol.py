#!/usr/bin/env python3
"""Check the production RPC admission predicate, not a hand-transcribed model.

A deliberately small, fail-closed Clang-AST translator handles this loop-free
integer/Boolean C function. Z3 checks all 32-bit phase/kind and 64-bit ID values.
A separately compiled copy of the exact production function checks representative
classes against the specification. Neither check proves parsing, OS effects,
permission policy, compiler correctness, or the surrounding RPC state machine.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/core/execution_protocol.c"
FUNCTION = "tny_exec_protocol_admit"
CLANG = os.environ.get("TNY_FORMAL_CLANG", "clang")
Z3 = os.environ.get("TNY_FORMAL_Z3", "z3")
ENUMS = (
    "TNY_EXEC_WAIT_START",
    "TNY_EXEC_WAIT_RESULT",
    "TNY_EXEC_WAIT_REPLY",
    "TNY_EXEC_DONE",
    "TNY_EXEC_INVALID",
    "TNY_EXEC_EXECUTE",
    "TNY_EXEC_CONTROL",
    "TNY_EXEC_EVENT",
    "TNY_EXEC_PROMPT",
    "TNY_EXEC_ASK",
    "TNY_EXEC_STATE",
    "TNY_EXEC_RESULT",
)
FLAGS = [
    "-std=c11",
    "-D_DARWIN_C_SOURCE",
    "-Isrc",
    "-Ithird_party",
    "-Ithird_party/yyjson",
]


def run(arguments: list[str], *, input_text: str | None = None) -> str:
    result = subprocess.run(
        arguments,
        cwd=ROOT,
        input=input_text,
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    if result.returncode or result.stderr:
        raise RuntimeError(
            f"{arguments[0]} failed ({result.returncode}): {result.stderr[:3000]}"
        )
    return result.stdout


def json_values(text: str) -> list[dict]:
    decoder = json.JSONDecoder()
    values = []
    index = 0
    while index < len(text):
        if text[index].isspace():
            index += 1
            continue
        value, index = decoder.raw_decode(text, index)
        values.append(value)
    return values


@dataclass(frozen=True)
class Expr:
    text: str
    width: int = 0  # zero denotes a mathematical Boolean
    signed: bool = False

    def boolean(self) -> str:
        return (
            self.text
            if not self.width
            else f"(not (= {self.text} (_ bv0 {self.width})))"
        )

    def cast(self, width: int, signed: bool = False) -> Expr:
        if not width:
            return Expr(self.boolean())
        if not self.width:
            return Expr(
                f"(ite {self.text} (_ bv1 {width}) (_ bv0 {width}))", width, signed
            )
        if width == self.width:
            return Expr(self.text, width, signed)
        if width > self.width:
            operator = "sign_extend" if self.signed else "zero_extend"
            return Expr(
                f"((_ {operator} {width - self.width}) {self.text})", width, signed
            )
        return Expr(f"((_ extract {width - 1} 0) {self.text})", width, signed)


def c_type(node: dict) -> tuple[int, bool]:
    info = node["type"]
    name = info.get("desugaredQualType", info["qualType"])
    if name in ("bool", "_Bool"):
        return 0, False
    if name == "int":
        return 32, True
    if name in ("unsigned int", "tny_exec_phase", "tny_exec_kind"):
        return 32, False
    if name in ("unsigned long long", "unsigned long"):
        return 64, False
    raise ValueError(f"unhandled C type: {name}")


class Translator:
    def __init__(self, constants: dict[str, int]):
        self.constants = constants

    def expression(self, node: dict) -> Expr:
        kind = node["kind"]
        children = node.get("inner", [])
        width, signed = c_type(node)
        if kind == "IntegerLiteral":
            value = int(node["value"])
            return Expr(f"(_ bv{value % (1 << width)} {width})", width, signed)
        if kind == "DeclRefExpr":
            declaration = node["referencedDecl"]
            name = declaration["name"]
            if declaration["kind"] == "ParmVarDecl" and name in (
                "phase",
                "kind",
                "id",
                "expected",
            ):
                return Expr(name, width, signed)
            if declaration["kind"] == "EnumConstantDecl" and name in self.constants:
                return Expr(f"(_ bv{self.constants[name]} {width})", width, signed)
            raise ValueError(f"unhandled reference: {name}")
        if kind in ("ImplicitCastExpr", "ParenExpr"):
            if len(children) != 1:
                raise ValueError("invalid cast shape")
            if kind == "ImplicitCastExpr" and node["castKind"] not in (
                "IntegralCast",
                "IntegralToBoolean",
                "LValueToRValue",
            ):
                raise ValueError(f"unhandled cast: {node['castKind']}")
            return self.expression(children[0]).cast(width, signed)
        if kind == "UnaryOperator" and node["opcode"] == "!" and len(children) == 1:
            return Expr(f"(not {self.expression(children[0]).boolean()})").cast(
                width, signed
            )
        if kind == "BinaryOperator" and len(children) == 2:
            left, right = (self.expression(c) for c in children)
            operator = node["opcode"]
            if operator in ("&&", "||"):
                logical = "and" if operator == "&&" else "or"
                term = f"({logical} {left.boolean()} {right.boolean()})"
            else:
                if left.width != right.width or left.signed != right.signed:
                    raise ValueError(
                        "Clang must make usual arithmetic conversions explicit"
                    )
                if operator in ("==", "!="):
                    term = f"(= {left.text} {right.text})"
                    if operator == "!=":
                        term = f"(not {term})"
                elif operator in ("<", "<=", ">", ">=") and left.width:
                    suffix = {"<": "lt", "<=": "le", ">": "gt", ">=": "ge"}[operator]
                    comparison = "bv" + ("s" if left.signed else "u") + suffix
                    term = f"({comparison} {left.text} {right.text})"
                else:
                    raise ValueError(f"unhandled binary operation: {operator}")
            return Expr(term).cast(width, signed)
        raise ValueError(f"unhandled AST expression: {kind}")

    def statements(self, nodes: list[dict], continuation: str | None = None) -> str:
        result = continuation
        for node in reversed(nodes):
            kind = node["kind"]
            inner = node.get("inner", [])
            if kind == "ReturnStmt" and len(inner) == 1:
                result = self.expression(inner[0]).boolean()
            elif kind == "IfStmt" and len(inner) in (2, 3):
                if result is None and len(inner) == 2:
                    raise ValueError("uncovered fallthrough")
                condition = self.expression(inner[0]).boolean()
                yes = self.statements([inner[1]], result)
                no = self.statements([inner[2]], result) if len(inner) == 3 else result
                result = f"(ite {condition} {yes} {no})"
            elif kind == "CompoundStmt":
                result = self.statements(inner, result)
            else:
                raise ValueError(f"unhandled AST statement: {kind}")
        if result is None:
            raise ValueError("predicate has no total return")
        return result


def compiled_check(function_source: str, directory: Path) -> dict[str, int]:
    # Real compiler evaluates enum values and compares the exact production
    # function against an independently written allowed-case specification.
    constants = "\n".join(
        f'printf("{name}=%u\\n", (unsigned){name});' for name in ENUMS
    )
    probe = directory / "predicate.c"
    probe.write_text(
        """#include "core/execution_protocol.h"
#include <stdio.h>
#include <limits.h>
_Static_assert(sizeof(int) * CHAR_BIT == 32, "32-bit int required");
_Static_assert(sizeof(tny_exec_phase) * CHAR_BIT == 32, "32-bit enum required");
_Static_assert(sizeof(tny_exec_kind) * CHAR_BIT == 32, "32-bit enum required");
_Static_assert(sizeof(uint64_t) * CHAR_BIT == 64, "64-bit id required");
"""
        + function_source
        + """
static bool specification(unsigned p, unsigned k, uint64_t id, uint64_t expected) {
    if (!id || !expected) return false;
    switch (p) {
    case TNY_EXEC_WAIT_START:
        return k == TNY_EXEC_EXECUTE && id == 1 && expected == 1;
    case TNY_EXEC_WAIT_REPLY:
        return k == TNY_EXEC_RESULT && id == expected;
    case TNY_EXEC_WAIT_RESULT:
        if (k == TNY_EXEC_RESULT) return id == 1;
        if (k != TNY_EXEC_CONTROL && k != TNY_EXEC_EVENT && k != TNY_EXEC_PROMPT &&
            k != TNY_EXEC_ASK && k != TNY_EXEC_STATE) return false;
        return expected >= 2 && id == expected;
    default: return false;
    }
}
int main(void) {
"""
        + constants
        + """
    const uint64_t ids[] = {0, 1, 2, 3, 4, 4294967295ULL, 4294967296ULL,
                           9223372036854775807ULL, 9223372036854775808ULL, UINT64_MAX};
    const unsigned states[] = {0, 1, 2, 3, 4, 2147483647U, 2147483648U, UINT_MAX};
    const unsigned kinds[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 2147483647U, 2147483648U, UINT_MAX};
    size_t count = 0;
    for (size_t p = 0; p < sizeof states / sizeof *states; ++p)
    for (size_t k = 0; k < sizeof kinds / sizeof *kinds; ++k)
    for (size_t i = 0; i < sizeof ids / sizeof *ids; ++i)
    for (size_t e = 0; e < sizeof ids / sizeof *ids; ++e) {
        if (tny_exec_protocol_admit((tny_exec_phase)states[p], (tny_exec_kind)kinds[k], ids[i], ids[e])
            != specification(states[p], kinds[k], ids[i], ids[e])) return 2;
        ++count;
    }
    printf("compiled_cases=%zu\\n", count);
    return 0;
}
"""
    )
    binary = directory / "predicate"
    run([CLANG, *FLAGS, "-Wall", "-Wextra", "-Werror", str(probe), "-o", str(binary)])
    values = dict(line.split("=", 1) for line in run([str(binary)]).splitlines())
    if set(values) != set(ENUMS) | {"compiled_cases"}:
        raise ValueError("unexpected compiler probe output")
    print(
        f"Production predicate: {values.pop('compiled_cases')} compiled representative cases passed"
    )
    return {name: int(value) for name, value in values.items()}


def main(source_path: Path = SOURCE) -> None:
    source = source_path.read_bytes()
    ast_text = run(
        [
            CLANG,
            *FLAGS,
            "-Xclang",
            "-ast-dump=json",
            "-Xclang",
            "-ast-dump-filter",
            "-Xclang",
            FUNCTION,
            "-fsyntax-only",
            str(source_path),
        ]
    )
    definitions = [
        n
        for n in json_values(ast_text)
        if n.get("name") == FUNCTION
        and any(c["kind"] == "CompoundStmt" for c in n.get("inner", []))
    ]
    if len(definitions) != 1:
        raise ValueError("expected exactly one predicate definition")
    definition = definitions[0]
    parameters = [n for n in definition["inner"] if n["kind"] == "ParmVarDecl"]
    if [(n["name"], c_type(n)[0]) for n in parameters] != [
        ("phase", 32),
        ("kind", 32),
        ("id", 64),
        ("expected", 64),
    ]:
        raise ValueError("unexpected predicate signature")
    begin, end = definition["range"]["begin"], definition["range"]["end"]
    begin = begin.get("expansionLoc", begin)
    end = end.get("expansionLoc", end)
    function_source = source[begin["offset"] : end["offset"] + end["tokLen"]].decode()
    with tempfile.TemporaryDirectory(prefix="tny-protocol-proof-") as temporary:
        constants = compiled_check(function_source, Path(temporary))
        translator = Translator(constants)
        body = next(n for n in definition["inner"] if n["kind"] == "CompoundStmt")
        term = translator.statements(body["inner"])

        def enum(name: str) -> str:
            return f"(_ bv{constants['TNY_EXEC_' + name]} 32)"

        start = f"(= phase {enum('WAIT_START')})"
        waiting = f"(= phase {enum('WAIT_RESULT')})"
        reply = f"(= phase {enum('WAIT_REPLY')})"
        result = f"(= kind {enum('RESULT')})"
        execute = f"(= kind {enum('EXECUTE')})"
        callback = (
            "(or "
            + " ".join(
                f"(= kind {enum(k)})"
                for k in ("CONTROL", "EVENT", "PROMPT", "ASK", "STATE")
            )
            + ")"
        )
        nonzero = "(and (not (= id (_ bv0 64))) (not (= expected (_ bv0 64))))"
        first = "(and (= id (_ bv1 64)) (= expected (_ bv1 64)))"
        correlated = "(and (= id expected) (bvuge expected (_ bv2 64)))"
        specification = (
            f"(and {nonzero} (or (and {start} {execute} {first}) "
            f"(and {reply} {result} (= id expected)) "
            f"(and {waiting} (or (and {result} (= id (_ bv1 64))) (and {callback} {correlated})))))"
        )
        obligations = {
            "complete allowed-case equivalence": f"(not (= admit {specification}))",
            "zero identifiers never authorize dispatch": f"(and admit (not {nonzero}))",
            "terminal or invalid phases deny": f"(and admit (not (or {start} {waiting} {reply})))",
            "execute only at initial sequence": f"(and admit {execute} (not (and {start} {first})))",
            "nested replies require exact correlation": f"(and admit {reply} (not (and {result} (= id expected))))",
            "callbacks cannot borrow outer result identity": f"(and admit {callback} (not (and {waiting} {correlated})))",
            "outer results cannot use callback identity": f"(and admit {waiting} {result} (not (= id (_ bv1 64))))",
        }
        prelude = (
            "(set-logic QF_BV)\n(declare-const phase (_ BitVec 32))\n"
            "(declare-const kind (_ BitVec 32))\n(declare-const id (_ BitVec 64))\n"
            f"(declare-const expected (_ BitVec 64))\n(define-fun admit () Bool {term})\n"
        )
        queries = "".join(
            f"(push 1)\n(assert {bad})\n(check-sat)\n(pop 1)\n"
            for bad in obligations.values()
        )
        # A valid initial request must have a witness. Prevents vacuous success.
        queries += f"(assert (and admit {start} {execute} {first}))\n(check-sat)\n"
        proof = Path(temporary) / "predicate.smt2"
        proof.write_text(prelude + queries)
        actual = run([Z3, str(proof)]).splitlines()
        if actual != ["unsat"] * len(obligations) + ["sat"]:
            raise RuntimeError(f"proof failed: {actual!r}")
        if source_path.read_bytes() != source:
            raise RuntimeError(
                "source changed during verification; rerun on stable inputs"
            )
        for name in obligations:
            print(f"Proved for all 192 input bits: {name}")
        print("Non-vacuity witness: satisfiable")
        print("Predicate SHA256:", hashlib.sha256(function_source.encode()).hexdigest())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=SOURCE,
        help="alternate source for verifier mutation tests",
    )
    main(parser.parse_args().source.resolve())
