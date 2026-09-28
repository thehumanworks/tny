#!/usr/bin/env python3
"""Prove the production Python code-cell gates from their actual C source.

A small, fail-closed translator turns the Clang typed AST of every function
defined in src/core/code_policy.c into Lean 4 definitions over exact C types:
`_Bool` -> Bool, N-bit integers -> `BitVec N` with signed/unsigned comparison
chosen from the C type, and unsigned `+ - *` as wrapping BitVec operations.
Clang's implicit conversions are translated, not assumed. Anything outside
the whitelist (loops, calls, locals, pointers, enums, division, shifts, signed
arithmetic, narrowing) is an error.

For every unsigned `+ - *` the translator also emits a Lean obligation that
it cannot wrap on any path C evaluates it, using the `&&`/`||`/`?:`/`if`
short-circuit path condition. The hand-written independent specifications and
theorems in tests/formal/python_code_mode/CodePolicy.lean are then checked by
the pinned Lean kernel against the generated definitions.

A separate cross-check (not the proof) compiles the exact production file
with gcc and clang under UBSan, evaluates boundary vectors derived from the
source constants, and has Lean's kernel evaluate the generated definitions on
the same vectors. Trusted base: Clang's parser/type checker, this translator,
the Lean 4 kernel and its standard axioms, and the target ABI whose widths the
compiler probe asserts. Not covered: callers, the interpreter, the protocol
state machine outside these predicates, compiler code generation.
"""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = ROOT / "tests/formal/python_code_mode"
PROOFS = HERE / "CodePolicy.lean"
TOOLCHAIN = HERE / "lean-toolchain"
POLICY = "core/code_policy.c"
HEADERS = ("core/code_policy.h", "core/code_runtime.h")
EXPECTED = (
    "tny_code_timeout_admit",
    "tny_code_source_admit",
    "tny_code_call_admit",
    "tny_code_result_admit",
    "tny_code_output_admit",
    "tny_code_output_take",
    "tny_code_utf8_lead_width",
    "tny_code_memory_admit",
    "tny_code_frame_admit",
)
# Theorems the proof file must contain; each one's axioms are printed.
REQUIRED = (
    "timeout_admit_iff",
    "timeout_boundaries",
    "source_admit_iff",
    "source_boundaries",
    "call_admit_iff",
    "call_boundaries",
    "no_call_after_budget",
    "every_budgeted_call_admitted",
    "result_admit_iff",
    "result_boundaries",
    "result_fits_frame",
    "output_admit_iff",
    "output_admitted_sum_exact",
    "output_boundaries",
    "output_wrap_rejected",
    "memory_admit_iff",
    "memory_admitted_sum_exact",
    "memory_boundaries",
    "frame_admit_iff",
    "frame_requires_running",
    "no_terminal_phase_frames",
    "frame_only_call_or_done",
    "frame_nonempty",
    "no_call_frame_after_budget",
    "frame_boundaries",
    "admitted_call_fits_call_frame",
    "admitted_output_fits_done_frame",
    "output_take_eq",
    "output_take_le_available",
    "output_take_fits_head",
    "output_take_all_iff_admit",
    "output_take_boundaries",
    "utf8_lead_width_exact",
    "utf8_lead_width_bounded",
    "utf8_invalid_leads_rejected",
    "utf8_lead_width_boundaries",
    "nonvacuity",
)
STANDARD_AXIOMS = {"propext", "Classical.choice", "Quot.sound"}
FORBIDDEN = re.compile(
    r"\b(sorry|admit|axiom|native_decide|bv_decide|implemented_by|extern|unsafe|"
    r"opaque|decreasing_by|partial|ofReduceBool|debug\.skipKernelTC)\b|#exit|"
    r"\bnamespace\s+TnyC\b"
)
# (bits, signed) per C type spelling; the compiler probe asserts each entry.
C_TYPES = {
    "_Bool": (1, False),
    "bool": (1, False),  # Clang may print _Bool as the <stdbool.h> spelling
    "int": (32, True),
    "unsigned int": (32, False),
    "long": (64, True),
    "unsigned long": (64, False),
    "long long": (64, True),
    "unsigned long long": (64, False),
}
CFLAGS = ["-std=c11", "-D_DARWIN_C_SOURCE"]


class Unsupported(ValueError):
    """The production source left the translator's verified whitelist."""


