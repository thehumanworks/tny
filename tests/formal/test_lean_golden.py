"""Negative controls for fixture drift; a local fake Lake needs no toolchain."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from check_lean_golden import files

CHECKER = Path(__file__).with_name("check_lean_golden.py")
FAKE_LAKE = """#!/usr/bin/env python3
import shutil
import sys
from pathlib import Path

project = Path.cwd()
with (project / "calls").open("a") as log:
    log.write(sys.argv[1] + "\\n")
if (project / ("fail-" + sys.argv[1])).exists():
    raise SystemExit(19)
if sys.argv[1] == "exe":
    shutil.copytree(project / "exported", sys.argv[-1])
"""


class GoldenTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="tny-golden-test-")
        self.addCleanup(temporary.cleanup)
        self.project = Path(temporary.name) / "project with spaces"
        self.golden = self.project / "golden"
        (self.golden / "nested").mkdir(parents=True)
        (self.golden / "nested/table.tsv").write_bytes(b"name\tvalue\nalpha\t1\n")
        self.exported = self.project / "exported"
        shutil.copytree(self.golden, self.exported)
        self.lake = Path(temporary.name) / "lake"
        self.lake.write_text(FAKE_LAKE)
        self.lake.chmod(0o700)

    def run_check(self, expected):
        original = files(self.golden)
        result = subprocess.run(
            [sys.executable, str(CHECKER), str(self.project), "--lake", str(self.lake)],
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(
            result.returncode == 0, expected, result.stdout + result.stderr
        )
        self.assertEqual(files(self.golden), original)
        return result.stdout + result.stderr

    def test_matching_tables_rebuild_proofs_and_preserve_checkout(self):
        self.run_check(True)
        self.assertEqual((self.project / "calls").read_text(), "build\nexe\n")

    def test_stale_table_rejected(self):
        (self.exported / "nested/table.tsv").write_bytes(b"changed\n")
        self.assertIn("changed: nested/table.tsv", self.run_check(False))

    def test_missing_table_rejected(self):
        (self.exported / "nested/table.tsv").unlink()
        self.assertIn("missing: nested/table.tsv", self.run_check(False))

    def test_extra_table_rejected(self):
        (self.exported / "extra.tsv").write_bytes(b"new\n")
        self.assertIn("extra: extra.tsv", self.run_check(False))

    def test_build_failure_rejected_before_export(self):
        (self.project / "fail-build").touch()
        self.run_check(False)
        self.assertEqual((self.project / "calls").read_text(), "build\n")

    def test_export_failure_rejected(self):
        (self.project / "fail-exe").touch()
        self.run_check(False)
        self.assertEqual((self.project / "calls").read_text(), "build\nexe\n")

    def test_empty_committed_fixture_set_rejected(self):
        (self.golden / "nested/table.tsv").unlink()
        self.assertIn("no committed golden files", self.run_check(False))
        self.assertFalse((self.project / "calls").exists())


if __name__ == "__main__":
    unittest.main()
