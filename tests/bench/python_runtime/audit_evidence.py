"""Independent typed-effect, cohort and raw-event audit; never performs inference."""

from __future__ import annotations

import argparse
import hashlib
import json
import tarfile
from collections import Counter
from pathlib import Path
from typing import Any

import heldout
from execute import strict_json
from run import generation_valid

ROOT = Path(__file__).resolve().parents[3]
DATA = ROOT / "docs/verification/python-code-mode/data"
ARMS = ("cpython", "cpython_pr197", "monty")


def canonical(value: Any, ordered: bool = False) -> Any:
    if isinstance(value, dict):
        pairs = [(key, canonical(item, ordered)) for key, item in value.items()]
        return "object", tuple(pairs if ordered else sorted(pairs))
    if isinstance(value, list):
        return "array", tuple(canonical(item, ordered) for item in value)
    if type(value) in (int, float):
        return "number", value
    return type(value).__name__, value


def effect_pass(task: str, observed: dict[str, Any], case: dict[str, Any]) -> bool:
    if (
        observed.get("runtime_ok") is not True
        or observed.get("invalid_call") is not False
    ):
        return False
    calls, writes = observed.get("calls"), observed.get("writes")
    if (
        observed.get("stdout") != "done\n"
        or not isinstance(calls, list)
        or len(calls) > 64
    ):
        return False
    if not all(
        any(canonical(call) == canonical(needed) for call in calls)
        for needed in case["required_calls"]
    ):
        return False
    if not isinstance(writes, dict) or writes.keys() != case["expected"].keys():
        return False
    for path, expected in case["expected"].items():
        try:
            actual = (
                strict_json(writes[path])
                if path in case["json_paths"]
                else writes[path]
            )
        except (ValueError, TypeError):
            return False
        if canonical(actual, task in heldout.ORDERED) != canonical(
            expected, task in heldout.ORDERED
        ):
            return False
    return True


def verify_samples(samples: list[dict[str, Any]]) -> dict[str, Any]:
    expected = {
        (task, repetition, arm)
        for task, _ in heldout.TASKS
        for repetition in range(heldout.REPEATS)
        for arm in ARMS
    }
    identities = [(s["task"], s["repetition"], s["arm"]) for s in samples]
    if len(identities) != len(expected) or set(identities) != expected:
        raise ValueError("missing, duplicate or unexpected held-out cohort member")
    totals = {
        arm: Counter(
            samples=0,
            first_pass=0,
            final=0,
            generations=0,
            output_tokens=0,
            input_tokens=0,
            cached_input_tokens=0,
        )
        for arm in ARMS
    }
    variants_checked = 0
    for sample in samples:
        if sample["id"] != f"{sample['task']}-{sample['repetition']}-{sample['arm']}":
            raise ValueError("sample ID and fields disagree")
        if type(sample["first_pass"]) is not bool or type(sample["passed"]) is not bool:
            raise ValueError("sample success must be Boolean")
        attempts = sample["attempts"]
        if len(attempts) not in (1, 2) or (len(attempts) == 2 and sample["first_pass"]):
            raise ValueError("repair budget or no-rerun rule violated")
        outcomes = []
        for attempt in attempts:
            gen = attempt["generation"]
            if gen["requested_model"] != "gpt-6-luna" or gen["effort"] != "low":
                raise ValueError("wrong model or effort")
            if (
                gen["generation_ok"] is not True
                or gen["exit_code"] != 0
                or gen["timed_out"] is not False
            ):
                raise ValueError("incomplete generation")
            command = gen["command"]
            if (
                command[command.index("--model") + 1] != "gpt-6-luna"
                or "--ignore-user-config" not in command
            ):
                raise ValueError(
                    "actual CLI arguments do not identify the requested model"
                )
            if 'model_reasoning_effort="low"' not in command:
                raise ValueError("actual reasoning effort was not low")
            usage = gen["usage"]
            for key in (
                "input_tokens",
                "cached_input_tokens",
                "output_tokens",
                "reasoning_output_tokens",
            ):
                if type(usage.get(key)) is not int or usage[key] < 0:
                    raise ValueError("missing or invalid measured usage")
            if usage["cached_input_tokens"] > usage["input_tokens"]:
                raise ValueError("cached input exceeds total input")
            variants = attempt["evaluation"]["variants"]
            if [v["variant"] for v in variants] != list(range(heldout.VARIANTS)):
                raise ValueError("missing, reordered or duplicate variant")
            results = []
            for variant in variants:
                passed = effect_pass(
                    sample["task"],
                    variant["observed"],
                    heldout.fixture(sample["task"], variant["variant"]),
                )
                if type(variant["passed"]) is not bool or passed != variant["passed"]:
                    raise ValueError("independent typed-effect oracle disagrees")
                variants_checked += 1
                results.append(passed)
            if (
                type(attempt["evaluation"]["passed"]) is not bool
                or all(results) != attempt["evaluation"]["passed"]
            ):
                raise ValueError("attempt did not pass all required variants")
            outcomes.append(all(results))
            totals[sample["arm"]]["generations"] += 1
            for key in ("input_tokens", "cached_input_tokens", "output_tokens"):
                totals[sample["arm"]][key] += usage[key]
        if outcomes[0] != sample["first_pass"] or outcomes[-1] != sample["passed"]:
            raise ValueError("sample outcome differs from actual attempts")
        totals[sample["arm"]]["samples"] += 1
        totals[sample["arm"]]["first_pass"] += outcomes[0]
        totals[sample["arm"]]["final"] += outcomes[-1]
    return {
        "samples": len(samples),
        "variant_outcomes_checked": variants_checked,
        "totals": totals,
    }


