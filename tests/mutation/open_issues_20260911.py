#!/usr/bin/env python3
"""Controlled issue-123 faults; always operate on a disposable source copy.

The full issue-set contract remains in docs/verification/open-issues-2026-09-11.
This runner never treats compiler errors, hangs or a missing test as kills.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

TARGET = "src/core/subagent.c"
PLAN_TEST = "subagent_plan_carries_resolved_config_privately"
PREPARE_TEST = "subagent_prepare_rejects_with_exact_codes"
OUTCOME_TEST = "subagent_process_outcomes_are_classified"
TIMING_TEST = "subagent_child_wind_down_completes_before_forced_kill"
FAULTS = [
    (
        "M123.1",
        "if (action == SA_CREATE && id)",
        "if (false && action == SA_CREATE && id)",
        PREPARE_TEST,
    ),
    (
        "M123.2a",
        "argv[n++] = (char *)tny_provider_name(ctx);",
        'argv[n++] = (char *)"openai";',
        PLAN_TEST,
    ),
    (
        "M123.2b",
        "bool key = ctx->api_key && *ctx->api_key;",
        "bool key = false;",
        PLAN_TEST,
    ),
    (
        "M123.2c",
        'plan_assign(plan, "TNY_NESTED_MODE", tny_perm_mode_name(ctx->perm_mode))',
        'plan_assign(plan, "TNY_NESTED_MODE", "yolo")',
        PLAN_TEST,
    ),
    (
        "M123.3",
        "bool exited0 = p->status_known && WIFEXITED(p->status) && WEXITSTATUS(p->status) == 0;",
        "bool exited0 = true;",
        OUTCOME_TEST,
    ),
    (
        "M123.4",
        "SUBAGENT_INVALID_ARGUMENT: action must be",
        "SUBAGENT_WRONG_CODE: action must be",
        PREPARE_TEST,
    ),
    (
        "M123.5",
        "char *result = NULL;\n    if (exited0 && reported_ok)",
        'char *result = NULL;\n    if (cerr && *cerr) result = tool_err("%s", cerr);\n    if (exited0 && reported_ok)',
        OUTCOME_TEST,
    ),
    (
        "M123.6",
        "#define SUBAGENT_CANCEL_GRACE_MS (TNY_PROCESS_CANCEL_GRACE_MS + 1000)",
        "#define SUBAGENT_CANCEL_GRACE_MS 3000",
        TIMING_TEST,
    ),
]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def manifest(root):
    result = {}
    for base, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = sorted(
            d for d in dirs if d not in {".git", "build", "__pycache__", "node_modules"}
        )
        for name in sorted(files):
            path = Path(base) / name
            rel = path.relative_to(root).as_posix()
            if rel.startswith("docs/verification/"):
                continue
            result[rel] = (
                {"symlink": os.readlink(path)}
                if path.is_symlink()
                else {
                    "sha256": sha(path.read_bytes()),
                    "mode": oct(path.stat().st_mode & 0o777),
                }
            )
    return result


def store(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


def command(root, artifacts, name, argv, env, timeout=120):
    log = artifacts / (name + ".log")
    started = datetime.now(timezone.utc).isoformat()
    t0 = time.monotonic()
    with log.open("x") as out:
        try:
            process = subprocess.run(
                argv,
                cwd=root,
                env=env,
                stdout=out,
                stderr=subprocess.STDOUT,
                timeout=timeout,
            )
            code, timed_out = process.returncode, False
        except subprocess.TimeoutExpired:
            code, timed_out = None, True
    text = log.read_text(errors="replace")
    record = {
        "name": name,
        "started_utc": started,
        "cwd": str(root),
        "argv": argv,
        "exit_code": code,
        "timed_out": timed_out,
        "seconds": round(time.monotonic() - t0, 3),
        "log": log.name,
        "log_sha256": sha(log.read_bytes()),
        "target_sha256": sha((root / TARGET).read_bytes()),
    }
    binary = root / "build/tny-test"
    obj = root / "build/dbg/src/core/subagent.o"
    record["binary_sha256"] = sha(binary.read_bytes()) if binary.exists() else None
    record["object_sha256"] = sha(obj.read_bytes()) if obj.exists() else None
    store(artifacts / (name + ".json"), record)
    return record, text


def one_test(root, artifacts, name, test, env):
    record, text = command(
        root, artifacts, name, ["./build/tny-test", "-t", test, "-v"], env, 25
    )
    exactly_one = re.search(r"Total: 1 test \(", text) is not None
    passed = (
        record["exit_code"] == 0
        and exactly_one
        and "Pass: 1, fail: 0, skip: 0." in text
    )
    intended_failure = (
        record["exit_code"] not in (None, 0)
        and not record["timed_out"]
        and exactly_one
        and "Pass: 0, fail: 1, skip: 0." in text
        and test in text
        and re.search(r"FAIL.*" + re.escape(test), text) is not None
    )
    return passed, intended_failure, record


def rebuild(root, artifacts, name, env):
    # Deleting only this disposable object's cache avoids GNU make 3.81's
    # second-granularity trap without depending on future file timestamps.
    # Rebuilding an object is insufficient: old make may see the new object
    # and the old executable within one second and omit the final link.
    for path in (root / "build/dbg/src/core/subagent.o", root / "build/tny-test"):
        if path.exists():
            path.unlink()
    record, _ = command(
        root, artifacts, name, ["make", "-j4", "build/tny-test"], env, 180
    )
    return record["exit_code"] == 0 and not record["timed_out"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source", type=Path, required=True, help="frozen reviewed source directory"
    )
    parser.add_argument(
        "--artifacts", type=Path, required=True, help="new evidence directory"
    )
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    artifacts = args.artifacts.resolve()
    if artifacts.is_relative_to(source):
        parser.error("artifacts must not be inside the frozen input directory")
    artifacts.mkdir(parents=True, exist_ok=False)
    (artifacts / "runner.py").write_bytes(Path(__file__).read_bytes())
    before = manifest(source)
    root = Path(tempfile.mkdtemp(prefix="tny-123-controlled-faults-")) / "source"
    shutil.copytree(
        source,
        root,
        symlinks=True,
        ignore=shutil.ignore_patterns(".git", "build", "__pycache__", "node_modules"),
    )
    original = (root / TARGET).read_text()
    plan = [
        {
            "id": mid,
            "path": TARGET,
            "before": old,
            "after": new,
            "test": test,
            "replacement_count": original.count(old),
        }
        for mid, old, new, test in FAULTS
    ]
    store(
        artifacts / "plan.json",
        {
            "source": str(source),
            "disposable": str(root),
            "source_manifest": before,
            "source_sha256": sha(json.dumps(before, sort_keys=True).encode()),
            "runner_sha256": sha(Path(__file__).read_bytes()),
            "mutants": plan,
        },
    )
    env = os.environ.copy()
    for key in tuple(env):
        if key.startswith("TNY_") or any(
            part in key
            for part in ("TOKEN", "PASSWORD", "SECRET", "API_KEY", "ACCESS_KEY")
        ):
            env.pop(key)
    tests = list(dict.fromkeys(case["test"] for case in plan))
    outcomes = []
    original_ok = all(case["replacement_count"] == 1 for case in plan)
    if original_ok:
        original_ok = rebuild(root, artifacts, "baseline-build", env)
    if original_ok:
        for index, test in enumerate(tests):
            passed, _, _ = one_test(root, artifacts, f"baseline-{index}", test, env)
            original_ok = passed and original_ok
    try:
        for case in plan:
            mid = case["id"]
            outcome = {"id": mid, "test": case["test"], "verdict": "UNRUN"}
            if not original_ok:
                outcome["reason"] = (
                    "unmodified build/test or exact fault mapping did not pass"
                )
            else:
                (root / TARGET).write_text(
                    original.replace(case["before"], case["after"], 1)
                )
                if not rebuild(root, artifacts, mid + "-build", env):
                    outcome["verdict"] = "INVALID_OR_BUILD_TIMEOUT"
                else:
                    passed, killed, record = one_test(
                        root, artifacts, mid + "-test", case["test"], env
                    )
                    outcome["verdict"] = (
                        "KILLED_BEHAVIOR"
                        if killed
                        else "SURVIVED"
                        if passed
                        else "UNRELATED_FAILURE_OR_TIMEOUT"
                    )
                    outcome["test_record"] = record["name"] + ".json"
            outcomes.append(outcome)
            (root / TARGET).write_text(original)
            print(json.dumps(outcome), flush=True)
    finally:
        (root / TARGET).write_text(original)
    restored = rebuild(root, artifacts, "restored-build", env)
    if restored:
        for index, test in enumerate(tests):
            passed, _, _ = one_test(root, artifacts, f"restored-{index}", test, env)
            restored = passed and restored
    source_unchanged = manifest(source) == before
    restored_bytes = (root / TARGET).read_text() == original
    complete = (
        original_ok
        and restored
        and restored_bytes
        and source_unchanged
        and all(outcome["verdict"] == "KILLED_BEHAVIOR" for outcome in outcomes)
    )
    result = {
        "scope": "M123.1, M123.2a/b/c, M123.3-M123.6 only; other issue families remain active",
        "original_passed": original_ok,
        "mutants": outcomes,
        "restored_build_and_tests_passed": restored,
        "restored_target_bytes": restored_bytes,
        "frozen_source_unchanged": source_unchanged,
        "verdict": "PASS_THIS_FAULT_SET" if complete else "INCOMPLETE",
        "disposable_source": str(root),
    }
    store(artifacts / "result.json", result)
    print(json.dumps(result, indent=2), flush=True)
    return 0 if complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
