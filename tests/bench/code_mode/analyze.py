"""Reconcile the complete paired cohort and compute task-clustered intervals."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import random
import statistics
from typing import Any

from cases import LANGUAGES, REPEATS, TASKS, VARIANTS, fixture
from execute import BUILD, score
from policy import promote
from run import MODEL, atomic_json


def validate_cohort(rows: list[dict[str, Any]]) -> None:
    expected = {(language, task, repetition) for language in LANGUAGES
                for task, _ in TASKS for repetition in range(REPEATS)}
    actual = [(row["language"], row["task"], row["repetition"]) for row in rows]
    if len(actual) != len(expected) or set(actual) != expected:
        raise ValueError("missing, duplicate, extra or misclassified samples")
    for row in rows:
        if row["id"] != f"{row['task']}-{row['repetition']}-{row['language']}":
            raise ValueError("sample identity mismatch")
        attempts = row["attempts"]
        if len(attempts) not in (1, 2):
            raise ValueError("invalid attempt budget")
        if len(attempts) == 2 and attempts[0]["evaluation"]["passed"]:
            raise ValueError("successful first attempt was selectively retried")
        for attempt in attempts:
            generation = attempt["generation"]
            if generation["requested_model"] != MODEL or generation["effort"] != "low" or generation["generation_ok"] is not True:
                raise ValueError("wrong model/effort or incomplete generation")
            usage = generation["usage"]
            for key in ("input_tokens", "cached_input_tokens", "output_tokens"):
                if type(usage.get(key)) is not int or usage[key] < 0:
                    raise ValueError("invalid or missing measured usage")
            if usage["cached_input_tokens"] > usage["input_tokens"]:
                raise ValueError("cached input exceeds input")
            variants = attempt["evaluation"]["variants"]
            if [v["variant"] for v in variants] != list(range(VARIANTS)):
                raise ValueError("incomplete/duplicate variants")
            for variant in variants:
                recomputed = score(variant["observed"], fixture(row["task"], variant["variant"]))
                if any(recomputed[k] != variant[k] for k in recomputed):
                    raise ValueError("stored scoring differs from effect oracle")
            if attempt["evaluation"]["passed"] != all(v["passed"] for v in variants):
                raise ValueError("program did not pass every variant")
        if row["first_pass"] != attempts[0]["evaluation"]["passed"] or row["passed"] != attempts[-1]["evaluation"]["passed"]:
            raise ValueError("sample success does not match attempts")


def output_tokens(row: dict[str, Any]) -> int:
    return sum(a["generation"]["usage"]["output_tokens"] for a in row["attempts"])


def summarize(rows: list[dict[str, Any]]) -> dict[str, Any]:
    attempts = [a for row in rows for a in row["attempts"]]
    usages = [a["generation"]["usage"] for a in attempts]
    usage = {key: sum(u.get(key, 0) for u in usages) for key in
             ("input_tokens", "cached_input_tokens", "output_tokens", "reasoning_output_tokens")}
    usage["uncached_input_tokens"] = usage["input_tokens"] - usage["cached_input_tokens"]
    solved = sum(row["passed"] for row in rows)
    return {"samples": len(rows), "first_pass": sum(row["first_pass"] for row in rows),
            "eventual_pass": solved, "repairs": len(attempts) - len(rows), "usage": usage,
            "output_tokens_per_solved_task": usage["output_tokens"] / solved if solved else None,
            "source_bytes_mean_first_attempt": statistics.mean(len(r["attempts"][0]["generation"]["code"].encode()) for r in rows),
            "model_wall_seconds_median_per_task": statistics.median(sum(a["generation"]["wall_seconds"] for a in r["attempts"]) for r in rows),
            "model_wall_seconds_sum": sum(a["generation"]["wall_seconds"] for a in attempts),
            "nested_calls_total_all_variants_and_attempts": sum(len(v["observed"]["calls"]) for a in attempts for v in a["evaluation"]["variants"]),
            "runtime_ns_median_all_variants": statistics.median(v["observed"]["runtime_ns"] for a in attempts for v in a["evaluation"]["variants"] if "runtime_ns" in v["observed"])}


def quantile(values: list[float], p: float) -> float:
    ordered = sorted(values)
    index = (len(ordered) - 1) * p
    lower = math.floor(index)
    upper = math.ceil(index)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def paired_comparison(rows: list[dict[str, Any]], candidate: str) -> dict[str, Any]:
    grouped = {(language, task): [r for r in rows if r["language"] == language and r["task"] == task]
               for language in ("lua", candidate) for task, _ in TASKS}
    tasks = [task for task, _ in TASKS]
    rng = random.Random(20260927)
    ratios, success_differences = [], []
    for _ in range(10000):
        chosen = rng.choices(tasks, k=len(tasks))
        base = [row for task in chosen for row in grouped["lua", task]]
        alt = [row for task in chosen for row in grouped[candidate, task]]
        base_solved = sum(r["passed"] for r in base)
        alt_solved = sum(r["passed"] for r in alt)
        if not base_solved or not alt_solved:
            raise ValueError("undefined token-per-solved bootstrap sample")
        ratios.append((sum(map(output_tokens, alt)) / alt_solved) / (sum(map(output_tokens, base)) / base_solved))
        success_differences.append((sum(r["first_pass"] for r in alt) - sum(r["first_pass"] for r in base)) / len(base))
    base = summarize([r for r in rows if r["language"] == "lua"])
    alt = summarize([r for r in rows if r["language"] == candidate])
    token_ci = [quantile(ratios, p) for p in (0.025, 0.975)]
    success_ci = [quantile(success_differences, p) for p in (0.025, 0.975)]
    supported_gain = success_ci[0] > 0 or token_ci[1] <= 0.85
    return {"candidate": candidate,
            "output_tokens_per_solved_ratio": alt["output_tokens_per_solved_task"] / base["output_tokens_per_solved_task"],
            "output_ratio_task_clustered_95pct_ci": token_ci,
            "first_pass_difference": (alt["first_pass"] - base["first_pass"]) / base["samples"],
            "first_pass_difference_task_clustered_95pct_ci": success_ci,
            "supported_gain_under_preregistered_threshold": supported_gain,
            "eligible_even_if_full_production_parity_were_proven": promote(True, supported_gain, True,
                alt["first_pass"], base["first_pass"], alt["eventual_pass"], base["eventual_pass"]),
            "bootstrap": {"clusters": 12, "repetitions_within_cluster": 3, "draws": 10000, "seed": 20260927},
            "limitations": "Handcrafted families; few independent clusters. Intervals are exploratory, not population guarantees."}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=BUILD / "live/samples.json")
    parser.add_argument("--output", type=Path, default=BUILD / "analysis.json")
    args = parser.parse_args()
    rows = json.loads(args.input.read_text())
    validate_cohort(rows)
    report = {"model": MODEL, "effort": "low", "samples": len(rows),
              "languages": {lang: summarize([r for r in rows if r["language"] == lang]) for lang in LANGUAGES},
              "comparisons": {lang: paired_comparison(rows, lang) for lang in ("javascript", "python")},
              "failures": [{"id": r["id"], "first_pass": r["first_pass"], "passed": r["passed"],
                            "first_attempt_feedback": [v for v in r["attempts"][0]["evaluation"]["variants"] if not v["passed"]]}
                           for r in rows if not r["first_pass"]]}
    atomic_json(args.output, report)
    print(json.dumps({k: v for k, v in report.items() if k != "failures"}, indent=2))


if __name__ == "__main__":
    main()
