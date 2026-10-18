#!/usr/bin/env python3
"""The schema drift gate reports failures without modifying its inputs."""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class EventSchemaCheck(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="tny-schema-check-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        shutil.copytree(ROOT / "sdk/schema", self.root / "sdk/schema")
        shutil.copytree(ROOT / "include", self.root / "include")

    def check(self):
        return subprocess.run(
            [sys.executable, str(self.root / "sdk/schema/check.py")],
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )

    def test_current_artifacts_pass_without_writes(self):
        paths = list((self.root / "sdk/schema").rglob("*"))
        before = {
            p: (p.read_bytes(), p.stat().st_mtime_ns) for p in paths if p.is_file()
        }
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            before, {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in before}
        )

    def test_stale_artifact_fails_without_repair(self):
        path = self.root / "sdk/schema/generated/events.ts"
        path.write_text("deliberately stale\n")
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("generated/events.ts", result.stderr)
        self.assertEqual(path.read_text(), "deliberately stale\n")

    def test_missing_artifact_fails_without_creation(self):
        path = self.root / "sdk/schema/fixtures/events.json"
        path.unlink()
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("fixtures/events.json", result.stderr)
        self.assertFalse(path.exists())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
