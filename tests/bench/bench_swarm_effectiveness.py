#!/usr/bin/env python3
"""Opt-in paired tny swarm evaluation: accepted outcomes first, compute second.

Uses the existing external task oracles and isolated trial runner. Raw sessions
and login references remain private. Publish result.json only after inspection.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import bench_swarm as bench
from swarm_cases import CASES


def trial_order(case_index: int, repetition: int) -> tuple[str, str]:
    return (
        ("baseline", "candidate")
        if (case_index + repetition) % 2
        else ("candidate", "baseline")
    )


def evidence_metrics(home: Path, events: Path) -> dict[str, Any]:
    """Observable protocol work, not a claim that a message improved the solution."""
    types: dict[str, int] = {}
    topics: set[str] = set()
    attempted: dict[str, int] = {}
    error_counts: dict[str, int] = {}
    for path in (home / ".tny/sessions").rglob("session.json"):
        value = json.loads(path.read_text())
        for message in value.get("messages", []):
            for call in message.get("tool_calls", []):
                name = call.get("function", {}).get("name", "unknown")
                attempted[name] = attempted.get(name, 0) + 1
    for path in [events, *(home / ".tny/jobs").rglob("*.log")]:
        for event in bench.records(path):
            if event.get("type") == "tool_end" and event.get("tool_ok") is False:
                name = event.get("tool_name", "unknown")
                error_counts[name] = error_counts.get(name, 0) + 1
    for path in (home / ".tny/jobs").rglob("mailbox.json"):
        value = json.loads(path.read_text())
        for message in value.get("messages", []):
            try:
                # Durable mailbox storage uses payload; API receipts expose text.
                envelope = json.loads(message.get("payload", message.get("text", "")))
            except (TypeError, ValueError):
                continue
            if not isinstance(envelope, dict):
                continue
            kind, topic = envelope.get("kind"), envelope.get("topic")
            if isinstance(kind, str):
                types[kind] = types.get(kind, 0) + 1
            if isinstance(topic, str):
                topics.add(topic)
    return {
        "tool_attempts_by_name": attempted,
        "tool_errors_by_name": error_counts,
        "typed_message_kinds": types,
        "distinct_typed_topics": len(topics),
        "interpretation": "Observed calls and messages are not evidence of acceptance or consensus.",
    }


def summarize(rows: list[dict[str, Any]]) -> dict[str, Any]:
    summary: dict[str, Any] = {}
    for case in sorted({r["case"] for r in rows}):
        summary[case] = {}
        for condition in ("baseline", "candidate"):
            selected = [
                r for r in rows if r["case"] == case and r["condition"] == condition
            ]
            if not selected:
                continue
            measured = [
                r
                for r in selected
                if isinstance(r.get("end_to_end_seconds"), (float, int))
            ]
            accepted = [r for r in measured if r.get("passed")]
            complete_usage = len(measured) == len(selected) and all(
                r["metrics"].get("usage_complete") for r in measured
            )
            summary[case][condition] = {
                "attempted_runs": len(selected),
                "accepted_runs": len(accepted),
                "infrastructure_failures": sum(
                    "infrastructure_error" in r for r in selected
                ),
                "median_seconds_all_observed": statistics.median(
                    r["end_to_end_seconds"] for r in measured
                )
                if measured
                else None,
                "median_seconds_accepted": statistics.median(
                    r["end_to_end_seconds"] for r in accepted
                )
                if accepted
                else None,
                "usage_complete": complete_usage,
                "median_input_tokens": statistics.median(
                    r["metrics"]["input_tokens"] for r in measured
                )
                if complete_usage
                else None,
                "median_output_tokens": statistics.median(
                    r["metrics"]["output_tokens"] for r in measured
                )
                if complete_usage
                else None,
                "median_cached_input_tokens": statistics.median(
                    r["metrics"]["cached_input_tokens"] for r in measured
                )
                if complete_usage
                and all(r["metrics"].get("cache_coverage_complete") for r in measured)
                else None,
                "collaborators_per_observed_run": [
                    r["metrics"]["launched_collaborators"] for r in measured
                ],
            }
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live", action="store_true")
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--baseline-definition", type=Path, required=True)
    parser.add_argument("--candidate-definition", type=Path, required=True)
    parser.add_argument(
        "--codex", type=Path, default=Path(shutil.which("codex") or "codex")
    )
    parser.add_argument(
        "--codex-home",
        type=Path,
        default=Path(os.environ.get("CODEX_HOME", str(Path.home() / ".codex"))),
    )
    parser.add_argument("--model", default="gpt-5.6-sol")
    parser.add_argument("--effort", choices=("medium", "high"), default="medium")
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=360)
    parser.add_argument("--agents", type=int, default=3)
    parser.add_argument("--max-steps", type=int, default=60)
    parser.add_argument(
        "--cases",
        nargs="+",
        choices=[c.name for c in CASES],
        default=[c.name for c in CASES],
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.live:
        parser.error("live evaluation requires explicit --live authorization")
    if not (
        1 <= args.repetitions <= 3
        and 30 <= args.timeout <= 900
        and 1 <= args.agents <= 16
        and 1 <= args.max_steps <= 200
    ):
        parser.error("invalid repetitions, timeout, participant count or step limit")
    if len(set(args.cases)) != len(args.cases):
        parser.error("duplicate cases are not separate repetitions")
    args.output = args.output.resolve()
    args.codex, args.codex_home = args.codex.resolve(), args.codex_home.resolve()
    if not (args.codex_home / "auth.json").is_file():
        parser.error(
            "existing Codex login required; no login is performed by the benchmark"
        )
    paths = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    definitions = {
        "baseline": args.baseline_definition.resolve(),
        "candidate": args.candidate_definition.resolve(),
    }
    hashes = {name: bench.digest(path) for name, path in paths.items()}
    definition_hashes = {name: bench.digest(path) for name, path in definitions.items()}
    # Validate/count before spending inference. Both arms must have equal capacity.
    for name in paths:
        checked = subprocess.run(
            [str(paths[name]), "swarm", "validate", str(definitions[name]), "--json"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        if (
            checked.returncode
            or json.loads(checked.stdout).get("participants") != args.agents
        ):
            parser.error(f"{name} definition invalid or participant count differs")
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    for name in paths:
        (args.output / name).mkdir(mode=0o700)
    result: dict[str, Any] = {
        "schema_version": 1,
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "model": args.model,
        "effort": args.effort,
        "participants_per_condition": args.agents,
        "timeout_seconds": args.timeout,
        "max_steps": args.max_steps,
        "binaries": {
            name: {
                "sha256": hashes[name],
                "version": subprocess.check_output(
                    [str(path), "--version"], text=True
                ).strip(),
            }
            for name, path in paths.items()
        },
        "definition_sha256": definition_hashes,
        "cases_sha256": bench.digest(Path(__file__).with_name("swarm_cases.py")),
        "platform": platform.platform(),
        "runs": [],
        "limitations": [
            "Small synthetic task ladder, not a statistical or real-repository superiority claim.",
            "Same model/effort, participant count, external tasks and oracle; binary and contribution protocol differ together.",
            "No claim that topology, tool ergonomics or context changes alone caused any observed difference.",
            "Order alternates across cases and repetitions. Workspaces and state are fresh; provider cache state is uncontrolled.",
            "Input includes cached input. Missing usage is unknown, never zero. Cost is not acceptance.",
            "Failed/timeout trials remain. No message count or terminal success substitutes for external correctness.",
            "Raw logs, authentication references and generated workspaces remain private.",
        ],
    }
    bench.atomic_json(args.output / "result.json", result)
    for case_index, case in enumerate(c for c in CASES if c.name in args.cases):
        for repetition in range(1, args.repetitions + 1):
            for condition in trial_order(case_index, repetition):
                if any(
                    bench.digest(paths[name]) != hashes[name]
                    or bench.digest(definitions[name]) != definition_hashes[name]
                    for name in paths
                ):
                    raise RuntimeError(
                        "frozen binary or definition changed; evaluation stopped"
                    )
                trial_args = argparse.Namespace(**vars(args))
                trial_args.tny = paths[condition]
                trial_args.swarm_file = definitions[condition]
                trial_args.output = args.output / condition
                trial_args.activation = None
                try:
                    row = bench.run_trial(
                        trial_args, case, "tny_swarm", repetition, hashes[condition]
                    )
                    sample = trial_args.output / row["artifacts"]
                    row["protocol_observations"] = evidence_metrics(
                        sample / "home", sample / "events.jsonl"
                    )
                    row["declared_participation_observed"] = (
                        row["metrics"]["launched_collaborators"] == args.agents
                    )
                    row["passed"] = (
                        row["passed"] and row["declared_participation_observed"]
                    )
                    row.pop("command", None)
                    row["artifacts"] = f"{condition}/{row['artifacts']}"
                except Exception as exc:
                    # Preserve infrastructure failures without copying raw provider diagnostics.
                    row = {
                        "case": case.name,
                        "repetition": repetition,
                        "passed": False,
                        "infrastructure_error": type(exc).__name__,
                    }
                    row["artifacts"] = (
                        f"{condition}/{case.name}-r{repetition}-tny_swarm"
                    )
                row["condition"] = condition
                result["runs"].append(row)
                result["summary"] = summarize(result["runs"])
                bench.atomic_json(args.output / "result.json", result)
                print(
                    json.dumps(
                        {
                            key: row.get(key)
                            for key in (
                                "condition",
                                "case",
                                "passed",
                                "end_to_end_seconds",
                                "infrastructure_error",
                            )
                        }
                    ),
                    flush=True,
                )
                if "infrastructure_error" in row:
                    result["aborted_utc"] = datetime.now(timezone.utc).isoformat()
                    result["abort_reason"] = (
                        "Trial infrastructure failed; retained state needs inspection before another run."
                    )
                    bench.atomic_json(args.output / "result.json", result)
                    raise SystemExit(2)
    result["completed_utc"] = datetime.now(timezone.utc).isoformat()
    bench.atomic_json(args.output / "result.json", result)


if __name__ == "__main__":
    main()
