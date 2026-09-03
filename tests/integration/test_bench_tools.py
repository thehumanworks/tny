#!/usr/bin/env python3
"""Smoke the tool-profile A/B harness (tests/bench/bench_tools.py, issue #103).

No provider key: `--mock` scripts tests/integration/mock_openai.py to issue one
`terminal` call running the fixture's own reference solution, so the whole
pipeline — scratch copy, `ask -B --json --stdin`, `session --wait --json`,
transcript metrics, `check.sh` scoring, markdown + JSON report — runs for one
task in each of the three arms. `--verify-fixtures` additionally proves every
frozen task is red before any work and green after its reference solution, so
a broken fixture cannot hand an arm a free pass.
"""

import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TNY = os.environ.get("TNY", os.path.join(ROOT, "build", "tny"))
BENCH = os.path.join(ROOT, "tests", "bench", "bench_tools.py")
ARMS = ("all", "terminal+edit", "terminal")
TASK = "fix-py-sum-range"


def run(args, timeout=900):
    p = subprocess.run(
        [sys.executable, BENCH, "--tny", TNY] + args,
        cwd=ROOT,
        capture_output=True,
        timeout=timeout,
    )
    return (
        p.returncode,
        p.stdout.decode("utf-8", "replace"),
        p.stderr.decode("utf-8", "replace"),
    )


def check_dry_run():
    rc, out, err = run(["--dry-run"], timeout=120)
    assert rc == 0, err
    assert TASK in out, out
    families = {ln.split()[0] for ln in out.splitlines() if ln and not ln[0].isdigit()}
    assert {"fix-test", "refactor", "question"} <= families, families
    n = int(out.strip().splitlines()[-1].split()[0])
    assert n >= 20, f"the frozen task set shrank to {n} tasks"


def check_fixtures():
    rc, out, err = run(["--verify-fixtures"])
    assert rc == 0, out + err
    assert "fixtures verified" in out, out


def check_mock():
    with tempfile.TemporaryDirectory(prefix="tnybench.smoke.") as out_dir:
        rc, out, err = run(
            ["--mock", "--tasks", TASK, "--effort", "high", "--out", out_dir]
        )
        assert rc == 0, out + err
        for arm in ARMS:
            assert "`%s` | 1/1 (100%%)" % arm in out, out
        docs = [f for f in os.listdir(out_dir) if f.endswith(".json")]
        assert len(docs) == 1, docs
        doc = json.load(open(os.path.join(out_dir, docs[0])))
        assert doc["meta"]["provider"] == "mock", doc["meta"]
        assert len(doc["runs"]) == len(ARMS), doc["runs"]
        for rec in doc["runs"]:
            assert rec["pass"], rec
            assert rec["error"] is None, rec
            # the scripted trajectory is exactly one terminal call
            assert rec["tool_calls"] == 1, rec
            assert rec["terminal_calls"] == 1, rec
            assert rec["tokens_in"] > 0 and rec["tokens_out"] > 0, rec
            assert rec["repair_loops"] == 0, rec
        assert {r["arm"] for r in doc["runs"]} == set(ARMS)


def check_unknown_task():
    rc, out, err = run(["--tasks", "no-such-task", "--dry-run"], timeout=120)
    assert rc != 0, out
    assert "unknown task" in err, err


def main():
    check_dry_run()
    check_unknown_task()
    check_fixtures()
    check_mock()
    print("bench_tools: ok")


if __name__ == "__main__":
    main()
