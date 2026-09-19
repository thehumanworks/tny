#!/usr/bin/env python3
"""Offline bounded instruction evolution using a deterministic replay interpreter.

Run with --out report.json [--controller /path/to/python/tny_improve.py].
This is NOT a language model or a general coding-improvement benchmark. A fixed
scripted proposer uses measured feedback to select understood instruction markers.
No candidate code is executed. The workload finds an exact key/value record among
local files. Full scans, suffix filtering, and header reads give two useful
generations; a first-file shortcut is cheaper on train but fails validation.

Cost is bytes returned by logical file reads, NOT physical disk I/O, latency,
tokens, or money. All discovery/read operations are traced. Fixture creation,
oracle checks, hashing, controller/proposer work, and Python startup are excluded.
The fixed small corpus deliberately demonstrates validation rejection; it is not
statistical evidence of generalization. Holdout is evaluated only by the controller
on baseline/final after search. Temporary inputs are regenerated on every run.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT = Path(__file__).resolve()
ROOT = SCRIPT.parents[2]
DEFAULT_CONTROLLER = ROOT / "python" / "tny_improve.py"
STRATEGIES = ("scan-all", "record-files", "record-headers", "first-record")
INSTRUCTIONS = {
    strategy: f"replay-v1:{strategy}\nReturn the exact header for the requested key."
    for strategy in STRATEGIES
}
COST_UNIT = "bytes returned by workload file reads"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_json(path: Path, value) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def strategy_of(instructions: str) -> str:
    for strategy, text in INSTRUCTIONS.items():
        if instructions == text:
            return strategy
    raise ValueError(
        "unsupported instructions: only exact replay-v1 variants are allowed"
    )


def make_fixtures(root: Path) -> tuple[dict, dict]:
    """Separate roots and distinct content for each split, with independent oracles."""
    cases = {}
    hashes = {}
    for split, targets in (
        ("train", (0, 0, 0)),
        ("validation", (2, 0, 1)),
        ("test", (3, 1, 2)),
    ):
        cases[split] = []
        for index, target in enumerate(targets):
            case_id = f"{split}-{index}"
            directory = root / case_id / "files"
            directory.mkdir(parents=True)
            key = f"wanted-{case_id}"
            # Expected bytes come from the task definition, never from the runner's
            # claimed success or by copying its selected file after execution.
            expected = f"{key}\tanswer-{case_id}-\u03bb\n".encode()
            oracle = directory.parent / "expected.bin"
            oracle.write_bytes(expected)
            for number in range(4):
                header = (
                    expected
                    if number == target
                    else f"other-{case_id}-{number}\tdecoy-{number}\n".encode()
                )
                tail = f"padding-{case_id}-{number}\n".encode() * (
                    40 + 7 * index + number
                )
                (directory / f"{number:02d}.record").write_bytes(header + tail)
            for number in range(4):
                (directory / f"noise-{number}.txt").write_bytes(
                    f"not-a-record-{case_id}-{number}\n".encode() * (60 + index)
                )
            cases[split].append(
                {
                    "id": case_id,
                    "input": {
                        "root": str(directory),
                        "key": key,
                        "oracle": str(oracle),
                        "audit": str(root / "evaluations.jsonl"),
                    },
                }
            )
    for path in sorted(root.rglob("*")):
        if path.is_file():
            hashes[path.relative_to(root).as_posix()] = sha256(path.read_bytes())
    return cases, hashes


def interpret(instructions: str, directory: Path, key: str) -> tuple[bytes, list, list]:
    """An allowlisted policy interpreter, not an agent or arbitrary-code executor."""
    strategy = strategy_of(instructions)
    names = sorted(path.name for path in directory.iterdir())
    trace = [{"operation": "list_directory", "entries": names}]
    selected = (
        names if strategy == "scan-all" else [n for n in names if n.endswith(".record")]
    )
    if strategy == "first-record":
        selected = selected[:1]
    found = []
    hints = []
    for name in selected:
        header_only = strategy in ("record-headers", "first-record")
        with (directory / name).open("rb") as stream:
            data = stream.readline() if header_only else stream.read()
        trace.append(
            {
                "operation": "read_header" if header_only else "read_file",
                "path": name,
                "bytes": len(data),
                "sha256": sha256(data),
            }
        )
        header = data[: data.index(b"\n") + 1] if b"\n" in data else data
        if header.startswith(key.encode() + b"\t"):
            found.append(header)
            if name == selected[0]:
                hints.append("target-in-first-record")
        if not name.endswith(".record"):
            hints.append("non-record-reads")
        if len(data) > len(header):
            hints.append("unused-tail-bytes")
    # Duplicate matches deliberately fail the byte-exact oracle too.
    return b"".join(found), trace, sorted(set(hints))


def evaluate(request: dict) -> dict:
    case = request["case"]
    inputs = case["input"]
    instructions = request["instructions"]
    actual, operations, hints = interpret(
        instructions, Path(inputs["root"]), inputs["key"]
    )
    expected = Path(inputs["oracle"]).read_bytes()
    reads = [op for op in operations if op["operation"].startswith("read_")]
    record = {
        "case_id": case["id"],
        "strategy": strategy_of(instructions),
        "passed": actual == expected,
        "cost": sum(op["bytes"] for op in reads),
        "file_reads": len(reads),
        "directory_lists": 1,
        "operations": operations,
        "actual_hex": actual.hex(),
        "expected_hex": expected.hex(),
        "oracle_read_bytes_excluded": len(expected),
        "hints": hints,
    }
    if inputs.get("audit"):
        with Path(inputs["audit"]).open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(record, sort_keys=True) + "\n")
    return {
        "passed": record["passed"],
        "cost": record["cost"],
        "feedback": json.dumps(record, sort_keys=True),
    }


def feedback_records(value):
    """Accept nested controller reports and JSON-encoded evaluator feedback."""
    if isinstance(value, str):
        try:
            yield from feedback_records(json.loads(value))
        except (ValueError, RecursionError):
            return
    elif isinstance(value, dict):
        if "hints" in value and "case_id" in value:
            yield value
        else:
            for child in value.values():
                yield from feedback_records(child)
    elif isinstance(value, list):
        for child in value:
            yield from feedback_records(child)


def propose(request: dict) -> dict:
    # Canonical controller keys: parent_instructions, round, training_feedback.
    # Accept the original draft aliases without executing any candidate text.
    parent = request.get(
        "parent_instructions", request.get("instructions", request.get("parent"))
    )
    if isinstance(parent, dict):
        parent = parent["instructions"]
    strategy = strategy_of(parent)
    records = [
        item
        for item in feedback_records(
            request.get("training_feedback", request.get("feedback"))
        )
        if item["strategy"] == strategy and item["case_id"].startswith("train-")
    ]
    hints = {hint for item in records for hint in item["hints"]}
    transitions = {
        "scan-all": ("non-record-reads", "record-files"),
        "record-files": ("unused-tail-bytes", "record-headers"),
        "record-headers": ("target-in-first-record", "first-record"),
    }
    hint, candidate = transitions.get(strategy, (None, strategy))
    if hint not in hints:
        return {"instructions": parent, "rationale": "No measured hint; retain parent."}
    return {
        "instructions": INSTRUCTIONS[candidate],
        "rationale": f"Deterministic replay, round {request['round']}: train observed {hint}.",
    }


def summarize(records: list[dict], final_strategy: str) -> dict:
    summary = {}
    for split in ("train", "validation", "test"):
        arms = {}
        for arm, strategy in (("baseline", "scan-all"), ("final", final_strategy)):
            # Repeated incumbent measurements must agree, not inflate fixed-case sums.
            by_case = {}
            for record in records:
                if record["strategy"] != strategy or not record["case_id"].startswith(
                    split + "-"
                ):
                    continue
                old = by_case.setdefault(record["case_id"], record)
                if old != record:
                    raise ValueError("nondeterministic repeated measurement")
            if len(by_case) != 3:
                raise ValueError(f"missing {split}/{arm} case measurements")
            arms[arm] = {
                "cases": len(by_case),
                "passed": sum(item["passed"] for item in by_case.values()),
                "cost": sum(item["cost"] for item in by_case.values()),
                "file_reads": sum(item["file_reads"] for item in by_case.values()),
            }
        before, after = arms["baseline"]["cost"], arms["final"]["cost"]
        arms["reduction_bytes"] = before - after
        arms["reduction_fraction"] = (before - after) / before
        summary[split] = arms
    return summary


def run(controller: Path) -> dict:
    controller = controller.resolve()
    if not controller.is_file():
        raise FileNotFoundError(
            f"controller unavailable: {controller}; use --controller PATH"
        )
    sources = {"benchmark": SCRIPT, "controller": controller}
    source_hashes = {name: sha256(path.read_bytes()) for name, path in sources.items()}
    with tempfile.TemporaryDirectory(prefix="tny-instruction-bench-") as temp:
        root = Path(temp)
        cases, fixture_hashes = make_fixtures(root)
        baseline = root / "baseline.txt"
        baseline.write_text(INSTRUCTIONS["scan-all"], encoding="utf-8")
        spec = {
            "version": 1,
            "baseline": str(baseline),
            "proposer": [sys.executable, str(SCRIPT), "--propose"],
            "evaluator": [sys.executable, str(SCRIPT), "--evaluate"],
            "cases": cases,
            "rounds": 3,
            "timeout_s": 30,
            "cost_unit": COST_UNIT,
        }
        write_json(root / "spec.json", spec)
        completed = subprocess.run(
            [
                sys.executable,
                str(controller),
                "run",
                "--spec",
                str(root / "spec.json"),
                "--out",
                str(root / "controller"),
            ],
            capture_output=True,
            text=True,
            timeout=180,
            check=True,
        )
        records = [
            json.loads(line)
            for line in (root / "evaluations.jsonl").read_text().splitlines()
        ]
        # Read final from actual holdout evaluations, not a duplicated selection rule.
        controller_report = json.loads(
            (root / "controller" / "report.json").read_text()
        )
        holdout_strategies = {
            r["strategy"] for r in records if r["case_id"].startswith("test-")
        }
        finals = holdout_strategies - {"scan-all"}
        if len(finals) > 1 or not holdout_strategies:
            raise ValueError("expected baseline/final-only holdout evaluations")
        final = next(iter(finals), "scan-all")
        archived_final = (root / "controller" / "final.md").read_text(encoding="utf-8")
        if strategy_of(archived_final) != final:
            raise ValueError(
                "holdout strategy differs from archived final instructions"
            )
        artifacts = {
            p.relative_to(root / "controller").as_posix(): p.read_text(encoding="utf-8")
            for p in sorted((root / "controller").rglob("*"))
            if p.is_file()
        }
        # Keep original artifact bytes (UTF-8), including temporary paths, so the
        # controller manifest remains checkable. Inputs are regenerated, not kept.
        if source_hashes != {
            name: sha256(path.read_bytes()) for name, path in sources.items()
        }:
            raise ValueError(
                "benchmark/controller source changed during measurement; rerun"
            )
        return {
            "version": 1,
            "benchmark": "bounded instruction evolution: deterministic scripted replay",
            "limitations": [
                "NOT language-model or general coding improvements; no live inference.",
                "Fixed policies and small purpose-built corpus; not statistical generalization.",
                "No latency, token, monetary, physical-I/O, or model performance claims.",
                "Cost excludes oracle reads, fixture setup, hashing, and controller/proposer overhead.",
                "Logical reads count returned bytes, not buffering or operating-system read calls.",
                "Validation intentionally defeats the tempting first-record shortcut.",
                "Archive text retains temporary paths; fixture files are regenerated, not retained.",
            ],
            "cost_unit": COST_UNIT,
            "environment": {
                "python": sys.version,
                "executable": sys.executable,
                "platform": platform.platform(),
            },
            "sources_sha256": source_hashes,
            "fixture_sha256": fixture_hashes,
            "instructions": INSTRUCTIONS,
            "final_strategy": final,
            "summary": summarize(records, final),
            "evaluations": records,
            "controller_report": controller_report,
            "controller_artifacts": artifacts,
            "controller_stdout": completed.stdout.replace(str(root), "<scratch>"),
            "controller_stderr": completed.stderr.replace(str(root), "<scratch>"),
        }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, help="new JSON report (refuse overwrite)")
    parser.add_argument("--controller", type=Path, default=DEFAULT_CONTROLLER)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--propose", action="store_true", help=argparse.SUPPRESS)
    modes.add_argument("--evaluate", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.propose or args.evaluate:
        result = (propose if args.propose else evaluate)(json.load(sys.stdin))
        print(json.dumps(result, sort_keys=True))
        return
    if args.out is None:
        parser.error("--out is required")
    if args.out.exists():
        parser.error("--out must be a new file")
    try:
        report = run(args.controller)
    except subprocess.CalledProcessError as error:
        parser.exit(1, f"controller failed ({error.returncode}): {error.stderr}\n")
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"benchmark failed: {error}\n")
    with args.out.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(json.dumps(report["summary"], sort_keys=True))


if __name__ == "__main__":
    main()
