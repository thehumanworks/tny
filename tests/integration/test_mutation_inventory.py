#!/usr/bin/env python3
"""Prevent vacuous mutation success after ownership representations change."""

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "tny_mutation", ROOT / "tests/mutation/mutate.py"
)
assert SPEC is not None and SPEC.loader is not None
MUTATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MUTATION)


class MutationInventory(unittest.TestCase):
    def test_custom_tool_cpp_selection_generates_real_semantic_mutants(self):
        selected = [
            item
            for item in MUTATION.TARGETS
            if len(item) > 4 and item[4] == "libtny-custom-tools"
        ]
        self.assertEqual(len(selected), 1)
        path, names, pattern, *_ = selected[0]
        mutants = MUTATION.gen_mutants(str(ROOT / path), names, pattern)
        self.assertGreaterEqual(len(mutants), 3)
        self.assertTrue(
            any("call.generation !=" in item["content"] for item in mutants)
        )
        self.assertTrue(any("call.epoch !=" in item["content"] for item in mutants))

    def test_empty_focus_fails_before_running_or_editing(self):
        with (
            patch.object(sys, "argv", ["mutate", "--focus", "missing-focus"]),
            patch.object(MUTATION, "run") as run,
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(MUTATION.main(), 2)
        run.assert_not_called()

    def exercise(self, results):
        with tempfile.TemporaryDirectory(prefix="tny-mutation-inventory-") as directory:
            source = Path(directory) / "fixture.c"
            original = "int fixture(void) { return 1; }\n"
            source.write_text(original)
            mutant = {
                "file": str(source),
                "text": original.replace("1", "0"),
                "line": 1,
                "op": "1 -> 0",
                "content": original.strip(),
            }
            with (
                patch.object(sys, "argv", ["mutate", "--focus", "fixture"]),
                patch.object(
                    MUTATION,
                    "TARGETS",
                    [(str(source), None, None, "fixture.py", "fixture")],
                ),
                patch.object(MUTATION, "gen_mutants", return_value=[mutant]),
                patch.object(MUTATION, "run", side_effect=results),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                result = MUTATION.main()
            self.assertEqual(source.read_text(), original)
            return result

    def test_unmodified_baseline_failure_is_not_a_kill(self):
        self.assertEqual(self.exercise([(1, "compile failure")]), 2)

    def test_all_uncompilable_mutants_fail_closed(self):
        self.assertEqual(
            self.exercise([(0, ""), (0, ""), (1, "invalid mutant"), (0, ""), (0, "")]),
            2,
        )

    def test_failed_restoration_invalidates_a_behavioral_kill(self):
        self.assertEqual(
            self.exercise(
                [(0, ""), (0, ""), (0, ""), (1, "assertion"), (1, "restore failure")]
            ),
            2,
        )

    def test_valid_killed_mutant_and_restored_build_pass(self):
        self.assertEqual(
            self.exercise(
                [(0, ""), (0, ""), (0, ""), (1, "assertion"), (0, ""), (0, "")]
            ),
            0,
        )


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