def run(arguments: list[str], *, cwd: Path = ROOT, timeout: int = 120) -> str:
    result = subprocess.run(
        arguments, cwd=cwd, capture_output=True, text=True, timeout=timeout, check=False
    )
    if result.returncode or result.stderr.strip():
        raise RuntimeError(
            f"{Path(arguments[0]).name} failed ({result.returncode}):\n"
            f"{result.stdout[-4000:]}{result.stderr[-4000:]}"
        )
    return result.stdout


def c_type(node: dict) -> tuple[int, bool]:
    info = node.get("type", {})
    name = info.get("desugaredQualType", info.get("qualType"))
    if name not in C_TYPES:
        raise Unsupported(f"unsupported C type: {name!r}")
    return C_TYPES[name]


def lean_type(ctype: tuple[int, bool]) -> str:
    return "Bool" if ctype[0] == 1 else f"(BitVec {ctype[0]})"


def lean_name(name: str) -> str:
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        raise Unsupported(f"unsupported identifier: {name!r}")
    return f"«{name}»"


@dataclass(frozen=True)
class Expr:
    """A C value. `logical` means `text` is a Lean Bool b with C value b?1:0."""

    text: str
    ctype: tuple[int, bool]
    logical: bool
    constant: int | None = None  # value when parameter-free, for vector choice
    parameter: str | None = None  # (converted) parameter, for vector choice

    def boolean(self) -> str:
        if self.logical:
            return self.text
        return f"(decide ({self.text} ≠ 0#{self.ctype[0]}))"

    def bits(self) -> str:
        width = self.ctype[0]
        if width == 1:
            raise Unsupported("_Bool used as an integer operand")
        if self.logical:
            return f"(bif {self.text} then 1#{width} else 0#{width})"
        return self.text


@dataclass
class Obligation:
    function: str
    operator: str
    left: str
    right: str
    width: int
    path: list[tuple[str, bool]]


