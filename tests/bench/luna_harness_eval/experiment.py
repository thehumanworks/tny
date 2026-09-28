"""Bounded, paired evaluation driver. No production code modifications."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from types import SimpleNamespace

from adapters import _binary
from run import run_one

HERE = Path(__file__).resolve().parent


def atomic(path, value):
    p = Path(path)
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(value, indent=2) + "\n")
    tmp.replace(p)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live", action="store_true")
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    if not args.live:
        raise SystemExit("Pass --live to authorize account inference")
    state = json.loads((HERE / "STATE.json").read_text())
    temp = Path(state["temporary_root"])
    os.environ["TMPDIR"] = str(temp / "tmp")
    os.environ["TIKTOKEN_CACHE_DIR"] = str(temp / "tokenizer-cache")
    os.environ["PATH"] = (
        str(Path(sys.executable).parent)
        + os.pathsep
        + str(Path.home() / ".local/share/mise/installs/python/3.14.7/bin")
        + os.pathsep
        + os.environ["PATH"]
    )
    settings = SimpleNamespace(
        out=temp / "output",
        label="smoke" if args.smoke else "primary",
        auth_file=Path.home() / ".codex/auth.json",
        model="gpt-6-luna",
        effort="low",
        tny_bin=str(temp / "bin/tny"),
    )
    if args.smoke:
        tasks = [
            Path(state["source_root"])
            / "tests/bench/harness_bench/tasks-smoke/smoke-hello"
        ]
        repetitions = 1
    else:
        tasks = sorted(
            (HERE / "tasks").iterdir(),
            key=lambda p: (
                json.loads((p / "task.json").read_text())["eval_tier"],
                p.name,
            ),
        )
        repetitions = 2
    meta = {
        "model": settings.model,
        "effort": settings.effort,
        "primary": not args.smoke,
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "tny_revision": state["baseline_revision"],
        "driver_revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=HERE, text=True
        ).strip(),
        "binaries": {},
        "driver_sources": {
            str(p.relative_to(HERE)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in HERE.glob("*.py")
        },
    }
    for name, path in [("tny", settings.tny_bin), ("codex", _binary("codex"))]:
        meta["binaries"][name] = {
            "path": path,
            "sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest(),
            "version": subprocess.check_output([path, "--version"], text=True).strip(),
        }
    atomic(
        temp / ("smoke-manifest.json" if args.smoke else "primary-manifest.json"), meta
    )
    rows = []
    with ThreadPoolExecutor(max_workers=2) as pool:
        for index, task in enumerate(tasks):
            for repetition in range(1, repetitions + 1):
                order = (
                    ["tny", "codex"] if (index + repetition) % 2 else ["codex", "tny"]
                )
                futures = [
                    pool.submit(run_one, settings, task, h, repetition) for h in order
                ]
                for future in as_completed(futures):
                    r = future.result()
                    rows.append(r)
                    print(
                        json.dumps(
                            {
                                "completed": len(rows),
                                "task": r["task"],
                                "tier": r.get("tier"),
                                "harness": r["harness"],
                                "rep": r["rep"],
                                "pass": r["pass"],
                                "status": r["status"],
                                "requests": r["requests"],
                                "input": r["input_tokens"],
                                "cached": r["cached_input_tokens"],
                                "output": r["output_tokens"],
                                "calls": r["tool_calls"],
                                "seconds": r["wall_s"],
                                "reason": r["reason"],
                            }
                        ),
                        flush=True,
                    )
                if args.smoke and any(
                    not r["pass"] or not r["measurement_valid"] for r in rows
                ):
                    raise SystemExit(
                        "Wiring smoke did not pass; no scored trials allowed"
                    )
    atomic(
        temp / ("smoke-results.json" if args.smoke else "primary-results.json"), rows
    )
    if len(rows) != len(tasks) * repetitions * 2:
        raise SystemExit("Incomplete cohort")
    print("COMPLETE", len(rows), flush=True)


if __name__ == "__main__":
    main()
