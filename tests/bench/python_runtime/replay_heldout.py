"""Replay every held-out trial attempt on another executor and compare.

Runtime evidence only (no generation): each CPython-arm attempt's recorded
pass/fail under the trial's stock-CPython executor is compared with the same
unmodified code on the given executor (normally the production cell path).
"""

from __future__ import annotations

import argparse
import hashlib
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
    parser.add_argument(
        "--require-match",
        action="store_true",
        help="Fail on any recorded/replayed variant mismatch",
    )
    args = parser.parse_args()
    arms = set(args.arms.split(","))
    command = sandbox(BUILD / args.executor, [])
    rows = []
    variant_matches = 0
    variant_total = 0
    for sample in json.loads(args.samples.read_text()):
        if sample["arm"] not in arms:
            continue
        for index, attempt in enumerate(sample["attempts"]):
            code = attempt["generation"]["code"]
            variants = []
            for v in range(heldout.VARIANTS):
                case = heldout.fixture(sample["task"], v)
                observed = run(command, code, case["runtime"])
                scored = heldout.score(sample["task"], observed, case)
                recorded = attempt["evaluation"]["variants"][v]["passed"]
                variants.append(scored | {"recorded": recorded, "observed": observed})
                variant_matches += scored["passed"] == recorded
                variant_total += 1
            passed = all(x["passed"] for x in variants)
            rows.append(
                {
                    "id": sample["id"],
                    "arm": sample["arm"],
                    "attempt": index,
                    "last_attempt": index == len(sample["attempts"]) - 1,
                    "code_sha256": hashlib.sha256(code.encode()).hexdigest(),
                    "recorded": attempt["evaluation"]["passed"],
                    "replayed": passed,
                    "variants": variants,
                }
            )
    same = sum(r["recorded"] == r["replayed"] for r in rows)
    report = {
        "executor": args.executor,
        "binary_sha256": hashlib.sha256(
            (BUILD / args.executor).read_bytes()
        ).hexdigest(),
        "samples_sha256": hashlib.sha256(args.samples.read_bytes()).hexdigest(),
        "variant_matches": variant_matches,
        "variant_total": variant_total,
        "by_arm": {
            arm: {
                "programs": sum(r["attempt"] == 0 for r in rows if r["arm"] == arm),
                "first_pass": sum(
                    r["replayed"] for r in rows if r["arm"] == arm and r["attempt"] == 0
                ),
                "final_pass": sum(
                    r["replayed"] for r in rows if r["arm"] == arm and r["last_attempt"]
                ),
            }
            for arm in sorted(arms)
        },
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
                r["variants"][0]["observed"].get("stdout", "")[:200],
            )
    print(json.dumps(report["by_arm"], indent=2))
    if args.require_match and (same != len(rows) or variant_matches != variant_total):
        raise SystemExit("production replay differs from recorded outcomes")


if __name__ == "__main__":
    main()