@dataclass
class Translator:
    function: str
    parameters: dict[str, tuple[int, bool]]
    obligations: list[Obligation] = field(default_factory=list)
    constants: set[int] = field(default_factory=set)
    # Constants each parameter is directly compared with, for vector choice.
    related: dict[str, set[int]] = field(default_factory=dict)

    def expression(self, node: dict, path: list[tuple[str, bool]]) -> Expr:
        kind = node["kind"]
        inner = node.get("inner", [])
        if kind == "ParenExpr":
            if len(inner) != 1:
                raise Unsupported("malformed parenthesis")
            return self.expression(inner[0], path)
        ctype = c_type(node)
        if kind == "IntegerLiteral":
            value = int(node["value"])
            width, signed = ctype
            limit = 1 << (width - 1 if signed else width)
            if width == 1 or not 0 <= value < limit:
                raise Unsupported(f"literal {value} outside its C type")
            self.constants.add(value)
            return Expr(f"({value}#{width})", ctype, False, value)
        if kind == "DeclRefExpr":
            declaration = node.get("referencedDecl", {})
            name = declaration.get("name")
            if declaration.get("kind") != "ParmVarDecl" or name not in self.parameters:
                raise Unsupported(f"reference to non-parameter {name!r}")
            if self.parameters[name] != ctype:
                raise Unsupported("parameter reference changed type")
            return Expr(lean_name(name), ctype, ctype[0] == 1, None, name)
        if kind in ("ImplicitCastExpr", "CStyleCastExpr"):
            if len(inner) != 1:
                raise Unsupported("malformed cast")
            cast = node.get("castKind")
            if cast == "LValueToRValue":
                if inner[0]["kind"] != "DeclRefExpr":
                    raise Unsupported("load from something other than a parameter")
                return self.expression(inner[0], path)
            source = self.expression(inner[0], path)
            if cast == "IntegralToBoolean":
                if ctype[0] != 1:
                    raise Unsupported("IntegralToBoolean to a non-_Bool type")
                return Expr(source.boolean(), ctype, True)
            if cast == "IntegralCast":
                return self.convert(source, ctype)
            raise Unsupported(f"unsupported cast kind: {cast}")
        if kind == "UnaryOperator":
            if node.get("opcode") != "!" or len(inner) != 1 or node.get("isPostfix"):
                raise Unsupported(f"unsupported unary operator: {node.get('opcode')}")
            operand = self.expression(inner[0], path)
            return self.logical(f"(!{operand.boolean()})", ctype)
        if kind == "BinaryOperator":
            return self.binary(node, ctype, path)
        if kind == "ConditionalOperator":
            if len(inner) != 3:
                raise Unsupported("malformed conditional")
            condition = self.expression(inner[0], path).boolean()
            yes = self.expression(inner[1], [*path, (condition, True)])
            no = self.expression(inner[2], [*path, (condition, False)])
            if yes.ctype != ctype or no.ctype != ctype:
                raise Unsupported("conditional branches need explicit conversions")
            if ctype[0] == 1:
                return Expr(
                    f"(bif {condition} then {yes.boolean()} else {no.boolean()})",
                    ctype,
                    True,
                )
            return Expr(
                f"(bif {condition} then {yes.bits()} else {no.bits()})", ctype, False
            )
        raise Unsupported(f"unsupported expression: {kind}")

    def logical(self, text: str, ctype: tuple[int, bool]) -> Expr:
        # C logical and relational operators yield int 0 or 1.
        if ctype != (32, True):
            raise Unsupported("logical result is not int")
        return Expr(text, ctype, True)

    def convert(self, source: Expr, target: tuple[int, bool]) -> Expr:
        width, signed = target
        if width == 1:
            raise Unsupported("IntegralCast to _Bool")
        if source.logical:  # 0 and 1 are representable in every integer type
            return Expr(source.text, target, True)
        old_width, old_signed = source.ctype
        value = source.constant
        if old_width == width and old_signed == signed:
            return source
        if signed and not old_signed and old_width >= width:
            raise Unsupported("unsigned to signed conversion may be out of range")
        if width < old_width:
            raise Unsupported("narrowing integer conversion")
        if value is not None and not signed:
            value %= 1 << width
        if width == old_width:  # signed -> unsigned of the same width: modulo 2^N
            return Expr(source.text, target, False, value, source.parameter)
        extend = "signExtend" if old_signed else "zeroExtend"
        text = f"(BitVec.{extend} {width} {source.text})"
        return Expr(text, target, False, value, source.parameter)

    def binary(self, node: dict, ctype: tuple[int, bool], path: list) -> Expr:
        inner = node.get("inner", [])
        if len(inner) != 2:
            raise Unsupported("malformed binary operator")
        operator = node.get("opcode")
        left = self.expression(inner[0], path)
        if operator in ("&&", "||"):
            guard = (left.boolean(), operator == "&&")
            right = self.expression(inner[1], [*path, guard])
            lean = "&&" if operator == "&&" else "||"
            return self.logical(f"({left.boolean()} {lean} {right.boolean()})", ctype)
        right = self.expression(inner[1], path)
        if left.ctype != right.ctype or left.ctype[0] == 1:
            raise Unsupported("Clang must make usual arithmetic conversions explicit")
        width, signed = left.ctype
        a, b = left.bits(), right.bits()
        for one, other in ((left, right), (right, left)):
            if one.parameter and other.constant is not None:
                self.related.setdefault(one.parameter, set()).add(other.constant)
        if operator in ("==", "!="):
            test = f"(decide ({a} = {b}))"
            return self.logical(test if operator == "==" else f"(!{test})", ctype)
        if operator in ("<", "<=", ">", ">="):
            if operator in (">", ">="):
                a, b = b, a
            prefix = "s" if signed else "u"
            suffix = "lt" if operator in ("<", ">") else "le"
            return self.logical(f"(BitVec.{prefix}{suffix} {a} {b})", ctype)
        if operator in ("+", "-", "*"):
            if signed:
                raise Unsupported("signed arithmetic can overflow (undefined behavior)")
            if ctype != left.ctype:
                raise Unsupported("arithmetic result type differs from operands")
            self.obligations.append(
                Obligation(self.function, operator, a, b, width, list(path))
            )
            value = None
            if left.constant is not None and right.constant is not None:
                value = {
                    "+": left.constant + right.constant,
                    "-": left.constant - right.constant,
                    "*": left.constant * right.constant,
                }[operator] % (1 << width)
                self.constants.add(value)
            return Expr(f"({a} {operator} {b})", ctype, False, value)
        raise Unsupported(f"unsupported binary operator: {operator}")

    def statements(
        self, nodes: list[dict], path: list, result: tuple[int, bool]
    ) -> str:
        if not nodes:
            raise Unsupported("control can reach the end of a non-void function")
        node, rest = nodes[0], nodes[1:]
        inner = node.get("inner", [])
        if node["kind"] == "CompoundStmt":
            return self.statements(inner + rest, path, result)
        if node["kind"] == "ReturnStmt":
            if len(inner) != 1:
                raise Unsupported("return without a value")
            value = self.expression(inner[0], path)
            if value.ctype != result:
                raise Unsupported("return needs an explicit conversion")
            return value.boolean() if result[0] == 1 else value.bits()
        if (
            node["kind"] == "IfStmt"
            and len(inner) in (2, 3)
            and not node.get("hasInit")
        ):
            if node.get("hasVar") or node.get("isConstexpr"):
                raise Unsupported("unsupported if form")
            condition = self.expression(inner[0], path).boolean()
            yes = self.statements([inner[1], *rest], [*path, (condition, True)], result)
            otherwise = [inner[2], *rest] if len(inner) == 3 else rest
            no = self.statements(otherwise, [*path, (condition, False)], result)
            return f"(bif {condition} then {yes} else {no})"
        raise Unsupported(f"unsupported statement: {node['kind']}")


