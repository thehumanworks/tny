#!/usr/bin/env python3
"""Source mutants must fail at the expected stage of the real checkers.

`lean` mutants are accepted by the translator and must be rejected by Lean
theorem checking (not by a hash). `translator` mutants leave the whitelist
and must be rejected before Lean runs. A mutant failing at the other stage,
or passing, is a test failure. An unmodified copy must pass first, so the
harness itself is shown to work. Production sources are only read.
"""

from __future__ import annotations

import ast
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
C_CHECKER = ROOT / "tests/formal/check_code_policy.py"
PY_CHECKER = ROOT / "tests/formal/python_code_mode/check_runtime_selection.py"
POLICY = ROOT / "tests/bench/python_runtime/policy.py"
C_FILES = ("core/code_policy.c", "core/code_policy.h", "core/code_runtime.h")

EXTRA = "\nbool tny_code_extra_gate(uint64_t x) { return x != 0; }\n"
# (name, file, pattern, replacement, expected stage). Patterns are regular
# expressions that must match exactly once in the current production source.
C_MUTANTS = [
    (
        "UTF-8 three-byte range admits invalid leads",
        "core/code_policy.c",
        r"byte >= 0xe0 && byte <= 0xef",
        "byte >= 0xe0",
        "lean",
    ),
    (
        "UTF-8 NUL accepted as text",
        "core/code_policy.c",
        r"byte >= 1 && byte <= 0x7f",
        "byte <= 0x7f",
        "lean",
    ),
    (
        "call budget < becomes <=",
        "core/code_policy.c",
        r"calls_done < TNY_CODE_TOOL_CALLS && name_bytes",
        "calls_done <= TNY_CODE_TOOL_CALLS && name_bytes",
        "lean",
    ),
    ("call ignores recursion", "core/code_policy.c", r"!recursive &&", "", "lean"),
    (
        "call ignores object check",
        "core/code_policy.c",
        r"&& argument_is_object;",
        ";",
        "lean",
    ),
    (
        "empty tool name admitted",
        "core/code_policy.c",
        r"name_bytes >= 1 &&",
        "",
        "lean",
    ),
    (
        "timeout admits zero",
        "core/code_policy.c",
        r"timeout_ms >= 1",
        "timeout_ms >= 0",
        "lean",
    ),
    (
        "output guard dropped",
        "core/code_policy.c",
        r"return used <= TNY_CODE_OUTPUT_BYTES && add",
        "return add",
        "lean",
    ),
    (
        "memory guard dropped",
        "core/code_policy.c",
        r"return used <= limit && header",
        "return header",
        "lean",
    ),
    (
        "memory sum may wrap",
        "core/code_policy.c",
        r"request <= limit - used - header",
        "request + header <= limit - used",
        "lean",
    ),
    (
        "result bound may wrap",
        "core/code_policy.c",
        r"result_bytes <= TNY_CODE_TOOL_RESULT_BYTES;",
        "result_bytes + 1 <= TNY_CODE_TOOL_RESULT_BYTES + 1;",
        "lean",
    ),
    (
        "frame admits terminal phases",
        "core/code_policy.c",
        r"phase == TNY_CODE_PHASE_RUNNING",
        "phase != TNY_CODE_PHASE_FAILED",
        "lean",
    ),
    (
        "frame call budget dropped",
        "core/code_policy.c",
        r"type == TNY_CODE_FRAME_CALL && calls_done < TNY_CODE_TOOL_CALLS &&",
        "type == TNY_CODE_FRAME_CALL &&",
        "lean",
    ),
    (
        "frame done bound off by one",
        "core/code_policy.c",
        r"TNY_CODE_RESULT_TEXT_BYTES \+ 1\)",
        "TNY_CODE_RESULT_TEXT_BYTES + 2)",
        "lean",
    ),
    (
        "frame admits empty payload",
        "core/code_policy.c",
        r"payload_bytes >= 1 &&",
        "",
        "lean",
    ),
    (
        "output take ignores the remaining room",
        "core/code_policy.c",
        r": TNY_CODE_OUTPUT_BYTES - used;",
        ": available;",
        "lean",
    ),
    (
        "output take subtraction may wrap",
        "core/code_policy.c",
        r"return used >= TNY_CODE_OUTPUT_BYTES\s+\? 0\s+:",
        "return",
        "lean",
    ),
    (
        "timeout ceiling raised",
        "core/code_runtime.h",
        r"TNY_CODE_MAX_TIMEOUT_MS 600000",
        "TNY_CODE_MAX_TIMEOUT_MS 600001",
        "lean",
    ),
    (
        "tool-call limit raised",
        "core/code_runtime.h",
        r"TNY_CODE_TOOL_CALLS\s+64u",
        "TNY_CODE_TOOL_CALLS 65u",
        "lean",
    ),
    (
        "source limit raised",
        "core/code_runtime.h",
        r"TNY_CODE_SOURCE_BYTES\s+\(256u",
        "TNY_CODE_SOURCE_BYTES (257u",
        "lean",
    ),
    (
        "loop",
        "core/code_policy.c",
        r"\{ return bytes <= TNY_CODE_SOURCE_BYTES; \}",
        "{ for (;;) return bytes <= TNY_CODE_SOURCE_BYTES; }",
        "translator",
    ),
    (
        "function call",
        "core/code_policy.c",
        r"return result_bytes <= TNY_CODE_TOOL_RESULT_BYTES;",
        "return result_bytes <= TNY_CODE_TOOL_RESULT_BYTES && tny_code_source_admit(0);",
        "translator",
    ),
    (
        "unsigned division",
        "core/code_policy.c",
        r"return bytes <= TNY_CODE_SOURCE_BYTES;",
        "return bytes / 2 <= TNY_CODE_SOURCE_BYTES;",
        "translator",
    ),
    (
        "shift",
        "core/code_policy.c",
        r"return bytes <= TNY_CODE_SOURCE_BYTES;",
        "return (bytes >> 1) <= TNY_CODE_SOURCE_BYTES;",
        "translator",
    ),
    (
        "signed arithmetic",
        "core/code_policy.c",
        r"timeout_ms <= TNY_CODE_MAX_TIMEOUT_MS",
        "timeout_ms + 1 <= TNY_CODE_MAX_TIMEOUT_MS + 1",
        "translator",
    ),
    (
        "narrowing cast",
        "core/code_policy.c",
        r"return bytes <= TNY_CODE_SOURCE_BYTES;",
        "return (uint32_t)bytes <= TNY_CODE_SOURCE_BYTES;",
        "translator",
    ),
    (
        "local variable",
        "core/code_policy.c",
        r"\{ return bytes <= TNY_CODE_SOURCE_BYTES; \}",
        "{ uint64_t limit = TNY_CODE_SOURCE_BYTES; return bytes <= limit; }",
        "translator",
    ),
    (
        "mutable global state",
        "core/code_policy.c",
        r"(bool tny_code_source_admit\(uint64_t bytes\)) \{ return bytes <= TNY_CODE_SOURCE_BYTES; \}",
        r"static uint64_t seen;\n\1 { return bytes <= TNY_CODE_SOURCE_BYTES && seen == 0; }",
        "translator",
    ),
    (
        "enum constant",
        "core/code_policy.c",
        r"(bool tny_code_source_admit\(uint64_t bytes\)) \{ return bytes <= TNY_CODE_SOURCE_BYTES; \}",
        r"enum { SOURCE_LIMIT = 5 };\n\1 { return bytes <= SOURCE_LIMIT; }",
        "translator",
    ),
    ("unproven extra gate", "core/code_policy.c", r"\Z", EXTRA, "translator"),
]


