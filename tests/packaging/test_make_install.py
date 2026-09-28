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
            self.assertTrue((prefix / "lib/tny/tny_ext/py.typed").is_file())
            helper = prefix / "share/tny/tny-workflows.sh"
            self.assertTrue(helper.is_file())
            self.assertTrue(helper.stat().st_mode & 0o111)
            self.assertEqual(
                (prefix / "share/tny/tny.zsh").read_bytes(),
                (ROOT / "shell/tny.zsh").read_bytes(),
            )
            # The executable embeds CPython (ADR 0179) and the pinned
            # libraries behind its stdlib modules (ADR 0180); their licenses ship.
            docs = prefix / "share/doc/tny"
            self.assertEqual(
                (docs / "CPython-LICENSE").read_bytes(),
                (ROOT / "third_party/cpython/LICENSE").read_bytes(),
            )
            self.assertTrue((docs / "CPython-incorporated-software.rst").is_file())
            self.assertTrue((docs / "HACL-LICENSE").is_file())
            self.assertTrue((docs / "THIRD_PARTY_NOTICES.md").is_file())
            pinned = sorted(p.parent for p in (ROOT / "third_party").glob("*/URL"))
            self.assertGreaterEqual(len(pinned), 7)
            for library in pinned:
                texts = [*library.glob("LICENSE*"), *library.glob("COPYING*")]
                self.assertTrue(texts, library)
                for text in texts:
                    self.assertEqual(
                        (docs / f"{library.name}-{text.name}").read_bytes(),
                        text.read_bytes(),
                    )


if __name__ == "__main__":
    unittest.main()
