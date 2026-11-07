#!/usr/bin/env python3
"""Opt-in paired native tny SIWC timing; retain event timings, never content."""

import argparse
import json
import os
import subprocess
import tempfile
import threading
import time
from pathlib import Path


def run(binary):
    with tempfile.TemporaryDirectory(prefix="tny-live-bench-") as workspace:
        env = dict(os.environ)
        for key in list(env):
            if key.endswith("_API_KEY") or key.endswith("_BASE_URL"):
                env.pop(key)
        start = time.perf_counter()
        process = subprocess.Popen(
            [
                binary,
                "--cwd",
                workspace,
                "--provider",
                "openai",
                "--model",
                "gpt-6.1-sol",
                "--effort",
                "low",
                "--no-self-improve",
                "ask",
                "--ephemeral",
                "--events=jsonl",
                "Call run_code exactly once with print(42). Then reply only OK.",
            ],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        timer = threading.Timer(90, process.kill)
        timer.start()
        rows = []
        try:
            for line in process.stdout:
                event = json.loads(line)
                rows.append(
                    {
                        "type": event.get("type"),
                        "ms": (time.perf_counter() - start) * 1000,
                    }
                )
            rc = process.wait(timeout=5)
            types = [r["type"] for r in rows]
            return {
                "exit": rc,
                "events": rows,
                "valid": rc == 0
                and "tool_start" in types
                and "tool_end" in types
                and "error" not in types,
            }
        finally:
            timer.cancel()
            if process.poll() is None:
                process.kill()
                process.wait()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--live", action="store_true", required=True)
    p.add_argument("--baseline", type=Path, required=True)
    p.add_argument("--candidate", type=Path, required=True)
    p.add_argument("--pairs", type=int, default=3)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    results = {"model": "gpt-6.1-sol", "effort": "low", "pairs": []}
    for i in range(args.pairs):
        rows = {}
        for name in (
            ("baseline", "candidate") if i % 2 == 0 else ("candidate", "baseline")
        ):
            rows[name] = run(str(getattr(args, name).resolve()))
            print(
                name,
                "valid",
                rows[name]["valid"],
                "total_ms",
                rows[name]["events"][-1]["ms"] if rows[name]["events"] else None,
                flush=True,
            )
        results["pairs"].append(rows)
        args.output.write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
