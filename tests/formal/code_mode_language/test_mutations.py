"""Both source mutations must be rejected by the actual Lean proof, not a hash."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
source = (ROOT / "tests/bench/code_mode/policy.py").read_text()
mutants = {
    "ignore-output": source.replace("execution_ok and output_ok and trace_ok", "execution_ok and trace_ok"),
    "reverse-first-pass": source.replace("first_candidate >= first_baseline", "first_baseline >= first_candidate"),
    "ignore-completeness": source.replace("complete and confirmed_gain", "confirmed_gain"),
    "ignore-parity": source.replace("confirmed_gain and parity", "confirmed_gain"),
}
with tempfile.TemporaryDirectory(prefix="tny-code-mode-mutations-") as temporary:
    for name, content in mutants.items():
        if content == source:
            raise SystemExit(f"mutation did not apply: {name}")
        path = Path(temporary) / f"{name}.py"
        path.write_text(content)
        result = subprocess.run([sys.executable, str(ROOT / "tests/formal/check_code_mode_language.py"),
                                 "--source", str(path)], env=os.environ, capture_output=True, text=True, timeout=65)
        if result.returncode == 0 or "error:" not in result.stdout:
            print(result.stdout, result.stderr)
            raise SystemExit(f"mutation not rejected by Lean: {name}")
        print(f"Lean rejected {name}")
