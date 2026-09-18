#!/usr/bin/env python3
"""Exercise make's integration-runner and cleanup contracts in temporary trees."""

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


class MakeCleanContract(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="tny-make-clean-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        (self.root / "Makefile").write_text((ROOT / "Makefile").read_text())
        self.env = {
            k: v
            for k, v in os.environ.items()
            if k not in {"MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEFILES"}
        }

    def artifact(self, directory):
        path = self.root / directory / "artifact"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("keep or remove as a unit\n")
        return path

    def clean(self, *args):
        result = subprocess.run(
            [
                "make",
                "--no-print-directory",
                "TNY_VERSION=1.0.0",
                "LIBTNY_MACH_CURRENT_VERSION=1.0.0",
                "clean",
                *args,
            ],
            cwd=self.root,
            env=self.env,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_default_removes_build_directories_only(self):
        removed = ["build", "dist", "build-acp", "build-sdk-final", "build-with space"]
        for directory in removed:
            self.artifact(directory)
        kept = [
            self.artifact("src"),
            self.artifact("tnytty/build"),
            self.artifact("nested/build-other"),
            self.artifact("external"),
        ]
        note = self.root / "build-notes.txt"
        note.write_text("not a build directory\n")
        link = self.root / "build-external"
        link.symlink_to(self.root / "external", target_is_directory=True)
        self.clean()
        for directory in removed:
            self.assertFalse((self.root / directory).exists(), directory)
        for path in [*kept, note, link]:
            self.assertTrue(path.exists(), str(path))
        self.clean()  # Missing outputs and an unmatched directory glob are safe.

    def test_empty_tree_is_safe(self):
        self.clean()
        self.clean()
        self.assertTrue((self.root / "Makefile").exists())

    def test_custom_build_keeps_other_builds(self):
        for selected in ("build-focus", "out", "./build"):
            with self.subTest(build=selected):
                self.artifact(selected)
                self.artifact("dist")
                other = self.artifact("build-other")
                default = self.artifact("build")
                self.clean(f"BUILD={selected}")
                self.assertFalse((self.root / selected).exists())
                self.assertFalse((self.root / "dist").exists())
                self.assertTrue(other.exists())
                if selected != "./build":
                    self.assertTrue(default.exists())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