@dataclass
class Function:
    name: str
    parameters: list[tuple[str, tuple[int, bool]]]
    result: tuple[int, bool]
    body: str
    obligations: list[Obligation]
    constants: set[int]
    related: dict[str, set[int]]
    source: str


def parse_json_stream(text: str) -> list[dict]:
    decoder = json.JSONDecoder()
    values, index = [], 0
    while index < len(text):
        if text[index].isspace():
            index += 1
            continue
        value, index = decoder.raw_decode(text, index)
        values.append(value)
    return values


def translate(clang: str, src: Path) -> list[Function]:
    path = src / POLICY
    raw = path.read_bytes()
    dump = run(
        [
            clang,
            *CFLAGS,
            f"-I{src}",
            "-Xclang",
            "-ast-dump=json",
            "-fsyntax-only",
            str(path),
        ]
    )
    unit = json.loads(dump)
    definitions = [
        n
        for n in unit.get("inner", [])
        if n.get("kind") == "FunctionDecl"
        and any(c.get("kind") == "CompoundStmt" for c in n.get("inner", []))
    ]
    names = [n["name"] for n in definitions]
    if sorted(names) != sorted(EXPECTED):
        raise Unsupported(
            f"translation unit must define exactly {sorted(EXPECTED)}, found {sorted(names)}"
        )
    functions = []
    for node in sorted(definitions, key=lambda n: EXPECTED.index(n["name"])):
        name = node["name"]
        if (
            node.get("storageClass")
            or node.get("inline")
            or node.get("variadic")
            or "(" not in node["type"]["qualType"]
        ):
            raise Unsupported(f"{name}: only plain external definitions are supported")
        result_spelling = node["type"]["qualType"].split("(", 1)[0].strip()
        typedefs = {"uint64_t": "unsigned long", "int64_t": "long", "bool": "_Bool"}
        result = C_TYPES.get(typedefs.get(result_spelling, result_spelling))
        if result is None or result[0] not in (1, 32, 64):
            raise Unsupported(f"{name}: unsupported return type {result_spelling!r}")
        parameters = []
        body = None
        for child in node["inner"]:
            if child["kind"] == "ParmVarDecl":
                parameters.append((child["name"], c_type(child)))
                lean_name(child["name"])
            elif child["kind"] == "CompoundStmt" and body is None:
                body = child
            else:
                raise Unsupported(
                    f"{name}: unsupported declaration part {child['kind']}"
                )
        if len({p for p, _ in parameters}) != len(parameters):
            raise Unsupported(f"{name}: duplicate parameters")
        translator = Translator(name, dict(parameters))
        text = translator.statements(body["inner"], [], result)
        begin, end = node["range"]["begin"], node["range"]["end"]
        begin, end = begin.get("expansionLoc", begin), end.get("expansionLoc", end)
        source = raw[begin["offset"] : end["offset"] + end["tokLen"]].decode()
        functions.append(
            Function(
                name,
                parameters,
                result,
                text,
                translator.obligations,
                translator.constants,
                translator.related,
                source,
            )
        )
    return functions


def binders(function: Function) -> str:
    return " ".join(
        f"({lean_name(p)} : {lean_type(t)})" for p, t in function.parameters
    )


