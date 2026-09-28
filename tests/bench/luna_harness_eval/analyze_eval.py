"""Recompute metrics from raw request receipts; never invokes a model."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import math
import random
import statistics
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIELDS = (
    "input_tokens",
    "cached_input_tokens",
    "cache_write_tokens",
    "output_tokens",
    "reasoning_tokens",
)


def load(path):
    return json.loads(Path(path).read_text())


def percentile(values, p):
    values = sorted(values)
    i = (len(values) - 1) * p
    lo = math.floor(i)
    hi = math.ceil(i)
    return values[lo] + (values[hi] - values[lo]) * (i - lo)


def request_valid(row):
    if row.get("completion") != "response.completed" or row.get("http_status") != 200:
        return False
    if (
        row.get("requested_model") != "gpt-6-luna"
        or row.get("reported_model") != "gpt-6-luna"
        or row.get("requested_effort") != "low"
    ):
        return False
    if any(type(row.get(k)) is not int or row[k] < 0 for k in FIELDS):
        return False
    return (
        row["cached_input_tokens"] <= row["input_tokens"]
        and row["cache_write_tokens"] <= row["input_tokens"]
    )


def output_text(value):
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return "\n".join(
            v.get("text", "") if isinstance(v, dict) else str(v) for v in value
        )
    return json.dumps(value, ensure_ascii=False)


def inspect_run(path):
    result = load(path)
    folder = path.parent
    rows = [
        json.loads(line)
        for line in (folder / "proxy/requests.jsonl").read_text().splitlines()
    ]
    if rows != result["request_rows"]:
        raise ValueError("Proxy ledger mismatch " + str(path))
    valid = bool(rows) and all(request_valid(row) for row in rows)
    totals = {
        k: sum(r[k] for r in rows) if all(type(r.get(k)) is int for r in rows) else None
        for k in FIELDS
    }
    for field in FIELDS:
        if totals[field] != result[field]:
            raise ValueError("Usage aggregate mismatch " + field)
    calls = Counter()
    tool_outputs = {}
    input_bodies = []
    affinity_count = 0
    failures = []
    for row in rows:
        raw = gzip.decompress((folder / "proxy" / row["body_file"]).read_bytes())
        if hashlib.sha256(raw).hexdigest() != row["request_sha256"]:
            raise ValueError("Request hash mismatch")
        body = json.loads(raw)
        input_bodies.append(body)
        if body["model"] != "gpt-6-luna" or body["reasoning"]["effort"] != "low":
            raise ValueError("Wire configuration mismatch")
        affinity_count += bool(row.get("affinity_sent"))
        for item in row["output_items"]:
            calls[item.get("name") or item["type"]] += 1
        for item in body.get("input", []):
            if isinstance(item, dict) and item.get("type") in (
                "function_call_output",
                "custom_tool_call_output",
            ):
                text = output_text(item.get("output", ""))
                key = item.get("call_id", item.get("id"))
                if key not in tool_outputs:
                    tool_outputs[key] = text
                    if (
                        text.lstrip().startswith("error:")
                        or "Process exited with code 1" in text
                        or "SyntaxError:" in text
                    ):
                        failures.append({"call_id": key, "excerpt": text[:500]})
    if sum(calls.values()) != result["tool_calls"]:
        raise ValueError("Tool call aggregate mismatch")
    answer = (folder / "final_message.txt").read_text()
    facts = {
        k: result[k]
        for k in (
            "harness",
            "task",
            "tier",
            "rep",
            "pass",
            "status",
            "reason",
            "wall_s",
            "requests",
        )
    }
    facts.update(totals)
    facts.update(
        measurement_valid=valid,
        noncached_input_tokens=totals["input_tokens"] - totals["cached_input_tokens"]
        if valid
        else None,
        model_tool_calls=sum(calls.values()),
        tool_calls_by_name=dict(calls),
        unique_tool_output_bytes=sum(len(t.encode()) for t in tool_outputs.values()),
        first_request_input=rows[0]["input_tokens"] if rows else None,
        first_request_cached=rows[0]["cached_input_tokens"] if rows else None,
        later_input=sum(r["input_tokens"] for r in rows[1:]) if valid else None,
        later_cached=sum(r["cached_input_tokens"] for r in rows[1:]) if valid else None,
        static_tokens_estimate=result.get("static_prefix_tokens"),
        static_tokenizer=result.get("static_prefix_token_method"),
        final_answer_chars=len(answer),
        cache_keys=len({r["cache_key_hash"] for r in rows}),
        instruction_versions=len({r["instruction_sha256"] for r in rows}),
        tool_versions=len({r["tools_sha256"] for r in rows}),
        affinity_sent_requests=affinity_count,
        tool_error_indicators=failures,
        final_answer=answer,
        protected_files_changed=result["protected_files_changed"],
        relative_run=str(folder),
    )
    if valid and result["harness"] == "tny":
        reported = load(folder / "stdout.txt")["usage"]
        for key in ("input_tokens", "output_tokens", "cached_input_tokens"):
            if reported[key] != totals[key]:
                raise ValueError("Tny usage differs from wire " + key)
    if valid and result["harness"] == "codex":
        completed = [
            json.loads(line)
            for line in (folder / "stdout.txt").read_text().splitlines()
            if line.startswith("{")
        ]
        completions = [e for e in completed if e.get("type") == "turn.completed"]
        if len(completions) != 1:
            raise ValueError("Codex completion count mismatch")
        for key in ("input_tokens", "cached_input_tokens", "output_tokens"):
            if completions[0]["usage"][key] != totals[key]:
                raise ValueError("Codex usage differs from wire " + key)
    return facts


def summary(rows):
    valid = [r for r in rows if r["measurement_valid"]]
    out = {
        "runs": len(rows),
        "passes": sum(r["pass"] for r in rows),
        "measurement_complete": len(valid) == len(rows),
        "failures": sum(not r["pass"] for r in rows),
        "infrastructure_errors": sum(r["status"] == "error" for r in rows),
    }
    for field in (
        *FIELDS,
        "noncached_input_tokens",
        "model_tool_calls",
        "requests",
        "unique_tool_output_bytes",
    ):
        out[field] = sum(r[field] for r in valid) if len(valid) == len(rows) else None
    if valid:
        inp = sum(r["input_tokens"] for r in valid)
        cache = sum(r["cached_input_tokens"] for r in valid)
        out["cached_input_fraction"] = cache / inp if inp else None
        out["median_wall_s"] = statistics.median(r["wall_s"] for r in rows)
        out["mean_wall_s"] = statistics.mean(r["wall_s"] for r in rows)
        out["median_initial_input"] = statistics.median(
            r["first_request_input"] for r in valid
        )
        out["first_request_cache_fraction"] = sum(
            r["first_request_cached"] for r in valid
        ) / sum(r["first_request_input"] for r in valid)
        later = sum(r["later_input"] for r in valid)
        out["later_request_cache_fraction"] = (
            sum(r["later_cached"] for r in valid) / later if later else None
        )
    for field in (
        "input_tokens",
        "noncached_input_tokens",
        "output_tokens",
        "model_tool_calls",
        "requests",
    ):
        out[field + "_per_pass"] = (
            out[field] / out["passes"]
            if out.get(field) is not None and out["passes"]
            else None
        )
    return out


def paired(rows):
    tasks = sorted({r["task"] for r in rows})
    blocks = {task: [r for r in rows if r["task"] == task] for task in tasks}
    rng = random.Random(9282026)
    fields = (
        "input_tokens",
        "noncached_input_tokens",
        "output_tokens",
        "model_tool_calls",
        "requests",
        "wall_s",
    )
    draws = {field: [] for field in fields}
    quality = []
    for _ in range(10000):
        sample = [r for task in rng.choices(tasks, k=len(tasks)) for r in blocks[task]]
        a = [r for r in sample if r["harness"] == "tny"]
        b = [r for r in sample if r["harness"] == "codex"]
        for field in fields:
            denom = sum(r[field] for r in b)
            if denom:
                draws[field].append(sum(r[field] for r in a) / denom)
        quality.append(
            sum(r["pass"] for r in a) / len(a) - sum(r["pass"] for r in b) / len(b)
        )
    return {
        "resampling": "10000 task-clustered paired draws; all repetitions preserved within each task",
        "seed": 9282026,
        "ratio_tny_over_codex_95pct": {
            f: [percentile(v, 0.025), percentile(v, 0.975)] for f, v in draws.items()
        },
        "pass_rate_difference_95pct": [
            percentile(quality, 0.025),
            percentile(quality, 0.975),
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path)
    parser.add_argument("--partial", action="store_true")
    args = parser.parse_args()
    root = args.root or Path(load(HERE / "STATE.json")["temporary_root"])
    rows = [
        inspect_run(p)
        for p in sorted((root / "output/primary").glob("*/*/rep-*/result.json"))
    ]
    expected = {
        (h, p.name, rep)
        for h in ("tny", "codex")
        for p in (HERE / "tasks").iterdir()
        for rep in (1, 2)
    }
    identities = [(r["harness"], r["task"], r["rep"]) for r in rows]
    if len(identities) != len(set(identities)) or not set(identities) <= expected:
        raise ValueError("Duplicate/unexpected cohort keys")
    complete = set(identities) == expected
    if not complete and not args.partial:
        raise SystemExit(f"Incomplete cohort: {len(rows)}/{len(expected)}")
    report = {
        "runs": rows,
        "complete": complete,
        "model": "gpt-6-luna",
        "effort": "low",
        "summary": {
            h: summary([r for r in rows if r["harness"] == h]) for h in ("tny", "codex")
        },
        "tiers": {
            str(t): {
                h: summary([r for r in rows if r["harness"] == h and r["tier"] == t])
                for h in ("tny", "codex")
            }
            for t in sorted({r["tier"] for r in rows})
        },
        "tasks": {
            task: {
                h: summary([r for r in rows if r["harness"] == h and r["task"] == task])
                for h in ("tny", "codex")
            }
            for task in sorted({r["task"] for r in rows})
        },
    }
    if complete and all(r["measurement_valid"] for r in rows):
        report["paired_intervals"] = paired(rows)
    (root / ("analysis-partial.json" if args.partial else "analysis.json")).write_text(
        json.dumps(report, indent=2) + "\n"
    )
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
