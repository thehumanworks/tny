"""Run isolated semantic cases on candidate runtimes and compare with CPython.

Usage: conformance.py --runtime NAME=COMMAND ... --output FILE
COMMAND is split on spaces; the case file path is appended. The reference is
the pinned CPython 3.14 interpreter given by --reference. A case matches only
when stdout is byte-identical and success/failure status is identical.
Stderr text is recorded but not compared (tracebacks legitimately differ).
"""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from pathlib import Path

from semantics import CASES


def run(command: list[str], path: Path) -> dict:
    try:
        done = subprocess.run(
            [*command, str(path)],
            capture_output=True,
            timeout=10,
            env={"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"},
            check=False,
        )
        return {
            "ok": done.returncode == 0,
            "exit": done.returncode,
            "stdout": done.stdout.decode("utf-8", "replace"),
            "stderr": done.stderr.decode("utf-8", "replace")[-400:],
        }
    except subprocess.TimeoutExpired:
        return {"ok": False, "exit": None, "stdout": "", "stderr": "timeout"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--runtime", action="append", default=[])
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    runtimes = dict(item.split("=", 1) for item in args.runtime)
    report = {"reference": args.reference, "cases": {}, "summary": {}}
    with tempfile.TemporaryDirectory(prefix="tny-py-conformance-") as tmp:
        for name, code in CASES.items():
            path = Path(tmp) / f"{name}.py"
            path.write_text(code + "\n")
            reference = run(args.reference.split(), path)
            entry = {"code": code, "reference": reference, "candidates": {}}
            for runtime, command in runtimes.items():
                observed = run(command.split(), path)
                observed["match"] = (
                    observed["ok"] == reference["ok"]
                    and observed["stdout"] == reference["stdout"]
                )
                entry["candidates"][runtime] = observed
            report["cases"][name] = entry
    for runtime in runtimes:
        failed = [n for n, e in report["cases"].items() if not e["candidates"][runtime]["match"]]
        report["summary"][runtime] = {
            "matched": len(CASES) - len(failed),
            "total": len(CASES),
            "mismatched": failed,
        }
    Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n")
    for runtime, summary in report["summary"].items():
        print(f"{runtime}: {summary['matched']}/{summary['total']} match CPython")
        print("  mismatched:", " ".join(summary["mismatched"]))


if __name__ == "__main__":
    main()
