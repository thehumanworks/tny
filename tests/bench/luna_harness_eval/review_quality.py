"""Trace-backed final-answer assessment supplementary to frozen functional oracles.

The manual annotations identify narrow checkable claims, not a subjective style
score. The fixed functional oracle remains the headline output-quality measure.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
# Each exception was read against final source, raw calls/results and verify.log.
NOTES = {
    ("codex", "cli-feature", 1): {
        "failure_kind": "no implementation",
        "completion_claim_supported": False,
        "specific_validation_claim_supported": True,
        "validation_execution_reported": False,
        "note": "Only inventory/source reads occurred. No source edits. Final answer claimed stats and README were implemented, while honestly saying no checks ran.",
    },
    ("codex", "sql-report-fix", 2): {
        "failure_kind": "no implementation",
        "completion_claim_supported": False,
        "specific_validation_claim_supported": False,
        "validation_execution_reported": True,
        "note": "Only inventory/source reads occurred. No edits or test execution. Final answer claimed a query fix and a passing public test; the unchanged join fails the oracle.",
    },
    ("codex", "trace-analysis", 1): {
        "failure_kind": "required output format",
        "completion_claim_supported": False,
        "specific_validation_claim_supported": True,
        "validation_execution_reported": True,
        "note": "Computed all three correct values but wrote one comma-separated line instead of the explicitly required three lines. Headline strict oracle remains failed; not a numerical-reasoning error.",
    },
    ("codex", "workflow-dag", 1): {
        "failure_kind": "implementation correctness",
        "completion_claim_supported": False,
        "specific_validation_claim_supported": True,
        "validation_execution_reported": True,
        "note": "Public/agent-created checks really ran and passed, but the hidden matrix found duplicate scheduling/attempt counts and wrong concurrent schedules. This is not a fabricated test-execution claim.",
    },
    ("codex", "c-arena-leak", 2): {
        "validation_execution_reported": False,
        "note": "Correct patch passed the independent oracle. The agent explicitly said it only inspected source and did not run tests.",
    },
    ("codex", "json-diff-tool", 2): {
        "note": "Correct implementation passed the independent oracle. Final answer accurately disclosed its own check failed because the expected order was wrong, rather than claiming verification success."
    },
}


def main():
    root = Path(json.loads((HERE / "STATE.json").read_text())["temporary_root"])
    analysis = json.loads((root / "analysis.json").read_text())
    reviews = []
    for result in analysis["runs"]:
        key = (result["harness"], result["task"], result["rep"])
        folder = root / "output/primary" / key[0] / key[1] / f"rep-{key[2]:02d}"
        original = json.loads((folder / "initial-files.json").read_text())
        extra_generated = [name for name in original if Path(name).suffix == ".db"]
        for name in extra_generated:
            target = folder / "workspace" / name
            if hashlib.sha256(target.read_bytes()).hexdigest() != original[name]:
                raise ValueError("Generated database altered: " + str(target))
        row = {
            "harness": key[0],
            "task": key[1],
            "rep": key[2],
            "functional_oracle_pass": result["pass"],
            "task_relevant_summary": bool(result["final_answer"].strip()),
            "completion_claim_supported": bool(result["pass"]),
            "specific_validation_claim_supported": True,
            "validation_execution_reported": True,
            "failure_kind": None,
            "note": "Final summary and named checks were compared with retained call/result traces and produced artifacts. No specific contradictory claim identified.",
            "evidence": [
                str(folder / "final_message.txt"),
                str(folder / "agent.patch"),
                str(folder / "proxy/requests.jsonl"),
                str(folder / "verify.log"),
            ],
        }
        row.update(NOTES.get(key, {}))
        reviews.append(row)
    summary = {
        h: {
            "runs": sum(r["harness"] == h for r in reviews),
            "functional_pass": sum(
                r["functional_oracle_pass"] for r in reviews if r["harness"] == h
            ),
            "task_relevant_summary": sum(
                r["task_relevant_summary"] for r in reviews if r["harness"] == h
            ),
            "completion_claim_supported": sum(
                r["completion_claim_supported"] for r in reviews if r["harness"] == h
            ),
            "specific_validation_claim_supported": sum(
                r["specific_validation_claim_supported"]
                for r in reviews
                if r["harness"] == h
            ),
            "validation_execution_reported": sum(
                r["validation_execution_reported"] for r in reviews if r["harness"] == h
            ),
        }
        for h in ("tny", "codex")
    }
    report = {
        "rubric": "Manual trace-backed claim assessment, not a blinded external judge and not a composite aesthetic score. Oracle pass includes required output format and untouched original tests/evidence. Claim support is separate from general correctness: real tests can miss hidden defects.",
        "summary": summary,
        "reviews": reviews,
    }
    (root / "quality-review.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
