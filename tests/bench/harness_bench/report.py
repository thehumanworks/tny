#!/usr/bin/env python3
"""Aggregate cross-harness result files into Markdown and JSON reports."""

import argparse
import gzip
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path

from compare import compare_runs
from compare import markdown as comparison_markdown
from cost import PRICE_DATE, request_cost
from proxy import static_parts


def _counter():
    try:
        import tiktoken  # noqa: PLC0415
    except ImportError:
        return lambda value: len(value) / 4, "chars/4 estimate"
    encoding = tiktoken.get_encoding("o200k_base")
    return lambda value: len(encoding.encode(value)), "tiktoken o200k_base"


def _fmt(value, digits=0):
    if value is None:
        return "n/a"
    return f"{value:,.{digits}f}"


def _mean(values):
    return statistics.mean(values) if values else None


def _wilson(passed, runs):
    z = 1.96
    rate = passed / runs
    denominator = 1 + z * z / runs
    center = (rate + z * z / (2 * runs)) / denominator
    half = (
        z
        * math.sqrt(rate * (1 - rate) / runs + z * z / (4 * runs * runs))
        / denominator
    )
    return [max(0, center - half), min(1, center + half)]


def _section_tokens(result, count):
    rows = result.get("request_rows", [])
    result_path = Path(result["_path"])
    totals = defaultdict(float)
    for row in rows:
        body_file = result_path.parent / "proxy" / row["body_file"]
        body = json.loads(gzip.decompress(body_file.read_bytes()))
        instructions, tools = static_parts(body)
        totals["instructions"] += sum(
            count(
                part if isinstance(part, str) else json.dumps(part, ensure_ascii=False)
            )
            for part in instructions
        )
        totals["tools"] += count(
            json.dumps(tools, ensure_ascii=False, separators=(",", ":"))
        )
        items = body.get("input") or []
        if not isinstance(items, list):
            items = [items]
        for item in items:
            if isinstance(item, dict) and item.get("role") in {"developer", "system"}:
                continue
            if isinstance(item, dict) and item.get("type") in {
                "function_call_output",
                "custom_tool_call_output",
            }:
                totals["tool_outputs"] += count(
                    json.dumps(item.get("output", ""), ensure_ascii=False)
                )
            else:
                totals["history"] += count(json.dumps(item, ensure_ascii=False))
    if rows:
        first_body = json.loads(
            gzip.decompress(
                (result_path.parent / "proxy" / rows[0]["body_file"]).read_bytes()
            )
        )
        instructions, tools = static_parts(first_body)
        totals["static_prefix"] = sum(
            count(
                part if isinstance(part, str) else json.dumps(part, ensure_ascii=False)
            )
            for part in instructions
        ) + count(json.dumps(tools, ensure_ascii=False, separators=(",", ":")))
    return dict(totals)


