"""Replay the frozen 36 Python programs on candidate runtimes, all variants.

Programs come unmodified from the published PR #197 samples. Scoring, hidden
fixtures and the catalog are imported from the preserved benchmark. Execution
uses the same mandatory bubblewrap/prlimit envelope (two-second watchdog).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/code_mode"))

from cases import CATALOG, VARIANTS, dump, fixture  # noqa: E402
from execute import score, strict_json  # noqa: E402

SAMPLES = ROOT / "docs/verification/code-mode-language/data/samples.json"
BUILD = ROOT / "build/python-runtime-bench"


def sandbox(binary: Path, extra_ro: list[str]) -> list[str]:
    bwrap, prlimit = shutil.which("bwrap"), shutil.which("prlimit")
    if not bwrap or not prlimit:
        raise RuntimeError("Linux bubblewrap and prlimit are mandatory; no unsafe fallback")
    command = [bwrap, "--unshare-all", "--die-with-parent", "--new-session"]
    for source in ("/usr", "/lib", "/lib64", *extra_ro):
        if Path(source).exists():
            command += ["--ro-bind", source, source]
    command += ["--ro-bind", str(binary), "/runner", "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp",
                "--chdir", "/tmp", "--clearenv", "--setenv", "PATH", "/usr/bin", "--setenv", "HOME", "/tmp",
                "--setenv", "LANG", "C.UTF-8", "--", "/usr/bin/prlimit", "--as=536870912", "--cpu=3",
                "--fsize=1048576", "--nofile=64", "--", "/runner"]
    return command


def run(command: list[str], code: str, data: dict[str, Any]) -> dict[str, Any]:
    start = time.monotonic()
    try:
        done = subprocess.run(command, input=dump({"code": code, "catalog": dump(CATALOG), "fixture": data}).encode(),
                              capture_output=True, env={"PATH": "/usr/bin:/bin"}, timeout=2, check=False)
        if done.returncode or len(done.stdout) > 2 * 1024 * 1024:
            result = {"runtime_ok": False, "stdout": "executor failed", "calls": [], "writes": {},
                      "invalid_call": False, "exit_code": done.returncode,
                      "stderr": done.stderr.decode(errors="replace")[:2000]}
        else:
            result = strict_json(done.stdout.decode())
    except (subprocess.TimeoutExpired, ValueError, UnicodeError) as error:
        result = {"runtime_ok": False, "stdout": str(error)[:1000], "calls": [], "writes": {}, "invalid_call": False}
    result["process_wall_seconds"] = time.monotonic() - start
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", action="append", required=True, help="adapter name in build dir")
    parser.add_argument("--binary", action="append", default=[], help="NAME=PATH override")
    parser.add_argument("--ro", action="append", default=[], help="extra read-only path")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    overrides = dict(item.split("=", 1) for item in args.binary)
    samples = [s for s in json.loads(SAMPLES.read_text()) if s["language"] == "python"]
    report: dict[str, Any] = {"samples_sha256": hashlib.sha256(SAMPLES.read_bytes()).hexdigest(), "runtimes": {}}
    for runtime in args.runtime:
        binary = Path(overrides.get(runtime, BUILD / runtime))
        command = sandbox(binary, args.ro)
        programs = []
        for sample in samples:
            code = sample["attempts"][0]["generation"]["code"]
            original = sample["attempts"][0]["evaluation"]["passed"]
            variants = []
            for index in range(VARIANTS):
                case = fixture(sample["task"], index)
                observed = run(command, code, case["runtime"])
                variants.append({"variant": index, **score(observed, case), "observed": observed})
            passed = all(v["passed"] for v in variants)
            programs.append({"id": sample["id"], "task": sample["task"], "passed": passed,
                             "cpython_original_passed": original,
                             "code_sha256": hashlib.sha256(code.encode()).hexdigest(), "variants": variants})
            print(runtime, sample["id"], "PASS" if passed else "FAIL", flush=True)
        report["runtimes"][runtime] = {
            "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
            "programs_passed": sum(p["passed"] for p in programs),
            "variant_executions_passed": sum(v["passed"] for p in programs for v in p["variants"]),
            "programs": programs,
        }
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n")
    for runtime, data in report["runtimes"].items():
        print(f"{runtime}: {data['programs_passed']}/36 programs, {data['variant_executions_passed']}/108 variants")


if __name__ == "__main__":
    main()