def definitions(functions: list[Function]) -> str:
    lines = [
        "-- GENERATED by tests/formal/check_code_policy.py from the Clang AST of",
        "-- src/core/code_policy.c. Do not edit; do not commit.",
        "namespace TnyC",
    ]
    for f in functions:
        lines.append(
            f"def {f.name} {binders(f)} : {lean_type(f.result)} :=\n  {f.body}\n"
        )
    lines.append("end TnyC\n")
    return "\n".join(lines)


def obligations(functions: list[Function]) -> tuple[str, list[str]]:
    lines = [
        "namespace TnyC.NoWrap",
        "open TnyC",
        "set_option linter.unusedVariables false",
    ]
    names = []
    for f in functions:
        for index, o in enumerate(f.obligations):
            name = f"{f.name}_{index}"
            names.append(f"TnyC.NoWrap.{name}")
            hypotheses = "".join(
                f"{condition} = {'true' if taken else 'false'} → "
                for condition, taken in o.path
            )
            left, right = f"({o.left}).toNat", f"({o.right}).toNat"
            claim = {
                "-": f"{right} ≤ {left}",
                "+": f"{left} + {right} < 2 ^ {o.width}",
                "*": f"{left} * {right} < 2 ^ {o.width}",
            }[o.operator]
            lines.append(
                f"-- `{o.operator}` in {f.name}: exact on every path C evaluates it\n"
                f"theorem {name} : ∀ {binders(f)}, {hypotheses}{claim} := by tny_no_wrap"
            )
    lines.append("end TnyC.NoWrap\n")
    return "\n".join(lines), names


# ---- Compiled cross-check of the exact production file (not the proof) ----


def samples(ctype: tuple[int, bool], constants: set[int]) -> list[int]:
    width, signed = ctype
    if width == 1:
        return [0, 1]
    low, high = (
        (-(1 << (width - 1)), (1 << (width - 1)) - 1)
        if signed
        else (0, (1 << width) - 1)
    )
    values = {low, high, 0, 1, 2} | ({-1} if signed else set())
    for c in constants:
        values.update((c - 1, c, c + 1))
    return sorted(v for v in values if low <= v <= high)


def vectors(function: Function) -> list[tuple[int, ...]]:
    # Type edges plus both sides of every constant a parameter is compared
    # with; parameters compared only with other parameters get all constants.
    domains = [
        samples(t, function.related.get(p, function.constants) if t[0] != 1 else set())
        for p, t in function.parameters
    ]
    return list(itertools.product(*domains))


def c_literal(value: int, ctype: tuple[int, bool]) -> str:
    width, signed = ctype
    if width == 1:
        return "true" if value else "false"
    if signed:
        if value == -(1 << (width - 1)):
            return f"(-{(1 << (width - 1)) - 1}LL - 1)"
        return f"{value}LL"
    return f"{value}ULL"


def c_type_name(ctype: tuple[int, bool]) -> str:
    width, signed = ctype
    return "bool" if width == 1 else f"{'' if signed else 'u'}int{width}_t"


def compiled_results(
    compilers: list[str], src: Path, functions: list[Function], work: Path
) -> dict[str, list[tuple[tuple[int, ...], int]]]:
    probe = work / "vectors.c"
    body = ["#include <inttypes.h>", "#include <stdio.h>", f'#include "{POLICY}"']
    main = ["int main(void) {"]
    for f in functions:
        rows = vectors(f)
        for column, (_, ctype) in enumerate(f.parameters):
            values = ", ".join(c_literal(r[column], ctype) for r in rows)
            body.append(
                f"static const {c_type_name(ctype)} {f.name}_{column}[] = {{{values}}};"
            )
        arguments = ", ".join(f"{f.name}_{c}[i]" for c in range(len(f.parameters)))
        # Unsigned results print as unsigned: no implementation-defined cast.
        form, cast = (
            ("%llu", "unsigned long long") if not f.result[1] else ("%lld", "long long")
        )
        main.append(
            f"    for (size_t i = 0; i < {len(rows)}; ++i)\n"
            f'        printf("{form}\\n", ({cast}){f.name}({arguments}));'
        )
    main.append("    return 0;\n}\n")
    probe.write_text("\n".join(body + main))
    outputs = []
    for compiler in compilers:
        binary = work / f"vectors-{Path(compiler).name}"
        sanitizers = "undefined"
        if "clang" in Path(compiler).name:
            sanitizers += ",unsigned-integer-overflow,implicit-conversion"
        run(
            [
                compiler,
                *CFLAGS,
                f"-I{src}",
                "-O1",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-fsanitize={sanitizers}",
                "-fno-sanitize-recover=all",
                str(probe),
                "-o",
                str(binary),
            ],
            timeout=300,
        )
        outputs.append(run([str(binary)]).split())
    if any(o != outputs[0] for o in outputs):
        raise RuntimeError("compilers disagree on the production gates")
    results, position = {}, 0
    for f in functions:
        rows = []
        for vector in vectors(f):
            rows.append((vector, int(outputs[0][position])))
            position += 1
        results[f.name] = rows
    if position != len(outputs[0]):
        raise RuntimeError("unexpected vector output length")
    return results


