#!/usr/bin/env python3
"""Production ACP run_code permission timing, denial and unsupported async boundary."""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    configured = os.environ.get("TNY_ACP_DEADLINE_BIN")
    if configured:
        binary = Path(configured).resolve()
    else:
        build_env = dict(os.environ)
        for key in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL"):
            build_env.pop(key, None)
        subprocess.run(
            ["make", "acp-bridge-deadline-fixture"],
            cwd=ROOT,
            env=build_env,
            timeout=300,
            check=True,
        )
        binary = (
            ROOT
            / "build"
            / ("acp-bridge-deadline.exe" if os.name == "nt" else "acp-bridge-deadline")
        )
    with tempfile.TemporaryDirectory(prefix="tny-acp-deadline-") as directory:
        subprocess.run(
            [str(binary), directory],
            env=dict(os.environ, TNY_TOOLS="all", TNY_SELF_IMPROVE="0"),
            timeout=15,
            check=True,
        )


if __name__ == "__main__":
    main()
