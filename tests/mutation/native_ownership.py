#!/usr/bin/env python3
"""Compile scoped native owner mutants in private copies, never the active graph."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

# name, source, original, replacement, oracle group
MUTANTS = (
    (
        "prepare-once",
        "request_owner.cpp",
        "if (request->attempted) return -2;",
        "",
        "request",
    ),
    (
        "connection-reset",
        "request_owner.cpp",
        'extern "C" void oa_connection_drop(oa_connection_owner *owner) { owner->connection.reset(); }',
        'extern "C" void oa_connection_drop(oa_connection_owner *owner) { (void)owner; }',
        "request",
    ),
    (
        "auth-wipe",
        "request_owner.cpp",
        "if (bytes) secure_zero(bytes.get(), size);",
        "(void)size;",
        "request",
    ),
    ("view-release", "request_owner.cpp", "    request->view.reset();", "", "request"),
    (
        "move-source",
        "turn_owner.cpp",
        "candidate.call = std::exchange(*call, tools_call{});",
        "candidate.call = *call;",
        "runtime",
    ),
    (
        "transfer-before-copy",
        "turn_owner.cpp",
        "        candidate.copy(id, original, effective, extension, reason);\n        /* Commit is nonthrowing. All input metadata may borrow the source. */\n        candidate.call = std::exchange(*call, tools_call{});",
        "        candidate.call = std::exchange(*call, tools_call{});\n        candidate.copy(id, original, effective, extension, reason);",
        "runtime",
    ),
    (
        "pending-lifetime",
        "openai.c",
        "    o->state = ST_WAIT_CUSTOM;",
        "    oa_pending_reset(pending);\n    o->state = ST_WAIT_CUSTOM;",
        "runtime",
    ),
    (
        "cancel-authority",
        "openai.c",
        "if (invalidate) tools_call_invalidate_async(&o->turn->custom.call);",
        "(void)invalidate;",
        "runtime",
    ),
    (
        "generation-replay",
        "../../lib/custom_tools.cpp",
        "call.generation != generation",
        "(static_cast<void>(generation), false)",
        "runtime",
    ),
)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(command, log, env):
    with log.open("w") as out:
        out.write("$ " + shlex.join(command) + "\n")
        out.flush()
        return subprocess.run(
            command,
            stdout=out,
            stderr=subprocess.STDOUT,
            env=env,
            timeout=120,
            check=False,
        ).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cc", "cxx", "cflags", "cxxflags", "ldflags", "request-objects"):
        parser.add_argument("--" + name, required=True)
    for name in ("object-root", "request-bin", "runtime-bin", "work-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("objects", nargs="+", type=Path)
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="run-", dir=args.work_dir)).resolve()
    env = dict(
        os.environ,
        ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:symbolize=0",
        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
    )
    for key in ("TNY_TEST_ALLOC_SCOPE", "TNY_TEST_ALLOC_FAIL_AT"):
        env.pop(key, None)
    source_root = Path("src/backends/openai")
    sources = {(source_root / m[1]).resolve() for m in MUTANTS}
    inputs = [
        *sources,
        *args.objects,
        args.request_bin,
        args.runtime_bin,
        Path("tests/fixtures/native_request_ownership.cpp"),
    ]
    manifest = {str(p.resolve()): sha(p) for p in inputs}
    report = {"inputs": manifest, "mutants": [], "passed": False}
    try:
        for name, binary, options in (
            ("request", args.request_bin, []),
            ("runtime", args.runtime_bin, ["-s", "openai_suite"]),
        ):
            status = run(
                [str(binary.resolve()), *options], out / (name + "-baseline.log"), env
            )
            if status:
                raise RuntimeError(f"{name} baseline failed: {status}")
        for name, filename, old, new, group in MUTANTS:
            source = (source_root / filename).resolve()
            text = source.read_text()
            if text.count(old) != 1:
                raise RuntimeError(f"{name}: anchor count {text.count(old)}")
            changed = out / (name + source.suffix)
            changed.write_text(text.replace(old, new, 1))
            obj, binary = out / (name + ".o"), out / name
            record = {"name": name, "source": str(source), "sha256": sha(changed)}
            report["mutants"].append(record)
            if group == "request":
                fixture = out / (name + "-fixture.cpp")
                fixture.write_text(
                    Path("tests/fixtures/native_request_ownership.cpp")
                    .read_text()
                    .replace(
                        '"backends/openai/request_owner.cpp"', json.dumps(str(changed))
                    )
                )
                compiled = fixture
                objects = shlex.split(args.request_objects)
                options = []
            else:
                compiled = changed
                relative = source.relative_to(Path.cwd())
                object_name = (
                    str(relative) + ".o"
                    if source.suffix == ".cpp"
                    else str(relative.with_suffix(".o"))
                )
                original = (args.object_root / object_name).resolve()
                objects = [str(p.resolve()) for p in args.objects]
                if objects.count(str(original)) != 1:
                    raise RuntimeError(f"{name}: original object absent/duplicated")
                objects.remove(str(original))
                test = (
                    "native_pending_transfer_preserves_source_on_failure"
                    if name in ("move-source", "transfer-before-copy")
                    else "native_pending_lifecycle_and_allocation_sweeps"
                )
                options = ["-s", "openai_suite", "-t", test]
            cpp = compiled.suffix == ".cpp"
            command = [
                *shlex.split(args.cxx if cpp else args.cc),
                *shlex.split(args.cxxflags if cpp else args.cflags),
                "-c",
                str(compiled),
                "-o",
                str(obj),
            ]
            record["compile_exit"] = run(command, out / (name + "-compile.log"), env)
            if record["compile_exit"]:
                raise RuntimeError(f"{name}: did not compile; not a kill")
            record["link_exit"] = run(
                [
                    *shlex.split(args.cxx),
                    str(obj),
                    *objects,
                    *shlex.split(args.ldflags),
                    "-o",
                    str(binary),
                ],
                out / (name + "-link.log"),
                env,
            )
            if record["link_exit"]:
                raise RuntimeError(f"{name}: did not link; not a kill")
            record["run_exit"] = run(
                [str(binary), *options], out / (name + ".log"), env
            )
            diagnostic = (out / (name + ".log")).read_text(errors="replace")
            record["killed"] = record["run_exit"] != 0 and any(
                marker in diagnostic
                for marker in (
                    "native request ownership failed",
                    "FAIL native_",
                    "Assertion failed",
                    "Assertion `",
                    "AddressSanitizer:",
                    "runtime error:",
                )
            )
            if not record["killed"]:
                raise RuntimeError(f"{name}: no behavioral kill")
            print(f"{name}: compiled, behavioral oracle killed mutant", flush=True)
        if any(sha(path) != digest for path, digest in manifest.items()):
            raise RuntimeError("active source/build graph changed during mutation run")
        report["passed"] = True
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
        print(str(exc), flush=True)
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"native mutation evidence: {out / 'report.json'}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
