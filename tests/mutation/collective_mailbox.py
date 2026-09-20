#!/usr/bin/env python3
"""Targeted mailbox mutants in a disposable source copy; never edits checkout."""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path("src/core/team_mailbox.c")
TEST = Path("tests/integration/test_team_mailbox.py")
MUTANTS = [
    (
        "publication-content-conflict",
        "memcmp(m->payload, payload, payload_len) != 0",
        "false",
        "test_collective_publication_is_atomic_and_recipient_replayable",
    ),
    (
        "publication-exact-quota-boundary",
        "pending >= TNY_MAILBOX_OUTSTANDING_MAX",
        "pending > TNY_MAILBOX_OUTSTANDING_MAX",
        "test_collective_full_recipient_aborts_every_receipt",
    ),
    (
        "wait-periodic-resnapshot",
        "if (event == 1) continue;",
        "if (event == 0) continue;",
        "test_event_wait_quiet_has_one_snapshot_and_no_periodic_rescans",
    ),
    (
        "capacity-hides-retained-history",
        "out->history_used = t.count;",
        "out->history_used = 0;",
        "test_capacity_snapshot_never_delivers_and_ack_only_frees_outstanding",
    ),
    (
        "capacity-drops-older-attempt-backlog",
        "t.messages[i].recipient.task == caller->task && outstanding(&t.messages[i])",
        "addressed(&t.messages[i], caller) && outstanding(&t.messages[i])",
        "test_capacity_includes_old_attempts_and_retained_tombstones",
    ),
    (
        "capacity-allows-overquota-underflow",
        "out->outstanding_used > TNY_MAILBOX_OUTSTANDING_MAX",
        "out->outstanding_used > TNY_MAILBOX_HISTORY_MAX",
        "test_capacity_full_history_and_backlog_and_corrupt_overquota",
    ),
]


def run(copy, name, test=None):
    command = [sys.executable, str(copy / TEST)]
    if test:
        command += ["MailboxTests." + test]
    result = subprocess.run(
        command,
        cwd=copy,
        env=dict(os.environ, PYTHONPATH=str(ROOT / "tests/integration")),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=120,
    )
    log = result.stdout
    print(f"{name}: exit {result.returncode}")
    if result.returncode and "FAIL:" not in log:
        print(log)
        raise RuntimeError("non-behavioral failure is not a mutation kill")
    return {"name": name, "exit": result.returncode, "test": test, "output": log}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "docs/verification/collective-swarm/artifacts/mutations.json",
    )
    args = parser.parse_args()
    original = (ROOT / SOURCE).read_text()
    outcomes = []
    # The only writes are under this worktree's ignored build directory.
    (ROOT / "build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="collective-mutations-", dir=ROOT / "build"
    ) as temp:
        copy = Path(temp)
        shutil.copytree(ROOT / "src", copy / "src")
        shutil.copytree(ROOT / "third_party", copy / "third_party")
        (copy / TEST).parent.mkdir(parents=True)
        shutil.copy2(ROOT / TEST, copy / TEST)
        (copy / "tests/fixtures").mkdir()
        shutil.copy2(
            ROOT / "tests/fixtures/team_mailbox_faults.c", copy / "tests/fixtures"
        )
        baseline = run(copy, "baseline")
        assert baseline["exit"] == 0, baseline["output"]
        outcomes.append(baseline)
        start = original.index("tny_mailbox_rc tny_team_mailbox_publish(")
        for name, before, after, test in MUTANTS:
            prefix, body = original[:start], original[start:]
            assert before in body, name
            (copy / SOURCE).write_text(prefix + body.replace(before, after, 1))
            result = run(copy, name, test)
            assert result["exit"] != 0, "surviving critical mutant: " + name
            assert "AssertionError" in result["output"], result["output"]
            outcomes.append(result)
        (copy / SOURCE).write_text(original)
        final = run(copy, "restored")
        assert final["exit"] == 0, final["output"]
        outcomes.append(final)
    artifact = args.output
    artifact.parent.mkdir(parents=True, exist_ok=True)
    artifact.write_text(
        json.dumps(
            {
                "source_sha256": hashlib.sha256(original.encode()).hexdigest(),
                "results": outcomes,
            },
            indent=2,
        )
        + "\n"
    )
    print(f"PASS: {len(MUTANTS)} behavioral kills; original source unchanged")


if __name__ == "__main__":
    main()
