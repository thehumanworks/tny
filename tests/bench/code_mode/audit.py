"""Independent cohort/effect/usage audit, optionally replaying every frozen cell."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from typing import Any

from cases import LANGUAGES, REPEATS, TASKS, fixture
from execute import BUILD, evaluate
from run import MODEL, atomic_json


def canonical(value: Any) -> Any:
    if isinstance(value, dict):
        return ("object", tuple(sorted((k, canonical(v)) for k, v in value.items())))
    if isinstance(value, list):
        return ("array", tuple(canonical(v) for v in value))
    if type(value) in (int, float):
        return ("number", value)
    return (type(value).__name__, value)


def independent_pass(observed: dict[str, Any], case: dict[str, Any]) -> bool:
    if (
        observed.get("runtime_ok") is not True
        or observed.get("invalid_call") is not False
    ):
        return False
    if observed.get("stdout") != "done\n" or not isinstance(
        observed.get("calls"), list
    ):
        return False
    if len(observed["calls"]) > 64:
        return False
    if not all(
        any(canonical(call) == canonical(required) for call in observed["calls"])
        for required in case["required_calls"]
    ):
        return False
    writes = observed.get("writes")
    if not isinstance(writes, dict) or sorted(writes) != sorted(case["expected"]):
        return False
    for path, expected in case["expected"].items():
        try:
            actual = (
                json.loads(writes[path]) if path in case["json_paths"] else writes[path]
            )
        except (TypeError, ValueError):
            return False
        if canonical(actual) != canonical(expected):
            return False
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=BUILD / "live")
    parser.add_argument("--replay", action="store_true")
    args = parser.parse_args()
    rows = json.loads((args.directory / "samples.json").read_text())
    expected = {
        f"{task}-{r}-{lang}"
        for task, _ in TASKS
        for r in range(REPEATS)
        for lang in LANGUAGES
    }
    identities = Counter(row["id"] for row in rows)
    if set(identities) != expected or any(count != 1 for count in identities.values()):
        raise ValueError("independent identity audit failed")
    totals = {
        lang: Counter(
            samples=0,
            first_pass=0,
            eventual_pass=0,
            attempts=0,
            input_tokens=0,
            cached_input_tokens=0,
            output_tokens=0,
        )
        for lang in LANGUAGES
    }
    variant_count = 0
    replayed = 0
    for row in rows:
        language = row["language"]
        outcomes = []
        for index, attempt in enumerate(row["attempts"]):
            generation = attempt["generation"]
            if generation["requested_model"] != MODEL or generation["effort"] != "low":
                raise ValueError("independent model/effort audit failed")
            command = generation["command"]
            if (
                command[command.index("--model") + 1] != MODEL
                or "--ignore-user-config" not in command
            ):
                raise ValueError("actual CLI arguments do not match the experiment")
            folder = args.directory / row["id"] / f"attempt-{index}"
            events = [
                json.loads(line)
                for line in (folder / "events.jsonl").read_text().splitlines()
                if line
            ]
            completions = [
                event for event in events if event.get("type") == "turn.completed"
            ]
            if len(completions) != 1 or completions[0]["usage"] != generation["usage"]:
                raise ValueError(
                    "raw provider usage differs from the aggregate receipt"
                )
            if any(
                event.get("item", {}).get("type")
                not in (None, "agent_message", "reasoning")
                for event in events
            ):
                raise ValueError("generation used an external capability")
            messages = [
                event["item"]["text"]
                for event in events
                if event.get("type") == "item.completed"
                and event.get("item", {}).get("type") == "agent_message"
            ]
            if not messages or json.loads(messages[-1])["code"] != generation["code"]:
                raise ValueError("stored code differs from the actual generated answer")
            local = []
            for variant in attempt["evaluation"]["variants"]:
                passed = independent_pass(
                    variant["observed"], fixture(row["task"], variant["variant"])
                )
                if passed != variant["passed"]:
                    raise ValueError("independent oracle disagrees")
                local.append(passed)
                variant_count += 1
            outcomes.append(all(local))
            totals[language]["attempts"] += 1
            for key in ("input_tokens", "cached_input_tokens", "output_tokens"):
                totals[language][key] += generation["usage"][key]
            if args.replay:
                fresh = evaluate(language, row["task"], generation["code"])
                if [v["passed"] for v in fresh["variants"]] != local:
                    raise ValueError(
                        f"frozen program replay disagrees: {row['id']} attempt {index}"
                    )
                replayed += len(fresh["variants"])
        if row["first_pass"] != outcomes[0] or row["passed"] != outcomes[-1]:
            raise ValueError("independent sample-level success audit failed")
        totals[language]["samples"] += 1
        totals[language]["first_pass"] += outcomes[0]
        totals[language]["eventual_pass"] += outcomes[-1]
    report = {
        "samples": len(rows),
        "independently_checked_variant_outcomes": variant_count,
        "frozen_program_replays": replayed,
        "raw_usage_and_generated_code_verified": True,
        "totals": totals,
        "limits": "Independent effect canonicalization and raw-event reconciliation, not an independent fixture author or model identity attestation.",
    }
    atomic_json(BUILD / "audit.json", report)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
