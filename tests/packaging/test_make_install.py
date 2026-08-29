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
            self.assertTrue((prefix / "lib/tny/tny_extension_host.py").is_file())
            self.assertTrue((prefix / "lib/tny/tny_ext/py.typed").is_file())
            helper = prefix / "share/tny/tny-workflows.sh"
            self.assertTrue(helper.is_file())
            self.assertTrue(helper.stat().st_mode & 0o111)


if __name__ == "__main__":
    unittest.main()
