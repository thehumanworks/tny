"""Sandboxed execution and strict independent-effect scoring for all arms."""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import time
from typing import Any

from cases import CATALOG, VARIANTS, dump, fixture
from policy import accept

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / "build/code-mode-bench"


def reject_constant(value: str) -> None:
    raise ValueError(f"nonfinite JSON number: {value}")


def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def strict_json(text: str) -> Any:
    return json.loads(text, object_pairs_hook=unique_object, parse_constant=reject_constant)


def typed_equal(left: Any, right: Any) -> bool:
    if type(left) is not type(right):
        # JSON has one numeric type; bool is deliberately NOT a number here.
        return type(left) in (int, float) and type(right) in (int, float) and left == right
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(typed_equal(left[k], right[k]) for k in left)
    if isinstance(left, list):
        return len(left) == len(right) and all(typed_equal(a, b) for a, b in zip(left, right, strict=True))
    return left == right


def score(result: dict[str, Any], case: dict[str, Any]) -> dict[str, Any]:
    execution_ok = result.get("runtime_ok") is True and result.get("stdout") == "done\n"
    calls = result.get("calls")
    trace_ok = (result.get("invalid_call") is False and isinstance(calls, list)
                and len(calls) <= 64 and all(expected in calls for expected in case["required_calls"]))
    writes = result.get("writes")
    output_ok = isinstance(writes, dict) and writes.keys() == case["expected"].keys()
    if output_ok:
        try:
            for path, expected in case["expected"].items():
                observed = strict_json(writes[path]) if path in case["json_paths"] else writes[path]
                output_ok = output_ok and typed_equal(observed, expected)
        except (ValueError, TypeError):
            output_ok = False
    return {"passed": accept(execution_ok, output_ok, trace_ok), "execution_ok": execution_ok,
            "output_ok": output_ok, "trace_ok": trace_ok}


def sandbox_command(language: str) -> list[str]:
    bwrap = shutil.which("bwrap")
    prlimit = shutil.which("prlimit")
    if not bwrap or not prlimit:
        raise RuntimeError("Linux bubblewrap and prlimit are mandatory; no unsafe fallback")
    build = json.loads((BUILD / "build.json").read_text())
    home = build["python_home"]
    command = [bwrap, "--unshare-all", "--die-with-parent", "--new-session"]
    for source in ("/usr", "/lib", "/lib64"):
        if Path(source).exists():
            command += ["--ro-bind", source, source]
    command += ["--ro-bind", home, home, "--ro-bind", str(BUILD / language), "/runner",
                "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp", "--chdir", "/tmp",
                "--clearenv", "--setenv", "PATH", "/usr/bin", "--setenv", "HOME", "/tmp",
                "--setenv", "LANG", "C.UTF-8", "--", "/usr/bin/prlimit", "--as=536870912",
                "--cpu=3", "--fsize=1048576", "--nofile=64", "--", "/runner"]
    return command


def run_program(language: str, code: str, data: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(code, str) or "\0" in code or len(code.encode()) > 262144:
        return {"runtime_ok": False, "stdout": "invalid source", "calls": [], "writes": {}, "invalid_call": False}
    start = time.monotonic()
    try:
        completed = subprocess.run(sandbox_command(language), input=dump({"code": code, "catalog": dump(CATALOG),
                                   "fixture": data}).encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   env={"PATH": "/usr/bin:/bin"}, timeout=6, check=False)
        if completed.returncode or len(completed.stdout) > 2 * 1024 * 1024:
            result = {"runtime_ok": False, "stdout": "executor failed", "calls": [], "writes": {},
                      "invalid_call": False, "exit_code": completed.returncode,
                      "stderr": completed.stderr.decode(errors="replace")[:2000]}
        else:
            result = strict_json(completed.stdout.decode())
            if not isinstance(result, dict):
                raise ValueError("executor result must be an object")
    except (subprocess.TimeoutExpired, ValueError, UnicodeError) as error:
        result = {"runtime_ok": False, "stdout": str(error)[:1000], "calls": [], "writes": {}, "invalid_call": False}
    result["process_wall_seconds"] = time.monotonic() - start
    return result


def evaluate(language: str, task: str, code: str) -> dict[str, Any]:
    variants = []
    for index in range(VARIANTS):
        case = fixture(task, index)
        observed = run_program(language, code, case["runtime"])
        variants.append({"variant": index, **score(observed, case), "observed": observed})
    return {"passed": all(v["passed"] for v in variants), "variants": variants}


def main() -> None:
    from cases import LANGUAGES, TASKS
    from references import reference
    results = []
    for language in LANGUAGES:
        for task, _ in TASKS:
            result = evaluate(language, task, reference(language, task))
            results.append({"language": language, "task": task, **result})
            print(language, task, result["passed"], flush=True)
            if not result["passed"]:
                print(json.dumps(result, ensure_ascii=False, indent=2))
    (BUILD / "controls.json").write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n")
    if not all(r["passed"] for r in results):
        raise SystemExit(1)
    print(f"Reference controls: {len(results) * VARIANTS}/{len(results) * VARIANTS} passed")


if __name__ == "__main__":
    main()
