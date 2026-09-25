#!/usr/bin/env python3
"""Compile scoped native owner mutants in private copies, never the active graph."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import signal
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
    # Model callbacks are deliberately unreachable. Mutate the still-supported
    # private lease API and the two production refusal boundaries instead.
    (
        "pending-lifetime",
        "../../lib/custom_tools.cpp",
        "else if (!call.completed) return 0;",
        "else if (!call.completed) return -1;",
        "runtime",
    ),
    (
        "cancel-authority",
        "../../lib/custom_tools.cpp",
        "    detach(*pending->state);",
        "    (void)pending;",
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
MUTANTS += (
    (
        "builder-view-release",
        "openai.c",
        "    oa_request_take_view(request, NULL);",
        "",
        "runtime",
    ),
    (
        "terminal-once",
        "openai.c",
        "provider_oom() || !o->turn_open",
        "provider_oom()",
        "runtime",
    ),
    (
        "continuation-retention",
        "openai.c",
        "if (!o->continuing) buf_clear(&o->turn->text);",
        "buf_clear(&o->turn->text);",
        "runtime",
    ),
    (
        "steer-transfer",
        "openai.c",
        'session_add_text(o->env.session, "user", o->turn->steer);',
        "(void)o;",
        "runtime",
    ),
    (
        "checkpoint-index",
        "openai.c",
        'o->tool_index = (int)jget_int(r, "tool_index", 0);',
        "o->tool_index = 0;",
        "runtime",
    ),
    (
        "raw-tool-refusal",
        "openai.c",
        'if (strcmp(name, "run_code") != 0) {',
        "if (false) {",
        "runtime",
    ),
    (
        "embedded-execution-refusal",
        "../../core/execution.c",
        "if ((env->ctx->library_mode && !env->ctx->prompt_optimisation) || env->ctx->custom_tools ||\n        env->ctx->host_services)",
        "if (false)",
        "runtime",
    ),
)


def child_environment(directory):
    allowed = {
        "PATH",
        "SYSTEMROOT",
        "WINDIR",
        "SDKROOT",
        "DEVELOPER_DIR",
        "LANG",
        "LC_ALL",
        "PKG_CONFIG_PATH",
    }
    env = {key: value for key, value in os.environ.items() if key in allowed}
    for key, name in (
        ("HOME", "home"),
        ("TMPDIR", "tmp"),
        ("XDG_CONFIG_HOME", "config"),
        ("XDG_CACHE_HOME", "cache"),
    ):
        path = directory / name
        path.mkdir(exist_ok=True)
        env[key] = str(path)
    env.update(
        ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:symbolize=0",
        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
    )
    return env


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
    env = child_environment(out)
    source_root = Path("src/backends/openai")
    sources = {(source_root / m[1]).resolve() for m in MUTANTS}
    tools_source = Path("src/core/tools.c").resolve()
    sources.add(tools_source)
    inputs = [
        *sources,
        *args.objects,
        args.request_bin,
        args.runtime_bin,
        Path("tests/fixtures/native_request_ownership.cpp"),
    ]
    manifest = {str(p.resolve()): sha(p) for p in inputs}
    report = {"inputs": manifest, "mutants": [], "passed": False}
    guard = "    if (call->custom_call) abort(); /* resource-only teardown requires no live lease */"
    tools_text = tools_source.read_text()
    if tools_text.count(guard) != 1:
        raise RuntimeError("live-lease guard anchor absent/duplicated")
    tools_object = (args.object_root / "src/core/tools.o").resolve()
    runtime_objects = [str(p.resolve()) for p in args.objects]

    def compile_tools(text, name):
        source, obj = out / (name + ".c"), out / (name + ".o")
        source.write_text(text)
        status = run(
            [
                *shlex.split(args.cc),
                *shlex.split(args.cflags),
                "-DNDEBUG",
                "-c",
                str(source),
                "-o",
                str(obj),
            ],
            out / (name + "-compile.log"),
            env,
        )
        if status:
            raise RuntimeError(f"{name}: compile failed, not a kill")
        return obj

    def link_runtime(obj, name):
        objects = [
            str(obj) if path == str(tools_object) else path for path in runtime_objects
        ]
        binary = out / name
        status = run(
            [
                *shlex.split(args.cxx),
                *objects,
                *shlex.split(args.ldflags),
                "-o",
                str(binary),
            ],
            out / (name + "-link.log"),
            env,
        )
        if status:
            raise RuntimeError(f"{name}: link failed, not a kill")
        return binary

    try:
        guarded_obj = compile_tools(tools_text, "guard-ndebug")
        guarded = link_runtime(guarded_obj, "guard-ndebug")
        guard_status = run(
            [str(guarded), "--native-storage-guard"], out / "guard-ndebug.log", env
        )
        report["guard_ndebug"] = {
            "compile_exit": 0,
            "link_exit": 0,
            "run_exit": guard_status,
            "caught": guard_status == -signal.SIGABRT,
        }
        if not report["guard_ndebug"]["caught"]:
            raise RuntimeError("NDEBUG invariant guard did not abort")
        guardless_obj = compile_tools(
            tools_text.replace(guard, ""), "guardless-test-copy"
        )
        report["guardless_source_sha256"] = sha(out / "guardless-test-copy.c")
        guardless = link_runtime(guardless_obj, "guardless-baseline")
        status = run(
            [str(guardless), "-s", "openai_suite"], out / "guardless-baseline.log", env
        )
        report["guardless_baseline_exit"] = status
        if status:
            raise RuntimeError("guardless control baseline failed")
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
            expected = 2 if name == "builder-view-release" else 1
            if text.count(old) != expected:
                raise RuntimeError(f"{name}: anchor count {text.count(old)}")
            changed = out / (name + source.suffix)
            changed.write_text(text.replace(old, new, expected))
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
                test = {
                    "pending-lifetime": "native_pending_private_completion_and_invalidation",
                    "cancel-authority": "native_pending_private_completion_and_invalidation",
                    "generation-replay": "native_pending_private_completion_and_invalidation",
                    "move-source": "native_pending_transfer_preserves_source_on_failure",
                    "transfer-before-copy": "native_pending_transfer_preserves_source_on_failure",
                    "builder-view-release": "request_construction_oom_after_usage_skips_finalization",
                    "terminal-once": "native_request_real_stale_replay_and_control_stop",
                    "continuation-retention": "native_continuation_retains_text",
                    "steer-transfer": "native_checkpoint_retains_steer_and_consumed_index",
                    "checkpoint-index": "native_checkpoint_retains_steer_and_consumed_index",
                }.get(name, "native_pending_lifecycle_and_allocation_sweeps")
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
                    "FAIL request_construction_",
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
