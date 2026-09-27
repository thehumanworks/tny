"""Replay every held-out trial attempt on another executor and compare.

Runtime evidence only (no generation): each CPython-arm attempt's recorded
pass/fail under the trial's stock-CPython executor is compared with the same
unmodified code on the given executor (normally the production cell path).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import heldout
from replay import BUILD, run, sandbox


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=Path, required=True)
    parser.add_argument("--executor", default="production")
    parser.add_argument("--arms", default="cpython,cpython_pr197")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    arms = set(args.arms.split(","))
    command = sandbox(BUILD / args.executor, [])
    rows = []
    for sample in json.loads(args.samples.read_text()):
        if sample["arm"] not in arms:
            continue
        for index, attempt in enumerate(sample["attempts"]):
            code = attempt["generation"]["code"]
            variants = []
            for v in range(heldout.VARIANTS):
                case = heldout.fixture(sample["task"], v)
                observed = run(command, code, case["runtime"])
                variants.append(
                    heldout.score(sample["task"], observed, case)
                    | {"stdout": observed.get("stdout", "")[:300]}
                )
            passed = all(x["passed"] for x in variants)
            rows.append(
                {
                    "id": sample["id"],
                    "attempt": index,
                    "recorded": attempt["evaluation"]["passed"],
                    "replayed": passed,
                    "variants": variants,
                }
            )
    same = sum(r["recorded"] == r["replayed"] for r in rows)
    report = {
        "executor": args.executor,
        "attempts": len(rows),
        "same_outcome": same,
        "rows": rows,
    }
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n")
    print(
        f"{args.executor}: {same}/{len(rows)} attempts reproduce the recorded outcome"
    )
    for r in rows:
        if r["recorded"] != r["replayed"]:
            print(
                "DIFF",
                r["id"],
                r["attempt"],
                r["recorded"],
                r["replayed"],
                r["variants"][0]["stdout"][:200],
            )


if __name__ == "__main__":
    main()
