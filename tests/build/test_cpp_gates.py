#!/usr/bin/env python3
"""Prove discovery, instrumentation and negative C++ quality gates."""

import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def run(args, **kwargs):
    return subprocess.run(args, cwd=ROOT, text=True, capture_output=True, **kwargs)


def main():
    # Deliberately untracked source: new files must enter discovery before commit.
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".cpp", prefix="gate_probe_", dir=ROOT / "src/cpp"
    ) as probe:
        relative = str(Path(probe.name).relative_to(ROOT))
        probe.write("int gate_probe(){return 1;}\n")
        probe.flush()
        variables = [
            "SRC_CPP",
            "SRC_SHARED",
            "FMT_SRC",
            "TIDY_CPP_SRC",
            "REL_OBJS",
            "TEST_OBJS",
            "PARSER_TEST_CPP_OBJS",
            "PARSER_TEST_OBJS",
            "LIB_PIC_OBJS",
            "FAULT_PIC_OBJS",
            "FAULT_SAN_PIC_OBJS",
            "TSAN_PIC_OBJS",
            "FUZZ_OBJS",
            "WASM_OBJS",
        ]
        script = "probe-vars:\n" + "".join(
            f"\t@printf '%s\\n' '{name}=$({name})'\n" for name in variables
        )
        result = run(
            ["make", "-s", "-f", "Makefile", "-f", "-", "probe-vars"], input=script
        )
        assert result.returncode == 0, result.stdout + result.stderr
        for line in result.stdout.splitlines():
            name, paths = line.split("=", 1)
            assert relative in paths, f"{name} did not discover {relative}"
        fmt = run(
            [
                "make",
                "format-check",
                f"FMT_SRC={relative}",
                "RUFF=true",
                "SHFMT=true",
                f"CLANG_FORMAT={os.environ.get('CLANG_FORMAT', 'clang-format')}",
            ]
        )
        assert fmt.returncode != 0 and "clang-format-violations" in fmt.stderr, (
            fmt.stdout + fmt.stderr
        )
        print("format negative: known C++ formatting violation rejected")
        Path(probe.name).write_text(
            "int gate_probe();\nint gate_probe() {\n    int *p = nullptr;\n    return *p;\n}\n"
        )
        tidy = run(
            [
                "make",
                "tidy",
                "TIDY_SRC=src/util/alloc.c",
                f"TIDY_CPP_SRC={relative}",
                f"CLANG_TIDY={os.environ.get('CLANG_TIDY', 'clang-tidy')}",
            ]
        )
        output = tidy.stdout + tidy.stderr
        assert (
            tidy.returncode != 0 and "clang-analyzer-core.NullDereference" in output
        ), output
        print("analyzer negative: enabled C++ null-dereference diagnostic rejected")
        for target, compiler, flag in [
            ("build/rel/" + relative + ".o", "c++20", "-flto"),
            ("build/dbg/" + relative + ".o", "c++20", "-fsanitize=address,undefined"),
            ("build/parser-test/" + relative + ".o", "c++20", "-DTNY_ALLOC_TESTING=1"),
            ("build/fault-pic/" + relative + ".o", "c++20", "-DTNY_ALLOC_TESTING=1"),
            (
                "build/fault-san-pic/" + relative + ".o",
                "c++20",
                "-fsanitize=address,undefined",
            ),
            ("build/tsan-pic/" + relative + ".o", "c++20", "-fsanitize=thread"),
            (
                "build/fuzz-libfuzzer/obj/" + relative + ".o",
                "clang++",
                "-fsanitize=fuzzer-no-link,address,undefined",
            ),
            ("build/wasm/obj/" + relative + ".o", "em++", "-fexceptions"),
        ]:
            dry = run(["make", "-n", target])
            assert (
                dry.returncode == 0 and compiler in dry.stdout and flag in dry.stdout
            ), dry.stdout + dry.stderr
        print(
            "positive discovery: release/debug/PIC/fault/sanitize/TSan/fuzz/wasm and quality"
        )


if __name__ == "__main__":
    main()
