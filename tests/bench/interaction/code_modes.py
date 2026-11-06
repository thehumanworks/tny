#!/usr/bin/env python3
"""Compare execution modes through the same production run_code boundary.

Also report warm Monty checkout separately: that pool isn't integrated in tny.
Monty dependency is optional and must be supplied explicitly in --monty-python.
"""

import argparse
import json
import statistics
import subprocess
from pathlib import Path

from bench import run


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--tny", required=True)
    p.add_argument("--monty-python", required=True)
    p.add_argument("--runs", type=int, default=10)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    expression = "sum(i*i for i in range(1,10001))"
    helper = str(Path(__file__).with_name("monty_cell.py").resolve())
    arms = {
        "embedded_cpython": f"print({expression})",
        "host_python": f'import subprocess\nsubprocess.run(["python3", "-c", {f"print({expression})"!r}], check=True)',
        "host_javascript": 'import subprocess\nsubprocess.run(["node", "-e", "let s=0; for(let i=1;i<=10000;i++) s+=i*i; console.log(s)"], check=True)',
        "monty_fresh_pool": f"import subprocess\nsubprocess.run([{a.monty_python!r}, {helper!r}], input={expression!r}, text=True, check=True)",
    }
    # Use sh without quoting artifacts from a second source-language layer.
    arms["host_shell"] = (
        "import subprocess\nsubprocess.run("
        + repr(
            [
                "/bin/sh",
                "-c",
                "awk 'BEGIN {s=0; for(i=1;i<=10000;i++) s+=i*i; printf \"%.0f\\n\", s}'",
            ]
        )
        + ", check=True)"
    )
    result = {"workload": "sum of squares 1..10000; every output checked", "arms": {}}
    for name, code in arms.items():
        rows = [
            run(
                str(Path(a.tny).resolve()), 0, False, code=code, expected="333383335000"
            )
            for _ in range(a.runs)
        ]
        result["arms"][name] = {
            "median_tool_continuation_ms": statistics.median(
                r["tool_continuation_ms"] for r in rows
            ),
            "runs": rows,
        }
        print(name, result["arms"][name]["median_tool_continuation_ms"], flush=True)
    # This intentionally excludes pool startup and the production protocol.
    warm = subprocess.run(
        [
            a.monty_python,
            "-c",
            """
import json, time
from pydantic_monty import Monty
rows=[]
start=time.perf_counter()
with Monty() as pool:
    startup=(time.perf_counter()-start)*1000
    for _ in range(30):
        start=time.perf_counter()
        with pool.checkout() as cell:
            assert cell.feed_run("sum(i*i for i in range(1,10001))") == 333383335000
        rows.append((time.perf_counter()-start)*1000)
print(json.dumps({'pool_start_ms':startup,'checkout_run_ms':rows}))
""",
        ],
        capture_output=True,
        text=True,
        timeout=30,
        check=True,
    )
    result["monty_warm_lower_bound"] = json.loads(warm.stdout)
    a.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
