#!/usr/bin/env python3
"""The real make-test recipe must not silently skip its integration runner."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class MakeTestContract(unittest.TestCase):
    def test_runner_must_exist_and_be_executable(self):
        lines = (ROOT / "Makefile").read_text().splitlines(keepends=True)
        start = next(i for i, line in enumerate(lines) if line.startswith("test:"))
        recipe = []
        for line in lines[start + 1 :]:
            if not line.startswith("\t"):
                break
            recipe.append(line)
        self.assertTrue(recipe)
        with tempfile.TemporaryDirectory(prefix="tny-make-contract-") as temp:
            root = Path(temp)
            # Isolate just the real recipe; compilation prerequisites are not
            # relevant to whether a missing runner incorrectly reports success.
            (root / "Makefile").write_text("test:\n" + "".join(recipe))
            runner = root / "tests/integration/run.sh"
            runner.parent.mkdir(parents=True)
            env = {
                k: v
                for k, v in os.environ.items()
                if k not in {"MAKEFLAGS", "MFLAGS", "MAKELEVEL"}
            }

            def run():
                return subprocess.run(
                    ["make", "test"],
                    cwd=root,
                    env=env,
                    capture_output=True,
                    text=True,
                    timeout=10,
                    check=False,
                )

            self.assertNotEqual(
                run().returncode, 0, "missing runner was silently skipped"
            )
            runner.write_text("#!/bin/sh\nprintf ran > integration-ran\n")
            runner.chmod(0o644)
            self.assertNotEqual(
                run().returncode, 0, "non-executable runner was silently skipped"
            )
            self.assertFalse((root / "integration-ran").exists())
            runner.chmod(0o755)
            result = run()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((root / "integration-ran").read_text(), "ran")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