def run(arguments: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, *arguments],
        env=os.environ,
        capture_output=True,
        text=True,
        timeout=600,
    )


def stage(result: subprocess.CompletedProcess) -> str:
    text = result.stdout + result.stderr
    if result.returncode == 0:
        return "passed"
    if result.returncode == 3 and "translator: unsupported source" in text:
        return "translator"
    if result.returncode == 1 and "Lean rejected" in text and "error:" in text:
        return "lean"
    return f"other (exit {result.returncode}): {text[-1500:]}"


def mutate(text: str, pattern: str, replacement: str) -> str:
    result, count = re.subn(pattern, replacement, text)
    if count != 1:
        raise SystemExit(
            f"mutation pattern {pattern!r} matched {count} times; update the mutant"
        )
    return result


def c_mutants(extra: list[str]) -> list[str]:
    failures = []
    with tempfile.TemporaryDirectory(prefix="tny-code-policy-mutants-") as temporary:
        base = Path(temporary)

        def tree(name: str) -> Path:
            directory = base / name
            for relative in C_FILES:
                (directory / relative).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / "src" / relative, directory / relative)
            return directory

        control = run(
            [str(C_CHECKER), "--no-vectors", "--src", str(tree("control")), *extra]
        )
        if stage(control) != "passed":
            raise SystemExit(f"unmodified copy did not pass: {stage(control)}")
        print("C control: unmodified copy passes")
        for index, (name, relative, pattern, replacement, expected) in enumerate(
            C_MUTANTS
        ):
            directory = tree(f"m{index}")
            path = directory / relative
            path.write_text(mutate(path.read_text(), pattern, replacement))
            actual = stage(
                run([str(C_CHECKER), "--no-vectors", "--src", str(directory), *extra])
            )
            verdict = "ok" if actual == expected else "FAIL"
            print(
                f"C {verdict}: {name}: rejected by {actual.split(':')[0]} (expected {expected})"
            )
            if actual != expected:
                failures.append(f"{name}: {actual}")
    return failures


