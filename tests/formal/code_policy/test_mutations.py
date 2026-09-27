"""Weakened copies of src/core/code_policy.c must fail the actual Lean proofs.

Each mutation removes or relaxes one production gate. A mutation that still
proves means the proofs do not constrain that gate, which fails this test.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "src/core/code_policy.c"
CHECK = ROOT / "tests/formal/check_code_policy.py"
MUTATIONS = {
    "call budget off by one": (
        "calls_done < TNY_CODE_TOOL_CALLS && name_bytes",
        "calls_done <= TNY_CODE_TOOL_CALLS && name_bytes",
    ),
    "recursion admitted": (
        "name_bytes <= TNY_CODE_NAME_BYTES && !recursive &&",
        "name_bytes <= TNY_CODE_NAME_BYTES &&",
    ),
    "output wrap-around": (
        "return used <= TNY_CODE_OUTPUT_BYTES && add <= TNY_CODE_OUTPUT_BYTES - used;",
        "return add <= TNY_CODE_OUTPUT_BYTES - used;",
    ),
    "memory wrap-around": (
        "return used <= limit && header <= limit - used &&",
        "return header <= limit - used &&",
    ),
    "frame phase ignored": (
        "return phase == TNY_CODE_PHASE_RUNNING && payload_bytes >= 1 &&",
        "return payload_bytes >= 1 &&",
    ),
    "int before bool": (
        ": is_bool          ? TNY_CODE_JSON_BOOL\n           : is_int           ? TNY_CODE_JSON_INT",
        ": is_int           ? TNY_CODE_JSON_INT\n           : is_bool          ? TNY_CODE_JSON_BOOL",
    ),
    "zero timeout": (
        "return timeout_ms >= 1 && timeout_ms",
        "return timeout_ms >= 0 && timeout_ms",
    ),
}


def main() -> int:
    lean = os.environ.get("LEAN", "lean")
    original = SOURCE.read_text()
    failures = []
    for name, (old, new) in MUTATIONS.items():
        if original.count(old) != 1:
            failures.append(f"{name}: mutation anchor not found exactly once")
            continue
        with tempfile.TemporaryDirectory(prefix="tny-code-policy-mutant-") as tmp:
            mutant = Path(tmp) / "code_policy.c"
            mutant.write_text(original.replace(old, new))
            done = subprocess.run(
                [sys.executable, str(CHECK), "--lean", lean, "--source", str(mutant)],
                capture_output=True,
                text=True,
                timeout=900,
            )
        status = "rejected" if done.returncode else "STILL PROVES"
        print(f"{name}: {status}")
        if not done.returncode:
            failures.append(name)
    if failures:
        print("mutations not detected:", ", ".join(failures), file=sys.stderr)
        return 1
    print(f"all {len(MUTATIONS)} weakened gates rejected by Lean")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