def verify_raw(samples: list[dict[str, Any]], archive: Path) -> int:
    expected = {}
    for sample in samples:
        for index, attempt in enumerate(sample["attempts"]):
            base = f"live-heldout/{sample['id']}/attempt-{index}"
            expected[base] = attempt["generation"]
    payloads = {}
    with tarfile.open(archive) as tar:
        for member in tar:
            if not member.isfile() or member.name.rsplit("/", 1)[-1] not in (
                "events.jsonl",
                "prompt.txt",
            ):
                continue
            if member.name in payloads or member.size > 2 * 1024 * 1024:
                raise ValueError("duplicate or oversized raw receipt")
            with tar.extractfile(member) as stream:
                payloads[member.name] = stream.read()
    for base, generation in expected.items():
        prompt = payloads[base + "/prompt.txt"]
        if hashlib.sha256(prompt).hexdigest() != generation["prompt_sha256"]:
            raise ValueError("prompt hash mismatch")
        events = [
            strict_json(line)
            for line in payloads[base + "/events.jsonl"].decode().splitlines()
            if line
        ]
        if not generation_valid(events, 0, {"code": generation["code"]}):
            raise ValueError("invalid or tool-using raw generation")
        completed = next(
            event for event in events if event.get("type") == "turn.completed"
        )
        if completed["usage"] != generation["usage"]:
            raise ValueError("recorded usage differs from actual completion event")
        answers = [
            event["item"]["text"]
            for event in events
            if event.get("type") == "item.completed"
            and event.get("item", {}).get("type") == "agent_message"
        ]
        if not answers or strict_json(answers[-1]) != {"code": generation["code"]}:
            raise ValueError("saved source differs from the generated answer")
    return len(expected)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=Path, default=DATA / "heldout-samples.json")
    parser.add_argument("--cohort-only", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    samples = strict_json(args.samples.read_text())
    report = verify_samples(samples)
    if not args.cohort_only:
        report["raw_generations_checked"] = verify_raw(
            samples, DATA / "heldout-raw-generations.tar.gz"
        )
        published = strict_json((DATA / "heldout-analysis.json").read_text())
        for arm, totals in report["totals"].items():
            if any(
                published["arms"][arm][key] != value for key, value in totals.items()
            ):
                raise ValueError("published aggregate differs from independent totals")
        report["scope"] = (
            "Independent typed-effect and raw-event reconciliation; task fixtures shared, not an independently authored corpus or statistical coverage proof."
        )
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    print(text, end="")


if __name__ == "__main__":
    main()
