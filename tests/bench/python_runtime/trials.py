"""Paired held-out generation trials over Python runtime/prompt arms.

Generation reuses the preserved PR #197 `run.generate` unchanged: normal Codex
CLI ChatGPT login (credentials never read here), gpt-6-luna, low effort, code-
only output schema, no generation-time tools, one fresh-session repair with
observed failures but never expected answers. Each arm's programs execute on
that arm's runtime inside the mandatory bubblewrap envelope.

  trials.py --controls                      # references on every arm (required)
  trials.py --live --output DIR             # explicit, bounded account usage
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/code_mode"))
sys.path.insert(0, str(HERE))

import heldout  # noqa: E402
from heldout_references import reference  # noqa: E402
from replay import BUILD, run, sandbox  # noqa: E402
from run import MODEL, atomic_json, generate  # noqa: E402

ARMS = ("cpython", "cpython_pr197", "monty")


# Arm name -> executor built by build.py. The CPython arm uses stock CPython
# 3.14.7 with the proposed production builtins/print/error policy.
EXECUTORS = {"cpython": "cpython_prod", "cpython_pr197": "cpython", "monty": "monty"}


def arm_command(arm: str) -> list[str]:
    build = json.loads((BUILD / "build.json").read_text())
    extra = [build["python_home"]] if arm.startswith("cpython") else []
    return sandbox(BUILD / EXECUTORS[arm], extra)


def evaluate(arm: str, task: str, code: str) -> dict[str, Any]:
    command = arm_command(arm)
    variants = []
    for index in range(heldout.VARIANTS):
        case = heldout.fixture(task, index)
        if not isinstance(code, str) or "\0" in code or len(code.encode()) > 262144:
            observed = {"runtime_ok": False, "stdout": "invalid source", "calls": [], "writes": {}, "invalid_call": False}
        else:
            observed = run(command, code, case["runtime"])
        variants.append({"variant": index, **heldout.score(task, observed, case), "observed": observed})
    return {"passed": all(v["passed"] for v in variants), "variants": variants}


def controls() -> list[dict[str, Any]]:
    rows = []
    for arm in ARMS:
        for task, _ in heldout.TASKS:
            result = evaluate(arm, task, reference(task))
            rows.append({"arm": arm, "task": task, **result})
            print(arm, task, result["passed"], flush=True)
            if not result["passed"]:
                bad = [v for v in result["variants"] if not v["passed"]][0]
                print(json.dumps(bad, ensure_ascii=False)[:3000])
    return rows


def sample(arm: str, task: str, repetition: int, output: Path) -> dict[str, Any]:
    key = f"{task}-{repetition}-{arm}"
    destination = output / key
    if destination.exists():
        raise RuntimeError(f"Refusing to overwrite or selectively rerun an existing sample: {key}")
    original = heldout.prompt(arm, task)
    attempts = []
    request = original
    for attempt in range(2):
        result = generate(request, destination / f"attempt-{attempt}")
        evaluation = (
            evaluate(arm, task, result["code"])
            if result["generation_ok"]
            else {"passed": False, "variants": [], "generation_failed": True}
        )
        attempts.append({"generation": result, "evaluation": evaluation})
        if evaluation["passed"] or not result["generation_ok"]:
            break
        feedback = [
            {k: v[k] for k in ("variant", "execution_ok", "output_ok", "trace_ok", "observed")}
            for v in evaluation["variants"]
            if not v["passed"]
        ]
        request = (
            original
            + "\nRepair the following previous attempt. This is the only repair opportunity.\nPrevious code:\n"
            + result["code"]
            + "\nObserved failures (expected answers are not supplied):\n"
            + json.dumps(feedback, ensure_ascii=False)[:16000]
        )
    row = {
        "id": key,
        "arm": arm,
        "task": task,
        "repetition": repetition,
        "attempts": attempts,
        "first_pass": attempts[0]["evaluation"]["passed"],
        "passed": attempts[-1]["evaluation"]["passed"],
    }
    atomic_json(destination / "sample.json", row)
    return row


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--controls", action="store_true")
    parser.add_argument("--live", action="store_true", help="Explicitly authorize bounded Codex account usage")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--workers", type=int, choices=range(1, 4), default=3)
    args = parser.parse_args()
    if args.controls:
        rows = controls()
        (BUILD / "heldout-controls.json").write_text(json.dumps(rows, ensure_ascii=False, indent=1) + "\n")
        passed = sum(v["passed"] for r in rows for v in r["variants"])
        total = sum(len(r["variants"]) for r in rows)
        print(f"Held-out controls: {passed}/{total} variant executions passed")
        raise SystemExit(0 if passed == total else 1)
    if not args.live or not args.output:
        raise SystemExit("Live inference is opt-in: pass --live --output NEW_DIR")
    controls_file = BUILD / "heldout-controls.json"
    rows = json.loads(controls_file.read_text()) if controls_file.exists() else []
    if len(rows) != len(ARMS) * len(heldout.TASKS) or not all(r["passed"] for r in rows):
        raise SystemExit("All held-out reference controls must pass on every arm before live inference")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    order = [
        (ARMS[(index + repetition + offset) % len(ARMS)], task, repetition)
        for repetition in range(heldout.REPEATS)
        for index, (task, _) in enumerate(heldout.TASKS)
        for offset in range(len(ARMS))
    ]
    build = json.loads((BUILD / "build.json").read_text())
    atomic_json(
        output / "manifest.json",
        {
            "model": MODEL,
            "effort": "low",
            "arms": ARMS,
            "source_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
            "codex_version": subprocess.check_output(["codex", "--version"], text=True).strip(),
            "source_sha256": {
                str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(HERE.glob("*.py")) + sorted((ROOT / "tests/bench/code_mode").glob("*.py"))
            },
            "prompt_sha256": {
                f"{task}-{arm}": hashlib.sha256(heldout.prompt(arm, task).encode()).hexdigest()
                for arm in ARMS
                for task, _ in heldout.TASKS
            },
            "executors": {arm: build["binaries"][EXECUTORS[arm]] for arm in ARMS},
            "workers": args.workers,
            "order": order,
            "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "authentication": "normal Codex CLI ChatGPT login; credentials never read by benchmark",
        },
    )
    results = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(sample, *entry, output) for entry in order]
        for future in as_completed(futures):
            row = future.result()
            results.append(row)
            print(f"{len(results)}/{len(order)} {row['id']} first={row['first_pass']} final={row['passed']}", flush=True)
    atomic_json(output / "samples.json", sorted(results, key=lambda r: r["id"]))
    print("COMPLETE", len(results), flush=True)


if __name__ == "__main__":
    main()
