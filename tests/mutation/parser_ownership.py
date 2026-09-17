#!/usr/bin/env python3
"""Compile behavioral parser mutants against the real instrumented owners.

A compile failure, timeout, or missing source anchor is an infrastructure
failure, never a killed mutant. Production sources and normal objects remain
unchanged. Each run writes an isolated, hashed evidence directory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

MUTANTS = (
    (
        "frame-limit",
        "src/net/connectrpc.cpp",
        "if (s.length > CONNECT_MAX_FRAME)",
        "if (false)",
    ),
    (
        "id-precedence",
        "src/backends/openai/toolcalls.cpp",
        "c = by_id(id);",
        "c = has_index ? by_index(index) : by_id(id);",
    ),
    (
        "borrowed-document",
        "src/backends/openai/toolcalls.cpp",
        "view(cs, *state, c);",
        "view(cs, *state, c);\n"
        '            if (cs->n) cs->calls[0].id = jget_str(yyjson_arr_get_first(tool_calls), "id");',
    ),
    (
        "swallowed-oom",
        "src/backends/openai/stream_decode.cpp",
        "if (decoder->status) return decoder->status;\n"
        "    /* C callbacks are outside all throwing decode operations. */",
        "if (decoder->status) return TNY_PARSE_OK;\n"
        "    /* C callbacks are outside all throwing decode operations. */",
    ),
)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(command: list[str], log: Path, timeout: float, env: dict[str, str]) -> int:
    with log.open("wb") as output:
        output.write(("$ " + shlex.join(command) + "\n").encode())
        output.flush()
        run = subprocess.run(
            command,
            stdout=output,
            stderr=subprocess.STDOUT,
            env=env,
            timeout=timeout,
            check=False,
        )
    return run.returncode


def main(
    mutants=MUTANTS,
    oracle_marker="ownership self-test failed at line",
    label="parser",
) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--flags", required=True)
    parser.add_argument("--ldflags", required=True)
    parser.add_argument("--object-root", type=Path, required=True)
    parser.add_argument("--test-object", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("objects", type=Path, nargs="+")
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=args.work_dir)).resolve()
    report: dict = {"schema": 1, "mutants": [], "passed": False}
    env = dict(
        os.environ,
        ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
    )
    for key in ("TNY_TEST_ALLOC_SCOPE", "TNY_TEST_ALLOC_FAIL_AT", "TNY_TOOLS"):
        env.pop(key, None)
    original_paths = [
        args.test_object,
        args.baseline,
        *args.objects,
        *(Path(row[1]) for row in mutants),
    ]
    try:
        fingerprints = {str(path.resolve()): sha(path) for path in original_paths}
        report["inputs"] = fingerprints
        baseline = execute(
            [str(args.baseline.resolve())], output / "baseline.log", 60, env
        )
        report["baseline_exit"] = baseline
        if baseline:
            raise RuntimeError(f"instrumented baseline failed with status {baseline}")
        compiler = shlex.split(args.cxx)
        flags = shlex.split(args.flags)
        for name, source, old, new in mutants:
            text = Path(source).read_text(encoding="utf-8")
            if text.count(old) != 1:
                raise RuntimeError(
                    f"{name}: expected one source anchor, found {text.count(old)}"
                )
            changed = output / f"{name}.cpp"
            changed.write_text(text.replace(old, new, 1), encoding="utf-8")
            obj, binary = output / f"{name}.o", output / name
            original_object = (args.object_root / (source + ".o")).resolve()
            objects = [str(path.resolve()) for path in args.objects]
            if objects.count(str(original_object)) != 1:
                raise RuntimeError(
                    f"{name}: mutated production object absent or duplicated"
                )
            objects[objects.index(str(original_object))] = str(obj)
            record = {"name": name, "source": source, "sha256": sha(changed)}
            report["mutants"].append(record)
            compile_command = [*compiler, *flags, "-c", str(changed), "-o", str(obj)]
            record["compile_exit"] = execute(
                compile_command, output / f"{name}-compile.log", 120, env
            )
            if record["compile_exit"]:
                raise RuntimeError(
                    f"{name}: mutant did not compile (not a behavioral kill)"
                )
            link = [
                *compiler,
                *flags,
                str(args.test_object.resolve()),
                *objects,
                *shlex.split(args.ldflags),
                "-o",
                str(binary),
            ]
            record["link_exit"] = execute(link, output / f"{name}-link.log", 120, env)
            if record["link_exit"]:
                raise RuntimeError(
                    f"{name}: mutant did not link (not a behavioral kill)"
                )
            record["run_exit"] = execute([str(binary)], output / f"{name}.log", 60, env)
            diagnostic = (output / f"{name}.log").read_text(errors="replace")
            oracle = any(
                marker in diagnostic
                for marker in (
                    oracle_marker,
                    "ERROR: AddressSanitizer:",
                    "runtime error:",
                )
            )
            record["killed"] = record["run_exit"] != 0 and oracle
            print(
                f"{name}: {'KILLED' if record['killed'] else 'SURVIVED/INFRASTRUCTURE FAILURE'} (exit {record['run_exit']})",
                flush=True,
            )
            if not record["killed"]:
                raise RuntimeError(f"{name}: no demonstrated behavioral kill")
        if any(sha(Path(path)) != expected for path, expected in fingerprints.items()):
            raise RuntimeError(
                "an original source or object changed during the mutation gate"
            )
        report["passed"] = True
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
        print(f"{label} mutation gate failed: {exc}", file=sys.stderr)
    (output / "report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    print(f"{label} mutation evidence: {output / 'report.json'}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