def lean_value(value: int, ctype: tuple[int, bool]) -> str:
    width, _ = ctype
    if width == 1:
        return "true" if value else "false"
    return f"({value % (1 << width)}#{width})"


def cross_check(functions: list[Function], results: dict) -> str:
    lines = ["namespace TnyC.Vectors", "open TnyC"]
    for f in functions:
        rows = results[f.name]
        for start in range(0, len(rows), 100):
            checks = []
            for vector, result in rows[start : start + 100]:
                arguments = " ".join(
                    lean_value(v, t)
                    for v, (_, t) in zip(vector, f.parameters, strict=True)
                )
                checks.append(f"{f.name} {arguments} == {lean_value(result, f.result)}")
            lines.append(
                f"set_option maxRecDepth 8192 in\nexample : [{', '.join(checks)}].all id = true := by decide"
            )
    lines.append("end TnyC.Vectors\n")
    return "\n".join(lines)


def width_probe(compilers: list[str], src: Path, work: Path) -> None:
    probe = work / "widths.c"
    lines = ["#include <limits.h>", "#include <stdbool.h>", "#include <stdint.h>"]
    for name, (width, signed) in C_TYPES.items():
        if width == 1:
            lines.append(f'_Static_assert((({name})2) == 1, "_Bool normalizes");')
            continue
        lines.append(
            f'_Static_assert(sizeof({name}) * CHAR_BIT == {width}, "{name} width");'
        )
        lines.append(
            f'_Static_assert((({name})-1 < 0) == {int(signed)}, "{name} signedness");'
        )
    lines.append(
        '_Static_assert(-1 >> 1 == -1 && (-3) / 2 == -1, "two\'s complement");'
    )
    probe.write_text("\n".join(lines) + "\n")
    for compiler in compilers:
        run([compiler, *CFLAGS, f"-I{src}", "-fsyntax-only", str(probe)])


# A proof-file declaration or notation named like a generated definition could
# shadow it (the current namespace wins over `open`), proving a hand copy.
def shadow_pattern(prefix: str) -> re.Pattern:
    return re.compile(
        r"\b(?:def|abbrev|theorem|lemma|instance|opaque|structure|inductive|class|axiom)"
        rf"\s+[«\w.]*{prefix}"
        r"|\b(?:notation|infix|infixl|infixr|prefix|postfix|macro|macro_rules|syntax|elab"
        rf"|export|renaming)\b[^\n]*{prefix}"
    )


def check_proof_text(text: str) -> None:
    stripped = re.sub(r"--[^\n]*|/-.*?-/", "", text, flags=re.S)
    match = FORBIDDEN.search(stripped) or shadow_pattern("tny_code_").search(stripped)
    if match:
        raise SystemExit(f"forbidden token in proof file: {match.group(0)!r}")


def lean_version(lean: str) -> str:
    version = run([lean, "--version"]).strip()
    pinned = TOOLCHAIN.read_text().strip().rsplit(":v", 1)[-1]
    if f"version {pinned}," not in version:
        raise SystemExit(
            f"Lean {pinned} required by {TOOLCHAIN.name}, found: {version}"
        )
    return version


