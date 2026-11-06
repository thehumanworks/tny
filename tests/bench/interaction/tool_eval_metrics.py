"""Summarize retained tool-evaluation traces without changing their scoring."""

import argparse
import json
import statistics
from pathlib import Path


def summarize(rows):
    result = {}
    for arm in dict.fromkeys(row["arm"] for row in rows):
        samples = [row for row in rows if row["arm"] == arm]
        attempts = [attempt for row in samples for attempt in row["attempts"]]
        usage = [attempt["generation"].get("usage") or {} for attempt in attempts]
        first = [row["attempts"][0] for row in samples]
        final = [row["attempts"][-1] for row in samples]
        successful = sum(row["passed"] for row in samples)
        inputs = sum(u.get("input_tokens", 0) for u in usage)
        outputs = sum(u.get("output_tokens", 0) for u in usage)

        def effects_ok(attempt):
            variants = attempt["evaluation"]["variants"]
            return bool(variants) and all(
                v["observed"].get("runtime_ok")
                and v["output_ok"]
                and v["trace_ok"]
                and v.get("retry_bound_ok", True)
                for v in variants
            )

        result[arm] = {
            "samples": len(samples),
            "first_pass": sum(row["first_pass"] for row in samples),
            "final_pass": successful,
            "first_correct_effects": sum(effects_ok(a) for a in first),
            "final_correct_effects": sum(effects_ok(a) for a in final),
            "requests_including_repairs": len(attempts),
            "input_tokens": inputs,
            "output_tokens": outputs,
            "reasoning_tokens_included_in_output": sum(
                u.get("output_tokens_details", {}).get("reasoning_tokens", 0)
                for u in usage
            ),
            "cached_input_tokens": sum(
                u.get("input_tokens_details", {}).get("cached_tokens", 0) for u in usage
            ),
            "usage_missing": sum(not u for u in usage),
            "tokens_per_success_including_failures": (inputs + outputs) / successful
            if successful
            else None,
            "median_first_output_tokens": statistics.median(
                a["generation"].get("usage", {}).get("output_tokens", 0) for a in first
            ),
            "executed_variants_including_repairs": sum(
                len(a["evaluation"]["variants"]) for a in attempts
            ),
            "invalid_call_variants": sum(
                v["observed"].get("invalid_call", False)
                for a in attempts
                for v in a["evaluation"]["variants"]
            ),
            "logical_tool_calls_all_variants_and_repairs": sum(
                len(v["observed"].get("calls", []))
                for a in attempts
                for v in a["evaluation"]["variants"]
            ),
            "tasks": {
                task: {
                    "first_pass": sum(
                        r["first_pass"] for r in samples if r["task"] == task
                    ),
                    "final_pass": sum(
                        r["passed"] for r in samples if r["task"] == task
                    ),
                }
                for task in dict.fromkeys(r["task"] for r in samples)
            },
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("samples", type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(json.loads(args.samples.read_text())), indent=2))


if __name__ == "__main__":
    main()
