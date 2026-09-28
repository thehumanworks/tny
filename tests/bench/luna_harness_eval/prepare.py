"""Create a marked disposable evaluation root and freeze an explicit Tny binary."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tny", type=Path, required=True)
    parser.add_argument("--temporary-parent", type=Path, default=Path("/var/tmp"))
    args = parser.parse_args()
    binary = args.tny.resolve()
    if not binary.is_file():
        raise SystemExit("Explicit Tny binary does not exist")
    if (HERE / "STATE.json").exists():
        raise SystemExit(
            "STATE.json exists; finish/export/clean the previous owned evaluation first"
        )
    work = Path(tempfile.mkdtemp(prefix="tny-codex-eval-", dir=args.temporary_parent))
    state = {
        "temporary_root": str(work),
        "source_root": str(ROOT),
        "worktree": str(ROOT),
        "model": "gpt-6-luna",
        "effort": "low",
        "baseline_revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
    }
    (work / "OWNER.json").write_text(
        json.dumps(
            {
                "task": "tny versus Codex gpt-6-luna evaluation",
                "root": str(work),
                "worktree": str(ROOT),
            },
            indent=2,
        )
        + "\n"
    )
    for name in ("bin", "tmp", "output"):
        (work / name).mkdir()
    shutil.copy2(binary, work / "bin/tny")
    (work / "bin/tny").chmod(0o755)
    (HERE / "STATE.json").write_text(json.dumps(state, indent=2) + "\n")
    print(
        json.dumps(
            {
                **state,
                "tny_sha256": hashlib.sha256(
                    (work / "bin/tny").read_bytes()
                ).hexdigest(),
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
