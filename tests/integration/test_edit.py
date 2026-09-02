#!/usr/bin/env python3
"""Process-level contract for the stateless `tny edit` verb."""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TNY = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", "build/tny"))
if not TNY.is_absolute():
    TNY = ROOT / TNY


class EditCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="tny-edit-cli-")
        self.directory = Path(self.temp.name)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("TNY_")}

    def tearDown(self) -> None:
        self.temp.cleanup()

    def run_edit(
        self, path: Path | str, payload: str, *options: str
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(TNY), "edit", *options, str(path)],
            input=payload,
            text=True,
            capture_output=True,
            cwd=self.directory,
            env=self.env,
            timeout=10,
            check=False,
        )

    def test_fence_input_and_relative_path(self) -> None:
        path = self.directory / "fence.txt"
        path.write_text("alpha\nold line\nomega\n")
        result = self.run_edit(
            "fence.txt", "*** SEARCH\nold line\n*** REPLACE\nnew line\n*** END\n"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("replaced 1 occurrence", result.stdout)
        self.assertEqual(path.read_text(), "alpha\nnew line\nomega\n")

    def test_custom_marker(self) -> None:
        path = self.directory / "marker.txt"
        path.write_text("old")
        result = self.run_edit(
            path, "@@ SEARCH\nold\n@@ REPLACE\nnew\n@@ END\n", "--marker", "@@"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(path.read_text(), "new")

    def test_json_input_output_and_replace_all(self) -> None:
        path = self.directory / "json.txt"
        path.write_text("old old")
        payload = json.dumps({"old": "old", "new": "new", "replace_all": True})
        result = self.run_edit(path, payload, "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            json.loads(result.stdout),
            {"kind": "edit", "path": str(path), "matches": 2, "replaced": 2},
        )
        self.assertEqual(path.read_text(), "new new")

    def test_zero_and_multiple_are_exit_two_without_writes(self) -> None:
        zero = self.directory / "zero.txt"
        zero.write_text("alpha\ncorrect target\nomega\n")
        result = self.run_edit(
            zero, "*** SEARCH\ncorrect targat\n*** REPLACE\nnew\n*** END\n"
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("0 matches", result.stderr)
        self.assertIn("nearest unique context is line 2: correct target", result.stderr)
        self.assertEqual(zero.read_text(), "alpha\ncorrect target\nomega\n")

        many = self.directory / "many.txt"
        many.write_text("old old")
        result = self.run_edit(many, "*** SEARCH\nold\n*** REPLACE\nnew\n*** END\n")
        self.assertEqual(result.returncode, 2)
        self.assertIn("2 matches", result.stderr)
        self.assertEqual(many.read_text(), "old old")

    def test_usage_and_missing_file_are_exit_one(self) -> None:
        result = self.run_edit(
            self.directory / "missing.txt",
            "*** SEARCH\nold\n*** REPLACE\nnew\n*** END\n",
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot read", result.stderr)

        result = self.run_edit(self.directory / "unused.txt", "not a fence")
        self.assertEqual(result.returncode, 1)
        self.assertIn("expected '*** SEARCH'", result.stderr)

    @unittest.skipIf(os.name == "nt", "POSIX signal contract")
    def test_interrupt_is_130_and_does_not_write(self) -> None:
        path = self.directory / "interrupt.txt"
        path.write_text("old")
        process = subprocess.Popen(
            [str(TNY), "edit", str(path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=self.directory,
            env=self.env,
            text=True,
        )
        time.sleep(0.1)
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 130, (stdout, stderr))
        self.assertEqual(path.read_text(), "old")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