def aggregate(results):
    count, token_method = _counter()
    by_harness = defaultdict(list)
    tasks = defaultdict(dict)
    errors = []
    for result in results:
        status = result.get("status", "pass" if result["pass"] else "fail")
        if status not in {"pass", "fail", "error"}:
            raise ValueError(f"invalid run status: {status}")
        result["_status"] = status
        if status == "error":
            errors.append(
                {
                    "harness": result["harness"],
                    "task": result["task"],
                    "rep": result.get("rep"),
                    "reason": result.get("reason", "error: unspecified"),
                }
            )
        costs = [
            request_cost(row, result["model"]) for row in result.get("request_rows", [])
        ]
        result["ite"] = (
            sum(cost[0] for cost in costs)
            if costs and all(cost[0] is not None for cost in costs)
            else None
        )
        result["usd"] = (
            sum(cost[1] for cost in costs)
            if costs and all(cost[1] is not None for cost in costs)
            else None
        )
        result["_sections"] = _section_tokens(result, count)
        by_harness[(result["harness"], result["model"])].append(result)
        tasks[result["task"]].setdefault(result["harness"], []).append(result)
    headline = []
    breakdown = []
    for (harness, model), rows in sorted(by_harness.items()):
        eligible = [row for row in rows if row["_status"] != "error"]
        costs = [row["ite"] for row in eligible if row.get("ite") is not None]
        dollars = [row["usd"] for row in eligible if row.get("usd") is not None]
        input_total = sum(
            row["input_tokens"]
            for row in eligible
            if row.get("input_tokens") is not None
        )
        cached_total = sum(
            row["cached_input_tokens"]
            for row in eligible
            if row.get("cached_input_tokens") is not None
        )
        successes = sum(row["pass"] for row in eligible)
        headline.append(
            {
                "harness": harness,
                "model": model,
                "runs": len(rows),
                "evaluated_runs": len(eligible),
                "error_runs": len(rows) - len(eligible),
                "pass_rate": successes / len(eligible) if eligible else None,
                "pass_rate_ci95": _wilson(successes, len(eligible))
                if eligible
                else [None, None],
                "ite_per_task": _mean(costs) if len(costs) == len(eligible) else None,
                "ite_sd": statistics.stdev(costs)
                if len(costs) == len(eligible) and len(costs) > 1
                else None,
                "ite_per_passed_task": sum(costs) / successes
                if successes and len(costs) == len(eligible)
                else None,
                "usd_per_task": _mean(dollars)
                if len(dollars) == len(eligible)
                else None,
                "usd_per_passed_task": sum(dollars) / successes
                if successes and len(dollars) == len(eligible)
                else None,
                "requests_per_task": _mean([row["requests"] for row in eligible]),
                "turns_completed_per_task": _mean(
                    [row.get("turns_completed", 1) for row in eligible]
                ),
                "turns_requested_per_task": _mean(
                    [row.get("turns_requested", 1) for row in eligible]
                ),
                "cache_hit": cached_total / input_total
                if input_total
                and all(row.get("cached_input_tokens") is not None for row in eligible)
                else None,
                "mean_context_tokens": input_total
                / sum(row["requests"] for row in eligible)
                if input_total
                and all(row.get("input_tokens") is not None for row in eligible)
                else None,
                "static_prefix_tokens": _mean(
                    [row["_sections"].get("static_prefix", 0) for row in eligible]
                ),
                "p50_wall_s": statistics.median(row["wall_s"] for row in eligible)
                if eligible
                else None,
            }
        )
        breakdown.append(
            {
                "harness": harness,
                "model": model,
                **{
                    key: sum(row["_sections"].get(key, 0) for row in eligible)
                    / max(1, sum(row["requests"] for row in eligible))
                    for key in ("instructions", "tools", "history", "tool_outputs")
                },
            }
        )
    matrix = {
        task: {
            harness: {
                "passed": sum(row["pass"] for row in rows if row["_status"] != "error"),
                "runs": sum(row["_status"] != "error" for row in rows),
                "errors": sum(row["_status"] == "error" for row in rows),
                "turns_completed": sum(row.get("turns_completed", 1) for row in rows),
                "turns_requested": sum(row.get("turns_requested", 1) for row in rows),
            }
            for harness, rows in harnesses.items()
        }
        for task, harnesses in sorted(tasks.items())
    }
    return {
        "token_method": token_method,
        "price_date": PRICE_DATE,
        "headline": headline,
        "pass_matrix": matrix,
        "errors": errors,
        "section_breakdown_per_request": breakdown,
    }


