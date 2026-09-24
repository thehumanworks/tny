#!/usr/bin/env python3
"""Unit check that replay fixtures preserve literal code search patterns."""

import tempfile
import unittest
from pathlib import Path

from replay_tool_search import seed


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


if __name__ == "__main__":
    unittest.main()
