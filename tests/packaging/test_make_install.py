#!/usr/bin/env python3
"""Regression coverage for Makefile install paths containing spaces."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class MakeInstallTests(unittest.TestCase):
    def test_install_accepts_prefix_with_spaces(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / "prefix with spaces"
            subprocess.run(
                ["make", "install", f"PREFIX={prefix}"],
                cwd=ROOT,
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            self.assertTrue((prefix / "bin/tny").is_file())
            for name in ("tny_improve.py", "tny_improve_propose.py"):
                installed = prefix / "lib/tny" / name
                self.assertEqual(
                    installed.read_bytes(), (ROOT / "python" / name).read_bytes()
                )
                subprocess.run(
                    ["python3", str(installed), "--help"],
                    check=True,
                    capture_output=True,
                    timeout=5,
                )
            helper = prefix / "share/tny/tny-workflows.sh"
            self.assertTrue(helper.is_file())
            self.assertTrue(helper.stat().st_mode & 0o111)
            self.assertEqual(
                (prefix / "share/tny/tny.zsh").read_bytes(),
                (ROOT / "shell/tny.zsh").read_bytes(),
            )
            # No language runtime is embedded (ADR 0222): tny's own license and
            # the vendored-source notices are the whole set.
            docs = prefix / "share/doc/tny"
            self.assertEqual(  # tny's own MIT license ships with the binary
                (docs / "LICENSE").read_bytes(), (ROOT / "LICENSE").read_bytes()
            )
            self.assertEqual(
                (docs / "THIRD_PARTY_NOTICES.md").read_bytes(),
                (ROOT / "THIRD_PARTY_NOTICES.md").read_bytes(),
            )
            self.assertEqual(
                sorted(p.name for p in docs.iterdir()),
                ["LICENSE", "THIRD_PARTY_NOTICES.md"],
            )


if __name__ == "__main__":
    unittest.main()