def py_mutants() -> tuple[list[str], int]:
    source = POLICY.read_text()

    def conjunct(drop: str | None = None, edit=None) -> str:
        tree = ast.parse(source)
        function = next(n for n in tree.body if isinstance(n, ast.FunctionDef))
        expression = function.body[0].value
        if drop is not None:
            before = len(expression.values)
            expression.values = [
                v for v in expression.values if drop not in ast.unparse(v)
            ]
            if len(expression.values) != before - 1:
                raise SystemExit(f"python mutant {drop!r} matched no single conjunct")
        if edit is not None:
            edit(expression)
        return ast.unparse(tree) + "\n"

    def weaken_size(expression: ast.BoolOp) -> None:
        node = next(v for v in expression.values if "bytes_lighter" in ast.unparse(v))
        node.ops = [ast.LtE()]

    def swap_tokens(expression: ast.BoolOp) -> None:
        node = next(v for v in expression.values if "tokens_lighter" in ast.unparse(v))
        node.left, node.comparators[0] = node.comparators[0], node.left

    def reverse_first(expression: ast.BoolOp) -> None:
        node = next(v for v in expression.values if "first_lighter" in ast.unparse(v))
        node.left, node.comparators[0] = node.comparators[0], node.left

    mutants = {
        "size gate dropped": (conjunct("bytes_lighter"), "lean"),
        "size gate < becomes <=": (conjunct(edit=weaken_size), "lean"),
        "semantics check dropped": (conjunct("semantics_ok"), "lean"),
        "trial completeness dropped": (conjunct("trials_complete"), "lean"),
        "solved guard dropped": (conjunct("solved_cpython > 0"), "lean"),
        "token comparison reversed": (conjunct(edit=swap_tokens), "lean"),
        "first-pass comparison reversed": (conjunct(edit=reverse_first), "lean"),
        "non-bool and operand": (
            source.replace("and semantics_ok", "and 1"),
            "translator",
        ),
        "chained comparison": (
            source.replace("corpus_total > 0", "0 < corpus_total < 10**9"),
            "translator",
        ),
        "helper call": (
            source.replace("corpus_total > 0", "max(corpus_total, 0) > 0"),
            "translator",
        ),
        "division": (
            source.replace(
                "tokens_lighter * solved_cpython", "tokens_lighter // solved_cpython"
            ),
            "translator",
        ),
    }
    failures = []
    with tempfile.TemporaryDirectory(prefix="tny-selection-mutants-") as temporary:
        control = Path(temporary) / "control.py"
        control.write_text(source)
        if stage(run([str(PY_CHECKER), "--source", str(control)])) != "passed":
            raise SystemExit("unmodified policy copy did not pass")
        print("Python control: unmodified copy passes")
        for index, (name, (text, expected)) in enumerate(mutants.items()):
            if text == source:
                raise SystemExit(f"python mutant {name!r} did not change the source")
            path = Path(temporary) / f"m{index}.py"
            path.write_text(text)
            actual = stage(run([str(PY_CHECKER), "--source", str(path)]))
            verdict = "ok" if actual == expected else "FAIL"
            print(
                f"Python {verdict}: {name}: rejected by {actual.split(':')[0]} (expected {expected})"
            )
            if actual != expected:
                failures.append(f"{name}: {actual}")
    return failures, len(mutants)


def main() -> None:
    extra = []
    for option, variable in (
        ("--clang", "TNY_FORMAL_CLANG"),
        ("--gcc", "TNY_FORMAL_GCC"),
    ):
        if os.environ.get(variable):
            extra += [option, os.environ[variable]]
    python_failures, python_count = py_mutants()
    failures = c_mutants(extra) + python_failures
    total = len(C_MUTANTS) + python_count
    if failures:
        raise SystemExit(
            "mutants not rejected at the expected stage:\n" + "\n".join(failures)
        )
    print(f"All {total} mutants rejected at the expected stage")


if __name__ == "__main__":
    main()