def check_axioms(output: str, names: list[str]) -> dict[str, list[str]]:
    found = {}
    for line in output.splitlines():
        match = re.match(r"'([^']+)' depends on axioms: \[(.*)\]$", line)
        if match:
            found[match.group(1)] = [a.strip() for a in match.group(2).split(",")]
        match = re.match(r"'([^']+)' does not depend on any axioms$", line)
        if match:
            found[match.group(1)] = []
    missing = [n for n in names if n not in found]
    if missing:
        raise SystemExit(f"missing axiom report for: {missing}")
    for name, axioms in found.items():
        if not set(axioms) <= STANDARD_AXIOMS:
            raise SystemExit(f"{name} depends on non-standard axioms: {axioms}")
    return found


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--src",
        type=Path,
        default=ROOT / "src",
        help="source root (mutation tests only)",
    )
    parser.add_argument("--lean", default=os.environ.get("LEAN", "lean"))
    parser.add_argument("--clang", default=os.environ.get("TNY_FORMAL_CLANG", "clang"))
    parser.add_argument("--gcc", default=os.environ.get("TNY_FORMAL_GCC", "gcc"))
    parser.add_argument(
        "--no-vectors", action="store_true", help="skip the compiled cross-check"
    )
    parser.add_argument(
        "--emit", type=Path, help="also write the checked Lean file here"
    )
    args = parser.parse_args()
    src = args.src.resolve()
    inputs = {name: (src / name).read_bytes() for name in (POLICY, *HEADERS)}
    proofs = PROOFS.read_text()
    check_proof_text(proofs)
    version = lean_version(args.lean)
    compilers = [c for c in (args.clang, args.gcc) if c]
    for compiler in compilers:
        if not shutil.which(compiler):
            raise SystemExit(f"required compiler not found: {compiler}")
    with tempfile.TemporaryDirectory(prefix="tny-code-policy-") as temporary:
        work = Path(temporary)
        width_probe(compilers, src, work)
        try:
            functions = translate(args.clang, src)
        except Unsupported as error:
            print(f"translator: unsupported source: {error}", file=sys.stderr)
            raise SystemExit(3) from error
        generated = definitions(functions)
        no_wrap, obligation_names = obligations(functions)
        vector_text, vector_count = "", 0
        if not args.no_vectors:
            results = compiled_results(compilers, src, functions, work)
            vector_text = cross_check(functions, results)
            vector_count = sum(len(r) for r in results.values())
            vector_detail = ", ".join(
                f"{f.name.removeprefix('tny_code_')} {sum(1 for _, r in results[f.name] if r)}"
                f"/{len(results[f.name])}"
                for f in functions
            )
        declared = re.findall(r"^theorem\s+(\w+)", proofs, re.M)
        extra = [n for n in declared if n not in REQUIRED]
        names = [f"CodePolicy.{n}" for n in (*REQUIRED, *extra)] + obligation_names
        report = "\n".join(f"#print axioms {n}" for n in names)
        checked = "\n".join([generated, proofs, no_wrap, vector_text, report, ""])
        path = work / "CodePolicyChecked.lean"
        path.write_text(checked)
        if args.emit:
            args.emit.write_text(checked)
        result = subprocess.run(
            [args.lean, str(path)],
            capture_output=True,
            text=True,
            timeout=600,
            check=False,
        )
        if result.returncode or "error:" in result.stdout:
            print(result.stdout[-8000:], result.stderr[-4000:])
            print("Lean rejected the proof of the translated production gates")
            raise SystemExit(1)
        axioms = check_axioms(result.stdout, names)
    changed = [n for n in inputs if (src / n).read_bytes() != inputs[n]]
    if changed:
        raise SystemExit(f"inputs changed during verification: {changed}")
    used = sorted({a for n in names for a in axioms[n]})
    groups: dict[str, int] = {}
    for n in names:
        key = ", ".join(sorted(axioms[n])) or "none"
        groups[key] = groups.get(key, 0) + 1
    print(version)
    print(
        f"Translated {len(functions)} production functions: {', '.join(f.name for f in functions)}"
    )
    print(
        f"Lean proved {len(REQUIRED)} specification theorems (+{len(extra)} helper) "
        f"and {len(obligation_names)} generated no-wrap obligations"
    )
    print(f"Axioms used (union over all): {', '.join(used) or 'none'}")
    for key, count in sorted(groups.items()):
        print(f"  {count} theorems depend on axioms: {key}")
    if not args.no_vectors:
        print(
            f"Cross-check (not proof): {vector_count} vectors agree across "
            f"{' and '.join(Path(c).name for c in compilers)} (UBSan) and Lean kernel evaluation"
        )
        print(f"  nonzero results per gate: {vector_detail}")
    for name, data in inputs.items():
        print(f"{name} SHA256: {hashlib.sha256(data).hexdigest()}")
    print(f"CodePolicy.lean SHA256: {hashlib.sha256(proofs.encode()).hexdigest()}")


if __name__ == "__main__":
    main()
