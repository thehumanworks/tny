#!/usr/bin/env python3
"""Behavioral phase-1 mutants in private build copies; never edits production."""

import hashlib
import json
import shlex
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MUTANTS = [
    (
        "frame-limit",
        "src/net/connectrpc.cpp",
        "if (state.length > max_frame) return d->status = -1;",
        "if (state.length > max_frame && false) return d->status = -1;",
        "64 MiB frame-size check",
    ),
    (
        "identity-precedence",
        "src/backends/openai/toolcalls.cpp",
        "if (candidate >= 0 && !cs->calls[candidate].id) slot = candidate;",
        "if (candidate >= 0) slot = candidate;",
        "fresh ID must take precedence",
    ),
    (
        "borrowed-document",
        "src/backends/openai/toolcalls.cpp",
        "c.has_id ? c.id.data() : nullptr",
        "const_cast<char *>(id)",
        "heap-use-after-free",
    ),
    (
        "swallowed-oom",
        "src/net/connectrpc.cpp",
        "return d->status = -2;",
        "return d->status = 0;",
        "OOM must not be swallowed",
    ),
]


def run(command, log):
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    log.write_text(result.stdout + result.stderr)
    return result


def main():
    directory = ROOT / "build/parser-mutations"
    directory.mkdir(parents=True, exist_ok=True)
    subprocess.run(["make", "test-parser-smoke"], cwd=ROOT, check=True)
    variables = subprocess.check_output(
        ["make", "-s", "-f", "Makefile", "-f", "-", "mutant-vars"],
        cwd=ROOT,
        text=True,
        input="mutant-vars:\n\t@printf '%s\\n' '$(CXX)' '$(call cppflags,$(DBG_CFLAGS))' "
        "'$(filter-out $(CXX_RUNTIME),$(DBG_LDFLAGS))' "
        "'$(filter-out $(OBJ_DBG)/src/util/alloc.o,$(sort $(TEST_OBJS)))'\n",
    ).splitlines()
    compiler, flags, linker, objects = map(shlex.split, variables)
    results = []
    for name, source, old, new, reason in MUTANTS:
        path = ROOT / source
        original = path.read_text()
        assert original.count(old) == 1, (name, "mutation anchor changed")
        fingerprint = hashlib.sha256(path.read_bytes()).hexdigest()
        mutant = directory / (name + ".cpp")
        mutant.write_text(original.replace(old, new))
        obj = directory / (name + ".o")
        binary = directory / name
        compile_result = run(
            [*compiler, *flags, "-c", str(mutant), "-o", str(obj)],
            directory / (name + "-compile.log"),
        )
        assert compile_result.returncode == 0, compile_result.stderr
        link = run(
            [
                *compiler,
                *flags,
                "-o",
                str(binary),
                str(obj),
                "build/dbg/tests/fuzz/fuzz_parsers.cpp.o",
                "build/dbg/parser-alloc.o",
                *[p for p in objects if p != "build/dbg/" + source + ".o"],
                *linker,
            ],
            directory / (name + "-link.log"),
        )
        assert link.returncode == 0, link.stderr
        test = run([str(binary)], directory / (name + "-test.log"))
        killed = test.returncode != 0 and reason in test.stdout + test.stderr
        results.append(
            {
                "mutation": name,
                "exit": test.returncode,
                "reason": reason,
                "killed": killed,
                "source_sha256": fingerprint,
            }
        )
        assert hashlib.sha256(path.read_bytes()).hexdigest() == fingerprint
        print(json.dumps(results[-1]), flush=True)
        assert killed, test.stdout + test.stderr
    (directory / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    subprocess.run(["make", "test-parser-smoke"], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
