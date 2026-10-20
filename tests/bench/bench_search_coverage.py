#!/usr/bin/env python3
"""Deterministic search evidence, not a token or whole-task latency benchmark.

Runs baseline and candidate production binaries through both local provider APIs.
The fixture combines hidden configuration and distracting ordinary source trees;
there are no live credentials, network providers, or inference charges.
"""

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/integration"))
import test_execution_code_mode as fixture  # noqa: E402
from code_mode_fixture import python_string  # noqa: E402


def measure(binary, wire):
    fixture.TNY = str(Path(binary).resolve())
    case = fixture.ExecutionCodeMode(methodName="runTest")
    case.setUp()
    try:
        (case.workspace / ".github/workflows").mkdir(parents=True)
        (case.workspace / ".github/workflows/check.yml").write_text(
            "HIDDEN_CONFIG_TARGET\n"
        )
        (case.workspace / "src").mkdir()
        (case.workspace / "src/main.c").write_text("SCOPED_TARGET\n")
        (case.workspace / "other").mkdir()
        for index in range(100):
            (case.workspace / f"other/{index:03d}.c").write_text(
                "SCOPED_TARGET " + "irrelevant " * 20 + "\n"
            )
        outputs = {}
        for name, args in (
            ("hidden", {"pattern": "HIDDEN_CONFIG_TARGET"}),
            ("scoped", {"pattern": "SCOPED_TARGET", "include": "src/*"}),
        ):
            output = case.run_code(
                f"print(tools.call('grep_files', {python_string(json.dumps(args))}))",
                wire=wire,
            )
            match = re.search(r"^\[search (\{[^\n]+\})\]", output, re.MULTILINE)
            outputs[name] = {
                "result_bytes": len(output.encode()),
                "target_found": (
                    ".github/workflows/check.yml:1:HIDDEN_CONFIG_TARGET" in output
                    if name == "hidden"
                    else "src/main.c:1:SCOPED_TARGET" in output
                ),
                "distractor_rows": len(re.findall(r"(?m)^other/", output)),
                "coverage": json.loads(match.group(1)) if match else None,
            }
        return outputs
    finally:
        case.doCleanups()


def binary_identity(binary):
    path = Path(binary).resolve()
    identity = {"binary_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    root = subprocess.run(
        ["git", "-C", str(path.parent), "rev-parse", "--show-toplevel"],
        capture_output=True,
        text=True,
        check=False,
    )
    if root.returncode == 0:
        checkout = Path(root.stdout.strip())
        patch = subprocess.check_output(
            ["git", "-C", str(checkout), "diff", "HEAD", "--", "src"]
        )
        identity["checkout_at_measurement"] = {
            "commit": subprocess.check_output(
                ["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True
            ).strip(),
            "tracked_src_dirty": bool(patch),
            "tracked_src_diff_sha256": hashlib.sha256(patch).hexdigest(),
            "source_sha256": {
                name: hashlib.sha256((checkout / name).read_bytes()).hexdigest()
                for name in ("src/core/tools_fs.c", "src/core/tools.c")
            },
        }
    return identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    args = parser.parse_args()
    result = {
        "fixture": "one hidden config, one scoped target, 100 distracting source files",
        "metric_limits": "Result bytes are not tokenizer counts or task-quality measurements. Baseline scan count is unobservable.",
        "baseline_identity": binary_identity(args.baseline),
        "candidate_identity": binary_identity(args.candidate),
        "provenance_limits": "Checkout identity is captured at measurement time; binary hashes identify the actual executables tested. Build evidence must establish their source provenance.",
        "wires": {},
    }
    for wire in ("responses", "chat"):
        result["wires"][wire] = {
            "baseline": measure(args.baseline, wire),
            "candidate": measure(args.candidate, wire),
        }
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
