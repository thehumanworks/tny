"""Audit the controlled cache experiment; actual token counters, no price assumptions."""

from __future__ import annotations

import argparse
import gzip
import json
import statistics
from pathlib import Path

from analyze_eval import request_valid


def aggregate(rows):
    fields = (
        "input_tokens",
        "cached_input_tokens",
        "cache_write_tokens",
        "output_tokens",
    )
    d = {key: sum(r[key] for r in rows) for key in fields}
    d["noncached_input_tokens"] = d["input_tokens"] - d["cached_input_tokens"]
    d["cache_fraction"] = (
        d["cached_input_tokens"] / d["input_tokens"] if d["input_tokens"] else None
    )
    d["requests"] = len(rows)
    d["requests_with_cache"] = sum(r["cached_input_tokens"] > 0 for r in rows)
    d["first_event_ms_median"] = (
        statistics.median(r["first_event_ms"] for r in rows) if rows else None
    )
    d["latency_ms_median"] = (
        statistics.median(r["elapsed_ms"] for r in rows) if rows else None
    )
    return d


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = args.root or Path(
        json.loads((here / "STATE.json").read_text())["temporary_root"]
    )
    runs = json.loads((root / "cache-results.json").read_text())
    expected = {
        (s, h, r)
        for s in ("conversation", "fresh")
        for h in ("tny", "codex")
        for r in (1, 2)
    }
    keys = [(r["scenario"], r["harness"], r["rep"]) for r in runs]
    if set(keys) != expected or len(keys) != len(expected):
        raise ValueError("Incomplete cache cohort")
    diagnostics = []
    for run in runs:
        folder = (
            root
            / "output/cache"
            / run["scenario"]
            / run["harness"]
            / f"rep-{run['rep']:02d}"
        )
        recorded = [
            json.loads(line)
            for line in (folder / "proxy/requests.jsonl").read_text().splitlines()
        ]
        if recorded != run["requests"]:
            raise ValueError("Cache raw-ledger mismatch")
        if len(run["turns"]) != 5 or [t["turn"] for t in run["turns"]] != list(
            range(1, 6)
        ):
            raise ValueError("Invalid cache turns")
        for row in recorded:
            if not request_valid(row):
                raise ValueError("Incomplete/wrong-model cache response")
            body = json.loads(
                gzip.decompress((folder / "proxy" / row["body_file"]).read_bytes())
            )
            serialized = json.dumps(body, ensure_ascii=False)
            if "Item 199:" not in serialized or "Item 000:" not in serialized:
                raise ValueError("Catalog truncated or absent")
        diagnostics.append(
            {
                "scenario": run["scenario"],
                "harness": run["harness"],
                "rep": run["rep"],
                "cache_keys": len({r["cache_key_hash"] for r in recorded}),
                "upstream_affinity_lengths": sorted(
                    {r.get("upstream_affinity_bytes", 0) for r in recorded}
                ),
                "affinity_sent_requests": sum(r["affinity_sent"] for r in recorded),
                "correct_turns": sum(t["correct"] for t in run["turns"]),
                "turn_wall_s": [t["wall_s"] for t in run["turns"]],
                "per_turn": [
                    {"turn": i, **aggregate([r for r in recorded if r["turn"] == i])}
                    for i in range(1, 6)
                ],
            }
        )
    summary = {}
    for scenario in ("conversation", "fresh"):
        summary[scenario] = {}
        for harness in ("tny", "codex"):
            selected = [
                r
                for run in runs
                if run["scenario"] == scenario and run["harness"] == harness
                for r in run["requests"]
            ]
            summary[scenario][harness] = {
                "all": aggregate(selected),
                "first_turn": aggregate([r for r in selected if r["turn"] == 1]),
                "after_first_turn": aggregate([r for r in selected if r["turn"] > 1]),
            }
    result = {
        "summary": summary,
        "diagnostics": diagnostics,
        "valid_requests": sum(len(r["requests"]) for r in runs),
        "correct_turns": sum(t["correct"] for r in runs for t in r["turns"]),
        "turns": sum(len(r["turns"]) for r in runs),
    }
    (root / "cache-analysis.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
