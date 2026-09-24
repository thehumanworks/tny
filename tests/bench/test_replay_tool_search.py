#!/usr/bin/env python3
"""Unit check that replay fixtures preserve literal code search patterns."""

import tempfile
import unittest
from pathlib import Path

from replay_tool_search import mapped_path, seed


class ReplaySeedTest(unittest.TestCase):
    def test_grep_keeps_special_characters(self) -> None:
        patterns = (
            'join(this.agentDir, "extensions")',
            "free(abs)",
            "argv[0]",
            "$(CC)",
            "a || b",
            "user?.name",
        )
        with tempfile.TemporaryDirectory() as td:
            work = Path(td)
            for pattern in patterns:
                payload = seed(work, "grep_files", {"pattern": pattern})
                self.assertEqual(pattern, payload["pattern"])
                self.assertEqual(pattern + "\n", (work / "fixture.txt").read_text())

    def test_outside_paths_and_braces_stay_distinct(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            work = Path(td) / "workspace"
            work.mkdir()
            mapped = mapped_path(
                work, "/elsewhere/node_modules/pkg", "/original/workspace"
            )
            self.assertEqual(Path(td) / "outside/node_modules/pkg", mapped)
            self.assertEqual(
                work, mapped_path(work, "/original/workspace", "/original/workspace")
            )
            seed(work, "glob_files", {"pattern": "**/{AGENTS.md,CLAUDE.md}"})
            self.assertTrue((work / "AGENTS.md").is_file())
            self.assertFalse((work / "{AGENTS.md,CLAUDE.md}").exists())


if __name__ == "__main__":
    unittest.main()
