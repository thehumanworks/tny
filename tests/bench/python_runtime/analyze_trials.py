"""Aggregate held-out trials and apply the preregistered selection gate.

All attempts, failures and repairs stay in the denominators. The bootstrap
resamples task families (all repetitions of a family together), 10,000 draws,
seed 20260927, exactly as PR #197 did.
"""

from __future__ import annotations

import argparse
import json
import random
import statistics
from pathlib import Path
from typing import Any

import policy

SEED = 20260927
DRAWS = 10000


def arm_rows(samples: list[dict[str, Any]], arm: str) -> list[dict[str, Any]]:
    return [s for s in samples if s["arm"] == arm]


def usage(rows: list[dict[str, Any]], key: str) -> int:
    return sum(
        (a["generation"].get("usage") or {}).get(key, 0)
        for r in rows
        for a in r["attempts"]
    )


def summary(rows: list[dict[str, Any]]) -> dict[str, Any]:
    solved = sum(r["passed"] for r in rows)
    output = usage(rows, "output_tokens")
    walls = [sum(a["generation"]["wall_seconds"] for a in r["attempts"]) for r in rows]
    first_bytes = [len(r["attempts"][0]["generation"]["code"].encode()) for r in rows]
    return {
        "samples": len(rows),
        "complete": all(
            a["generation"]["generation_ok"] for r in rows for a in r["attempts"]
        ),
        "first_pass": sum(r["first_pass"] for r in rows),
        "final": solved,
        "repairs": sum(len(r["attempts"]) - 1 for r in rows),
        "generations": sum(len(r["attempts"]) for r in rows),
        "output_tokens": output,
        "output_tokens_per_solved": output / solved if solved else None,
        "input_tokens": usage(rows, "input_tokens"),
        "cached_input_tokens": usage(rows, "cached_input_tokens"),
        "uncached_input_tokens": usage(rows, "input_tokens")
        - usage(rows, "cached_input_tokens"),
        "reasoning_output_tokens": usage(rows, "reasoning_output_tokens"),
        "median_generation_seconds_per_task": statistics.median(walls),
        "mean_first_attempt_source_bytes": statistics.mean(first_bytes),
    }


def bootstrap(samples: list[dict[str, Any]], base: str, other: str) -> dict[str, Any]:
    families = sorted({s["task"] for s in samples})

    def ratio(chosen: list[str]) -> tuple[float, float] | None:
        rows_b = [
            s for f in chosen for s in samples if s["task"] == f and s["arm"] == base
        ]
        rows_o = [
            s for f in chosen for s in samples if s["task"] == f and s["arm"] == other
        ]
        sb, so = sum(r["passed"] for r in rows_b), sum(r["passed"] for r in rows_o)
        if not sb or not so:
            return None
        tb, to = (
            usage(rows_b, "output_tokens") / sb,
            usage(rows_o, "output_tokens") / so,
        )
        first = (
            sum(r["first_pass"] for r in rows_o) - sum(r["first_pass"] for r in rows_b)
        ) / len(rows_b)
        return 1 - to / tb, first

    observed = ratio(families)
    rng = random.Random(SEED)
    savings, firsts = [], []
    for _ in range(DRAWS):
        value = ratio([rng.choice(families) for _ in families])
        if value:
            savings.append(value[0])
            firsts.append(value[1])
    savings.sort()
    firsts.sort()

    def interval(values: list[float]) -> list[float]:
        return [values[int(0.025 * len(values))], values[int(0.975 * len(values)) - 1]]

    return {
        "baseline": base,
        "candidate": other,
        "observed_output_token_saving": observed[0] if observed else None,
        "saving_95": interval(savings),
        "observed_first_pass_difference": observed[1] if observed else None,
        "first_pass_difference_95": interval(firsts),
        "draws": len(savings),
    }


def failures(samples: list[dict[str, Any]]) -> list[dict[str, Any]]:
    out = []
    for s in samples:
        for index, attempt in enumerate(s["attempts"]):
            if attempt["evaluation"]["passed"]:
                continue
            variants = attempt["evaluation"].get("variants", [])
            bad = [v for v in variants if not v["passed"]]
            out.append(
                {
                    "id": s["id"],
                    "attempt": index,
                    "variants_failed": [v["variant"] for v in bad],
                    "execution_ok": [v["execution_ok"] for v in bad],
                    "output_ok": [v["output_ok"] for v in bad],
                    "trace_ok": [v["trace_ok"] for v in bad],
                    "stdout": bad[0]["observed"].get("stdout", "")[:400]
                    if bad
                    else "generation failed",
                }
            )
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument(
        "--corpus",
        type=Path,
        required=True,
        help="replay JSON containing the lighter runtime",
    )
    parser.add_argument("--build", type=Path, required=True, help="probe build.json")
    parser.add_argument("--lighter", default="monty")
    parser.add_argument(
        "--semantics-ok",
        action="store_true",
        help="production semantics verified for the lighter runtime",
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    samples = json.loads(args.input.read_text())
    arms = sorted({s["arm"] for s in samples})
    report: dict[str, Any] = {
        "arms": {arm: summary(arm_rows(samples, arm)) for arm in arms}
    }
    report["paired"] = [
        bootstrap(samples, "cpython", arm) for arm in arms if arm != "cpython"
    ]
    report["failures"] = failures(samples)
    corpus = json.loads(args.corpus.read_text())["runtimes"][args.lighter]
    build = json.loads(args.build.read_text())["binaries"]
    lighter, cpython = report["arms"][args.lighter], report["arms"]["cpython"]
    inputs = {
        "corpus_passed": corpus["programs_passed"],
        "corpus_total": len(corpus["programs"]),
        "trials_complete": lighter["complete"]
        and cpython["complete"]
        and lighter["samples"] == cpython["samples"] == 36,
        "first_lighter": lighter["first_pass"],
        "first_cpython": cpython["first_pass"],
        "final_lighter": lighter["final"],
        "final_cpython": cpython["final"],
        "tokens_lighter": lighter["output_tokens"],
        "solved_lighter": lighter["final"],
        "tokens_cpython": cpython["output_tokens"],
        "solved_cpython": cpython["final"],
        "semantics_ok": args.semantics_ok,
        "bytes_lighter": build[args.lighter]["bytes"],
        "bytes_cpython": build["cpython_static"]["bytes"],
    }
    report["selection"] = {
        "inputs": inputs,
        "select_lighter": policy.select_lighter(**inputs),
    }
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "failures"}, indent=1))


if __name__ == "__main__":
    main()
