#!/usr/bin/env python3
"""Search scope and truthful coverage through real tools on synthetic HTTP wires."""

import json
import re
import sys
import unittest

import test_execution_code_mode as fixture
from code_mode_fixture import python_string


@unittest.skipIf(
    fixture.WASM, "native execution; Wasm keeps its explicit unsupported seam"
)
class SearchCoverage(unittest.TestCase):
    setUp = fixture.ExecutionCodeMode.setUp
    close_server = fixture.ExecutionCodeMode.close_server
    start = fixture.ExecutionCodeMode.start
    run_code = fixture.ExecutionCodeMode.run_code

    def write(self, name, content):
        path = self.workspace / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, bytes):
            path.write_bytes(content)
        else:
            path.write_text(content, encoding="utf-8")
        return path

    def search(self, tool, arguments, wire="responses"):
        return self.run_code(
            f"print(tools.call({tool!r}, {python_string(json.dumps(arguments))}))",
            wire=wire,
        )

    def coverage(self, output):
        match = re.search(r"^\[search (\{[^\n]+\})\]", output, re.MULTILINE)
        self.assertIsNotNone(match, output[:1000])
        return json.loads(match.group(1))

    def test_hidden_configuration_and_explicit_scope_on_both_wires(self):
        self.write(".github/workflows/check.yml", "scope_marker workflow\n")
        self.write(".agents/skills/check.md", "scope_marker skill\n")
        self.write(".editorconfig", "scope_marker editor\n")
        self.write(".env", "scope_marker excluded_secret_fixture\n")
        self.write(".env.local", "scope_marker excluded_local_fixture\n")
        self.write(".git", "scope_marker excluded_vcs_fixture\n")
        self.write(".tny/state.json", "scope_marker internal_state\n")
        self.write(".venv/module.py", "scope_marker virtual_environment\n")
        self.write("build/generated.c", "scope_marker generated\n")
        self.write("src/main.c", "scope_marker source\n")
        self.write("src/skip.c", "scope_marker excluded\n")
        for wire in ("responses", "chat"):
            with self.subTest(wire=wire):
                output = self.search("grep_files", {"pattern": "scope_marker"}, wire)
                meta = self.coverage(output)
                self.assertFalse(meta["partial"])
                self.assertEqual(meta["files"], 5)
                self.assertEqual(meta["scanned"], 5)
                for path in (
                    ".github/workflows/check.yml",
                    ".agents/skills/check.md",
                    ".editorconfig",
                ):
                    self.assertIn(path, output)
                for value in (
                    "excluded_secret_fixture",
                    "excluded_local_fixture",
                    "excluded_vcs_fixture",
                    "generated.c",
                    "internal_state",
                    "virtual_environment",
                ):
                    self.assertNotIn(value, output)
                scoped = self.search(
                    "grep_files",
                    {
                        "pattern": "scope_marker",
                        "include": "src/*",
                        "exclude": "*skip.c",
                    },
                    wire,
                )
                self.assertEqual(self.coverage(scoped)["files"], 1)
                self.assertIn("src/main.c:1:", scoped)
                self.assertNotIn("src/skip.c:", scoped)
                self.assertNotIn(".github/workflows/check.yml:", scoped)
                override = self.search(
                    "grep_files",
                    {
                        "pattern": "scope_marker",
                        "include_hidden": False,
                        "include_generated": True,
                    },
                    wire,
                )
                self.assertEqual(self.coverage(override)["files"], 3)
                self.assertIn("build/generated.c:1:", override)
                self.assertNotIn(".editorconfig:", override)
                direct = self.search(
                    "grep_files", {"pattern": "scope_marker", "path": ".env"}, wire
                )
                self.assertIn("excluded_secret_fixture", direct)
                environment = self.search(
                    "grep_files",
                    {
                        "pattern": "scope_marker",
                        "include": ".venv/*",
                        "include_generated": True,
                    },
                    wire,
                )
                self.assertEqual(self.coverage(environment)["files"], 1)
                self.assertIn("virtual_environment", environment)

    def test_partial_reads_cannot_claim_exhaustive_no_match(self):
        self.write("binary.bin", b"\x00not_text\n")
        self.write("late-nul.bin", b"a" * 5000 + b"\x00tail\n")
        self.write("invalid-utf8.bin", b"\xffinvalid\n")
        large = self.write("huge.txt", "")
        with large.open("wb") as stream:
            stream.truncate(2 * 1024 * 1024 + 1)
        output = self.search("grep_files", {"pattern": "absent"})
        meta = self.coverage(output)
        self.assertTrue(meta["partial"])
        self.assertEqual(meta["skipped"], 4)
        self.assertEqual(meta["files"], 4)
        self.assertNotEqual(output.strip(), "(no matches)")
        missing = self.search("grep_files", {"pattern": "absent", "path": "missing"})
        self.assertIn("error", missing.lower())
        self.assertNotEqual(missing.strip(), "(no matches)")

    def test_hit_and_line_limits_are_explicit(self):
        self.write("hits.txt", "needle " + "x" * 400 + "\n" + "needle\n" * 510)
        output = self.search("grep_files", {"pattern": "needle"})
        meta = self.coverage(output)
        self.assertTrue(meta["partial"])
        self.assertTrue(meta["hit_limit"])
        self.assertGreater(meta["line_truncations"], 0)
        exact = self.search("grep_files", {"pattern": "needle", "path": "hits.txt"})
        self.assertTrue(self.coverage(exact)["hit_limit"])

    def test_glob_limit_and_symlink_scope(self):
        for index in range(1001):
            self.write(f"files/{index:04d}.txt", "")
        outside = self.home / "outside"
        outside.mkdir()
        (outside / "outside.txt").write_text("needle", encoding="utf-8")
        (self.workspace / "link").symlink_to(outside, target_is_directory=True)
        output = self.search("glob_files", {"pattern": "*.txt"})
        meta = self.coverage(output)
        self.assertTrue(meta["partial"])
        self.assertTrue(meta["hit_limit"])
        self.assertNotIn("outside.txt", output)
        scoped = self.search(
            "glob_files", {"pattern": "*.txt", "include": "files/000?.txt"}
        )
        self.assertFalse(self.coverage(scoped)["partial"])
        self.assertEqual(self.coverage(scoped)["files"], 10)

    def test_walk_limit_reports_partial_coverage(self):
        directory = self.workspace / "many"
        directory.mkdir()
        for index in range(20001):
            (directory / f"{index:05d}").touch()
        output = self.search("glob_files", {"pattern": "absent"})
        meta = self.coverage(output)
        self.assertTrue(meta["partial"])
        self.assertTrue(meta["file_limit"])
        self.assertEqual(meta["files"], 20000)

    def test_coverage_survives_small_result_budget(self):
        self.write(".tny.json", json.dumps({"max_tool_result_bytes": 8}))
        self.write("hits.txt", "needle " + "x" * 400 + "\n" + "needle\n" * 510)
        output = self.search("grep_files", {"pattern": "needle", "path": "hits.txt"})
        meta = self.coverage(output)
        self.assertTrue(meta["partial"])
        self.assertTrue(meta["hit_limit"])
        self.assertGreater(meta["line_truncations"], 0)
        self.assertIn("truncated", output)

    def test_lexical_search_uses_same_scope_and_coverage(self):
        self.write(".github/check.yml", "deployment marker\n")
        self.write("src/main.c", "deployment marker\n")
        self.write("generated/main.c", "deployment marker\n")
        output = self.search(
            "semantic_search", {"query": "deployment", "include": ".github/*"}
        )
        self.assertEqual(self.coverage(output)["files"], 1)
        self.assertIn(".github/check.yml", output)
        self.assertNotIn("src/main.c", output)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