def markdown(report):
    lines = [
        "# Cross-harness benchmark",
        "",
        f"Token method: {report['token_method']}. Standard list prices as of {report['price_date']}; subscription dollars are comparison units, not a bill.",
        "",
        "| Harness | Model | Pass rate (95% CI) | Errors | ITE/task (SD) | ITE/passed | USD/task | USD/passed | Requests/task | Turns completed/requested | Cache hit | Mean context tokens | Static prefix | p50 wall |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in report["headline"]:
        lines.append(
            "| "
            + " | ".join(
                (
                    row["harness"],
                    row["model"],
                    f"{row['pass_rate']:.0%} ({row['pass_rate_ci95'][0]:.0%}–{row['pass_rate_ci95'][1]:.0%})"
                    if row["pass_rate"] is not None
                    else "n/a",
                    str(row["error_runs"]),
                    f"{_fmt(row['ite_per_task'])} ({_fmt(row['ite_sd'])})",
                    _fmt(row["ite_per_passed_task"]),
                    _fmt(row["usd_per_task"], 4),
                    _fmt(row["usd_per_passed_task"], 4),
                    _fmt(row["requests_per_task"], 1),
                    f"{_fmt(row['turns_completed_per_task'], 1)}/{_fmt(row['turns_requested_per_task'], 1)}",
                    f"{row['cache_hit']:.1%}"
                    if row["cache_hit"] is not None
                    else "n/a",
                    _fmt(row["mean_context_tokens"]),
                    _fmt(row["static_prefix_tokens"]),
                    _fmt(row["p50_wall_s"], 1) + "s"
                    if row["p50_wall_s"] is not None
                    else "n/a",
                )
            )
            + " |"
        )
    lines += ["", "## Pass matrix", ""]
    harnesses = sorted({h for task in report["pass_matrix"].values() for h in task})
    lines.append("| Task | " + " | ".join(harnesses) + " |")
    lines.append("| --- | " + " | ".join("---:" for _ in harnesses) + " |")
    for task, values in report["pass_matrix"].items():
        lines.append(
            "| "
            + task
            + " | "
            + " | ".join(
                f"{values[h]['passed']}/{values[h]['runs']}"
                + (
                    f" ({values[h]['turns_completed']}/{values[h]['turns_requested']} turns)"
                    if values[h]["turns_requested"] != values[h]["runs"]
                    else ""
                )
                + (f" (+{values[h]['errors']} error)" if values[h]["errors"] else "")
                if h in values
                else "—"
                for h in harnesses
            )
            + " |"
        )
    lines += ["", "## Verification and environment errors", ""]
    if report["errors"]:
        lines += ["| Harness | Task | Rep | Reason |", "| --- | --- | ---: | --- |"]
        for row in report["errors"]:
            lines.append(
                f"| {row['harness']} | {row['task']} | {row['rep'] or '—'} | "
                f"{row['reason'].replace('|', '/')} |"
            )
    else:
        lines.append("None.")
    lines += [
        "",
        "## Request section breakdown",
        "",
        "Mean tokens per request. Tool outputs are excluded from history.",
        "",
        "| Harness | Instructions | Tools | History | Tool outputs |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for row in report["section_breakdown_per_request"]:
        lines.append(
            "| "
            + " | ".join(
                (
                    row["harness"],
                    _fmt(row["instructions"]),
                    _fmt(row["tools"]),
                    _fmt(row["history"]),
                    _fmt(row["tool_outputs"]),
                )
            )
            + " |"
        )
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "input",
        nargs="?",
        type=Path,
        help="run label directory containing result.json files",
    )
    parser.add_argument(
        "--compare",
        nargs=2,
        metavar=("ARM_A", "ARM_B"),
        type=Path,
        help="compare complete task/rep pairs from two run directories",
    )
    parser.add_argument("--harness", help="select one harness from each comparison arm")
    parser.add_argument(
        "--margin",
        type=float,
        default=-8.0,
        help="non-inferiority margin in percentage points (default: -8)",
    )
    parser.add_argument(
        "--fire",
        action="append",
        default=[],
        metavar="NAME=REGEX",
        help="count regex matches in decompressed request bodies (repeatable)",
    )
    parser.add_argument("--out", type=Path, required=True, help="Markdown report path")
    args = parser.parse_args()
    if args.compare:
        if args.input:
            parser.error("positional input cannot be combined with --compare")
        try:
            report = compare_runs(
                *args.compare, harness=args.harness, margin=args.margin, fires=args.fire
            )
        except (OSError, ValueError) as error:
            parser.error(str(error))
        rendered = comparison_markdown(report)
    else:
        if args.input is None:
            parser.error("input directory or --compare is required")
        if args.harness or args.fire or args.margin != -8.0:
            parser.error("--harness, --fire, and --margin require --compare")
        paths = sorted(args.input.rglob("result.json"))
        if not paths:
            parser.error("no result.json files found")
        results = []
        for path in paths:
            row = json.loads(path.read_text())
            row["_path"] = str(path)
            results.append(row)
        report = aggregate(results)
        rendered = markdown(report)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(rendered)
    args.out.with_suffix(".json").write_text(
        json.dumps(report, indent=2, allow_nan=False) + "\n"
    )
    print(rendered)


if __name__ == "__main__":
    main()
