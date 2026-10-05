#!/usr/bin/env python3
"""Exercise leak-gate results without a platform leak checker or native build."""

import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLEAN = (
    "Total: 2 tests (1 ticks, 0.001 sec), 3 assertions\nPass: 2, fail: 0, skip: 0.\n"
)


@unittest.skipUnless(shutil.which("bash"), "leak driver requires bash")
class LeakcheckTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="tny-leakcheck-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        script = self.root / "scripts/leakcheck.sh"
        script.parent.mkdir()
        shutil.copyfile(ROOT / "scripts/leakcheck.sh", script)
        self.script = script
        self.trace = self.root / "checker-calls.jsonl"
        self.checker = self.executable(
            "fake-leaks",
            """import json, os, subprocess, sys
args = sys.argv[sys.argv.index('--') + 1:]
with open(os.environ['LEAK_FIXTURE_TRACE'], 'a') as trace:
    trace.write(json.dumps(args) + '\\n')
# Deliberately discard target status, as macOS leaks --atExit can do.
subprocess.run(args, check=False)
print('Process 123: 0 leaks for 0 total leaked bytes.')
sys.exit(int(os.environ.get('LEAK_FIXTURE_CHECKER_STATUS', '0')))
""",
        )
        self.executable(
            "unit",
            """import json, os, sys
suite = sys.argv[2]
sys.stdout.write(json.loads(os.environ['LEAK_FIXTURE_OUTPUTS'])[suite])
sys.exit(json.loads(os.environ.get('LEAK_FIXTURE_TARGET_STATUSES', '{}')).get(suite, 0))
""",
        )
        self.executable("cli", "print('CLI smoke output')\n")

    def executable(self, name, source):
        program = self.root / f"{name}.py"
        program.write_text(source)
        wrapper = self.root / name
        wrapper.write_text(
            f"#!{shutil.which('bash')}\nexec {shlex.quote(sys.executable)} "
            f'{shlex.quote(str(program))} "$@"\n'
        )
        wrapper.chmod(0o755)
        return wrapper

    def run_gate(self, outputs, *, checker_status=0, target_statuses=None):
        env = dict(os.environ)
        env.update(
            TEST_BIN="unit",
            CLI_BIN="cli",
            LEAKS=str(self.checker),
            LEAK_SUITES=" ".join(outputs),
            LEAK_FIXTURE_OUTPUTS=json.dumps(outputs),
            LEAK_FIXTURE_TRACE=str(self.trace),
            LEAK_FIXTURE_CHECKER_STATUS=str(checker_status),
            LEAK_FIXTURE_TARGET_STATUSES=json.dumps(target_statuses or {}),
        )
        return subprocess.run(
            ["bash", str(self.script), "leaks"],
            cwd=self.root,
            env=env,
            text=True,
            capture_output=True,
            timeout=20,
            check=False,
        )

    def calls(self):
        return [json.loads(line) for line in self.trace.read_text().splitlines()]

    def test_clean_unit_and_cli(self):
        result = self.run_gate({"clean_suite": CLEAN})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("leaks: clean (leaks)", result.stdout)
        self.assertEqual(
            self.calls(),
            [
                ["./unit", "-s", "clean_suite", "-e"],
                ["./cli", "--version"],
                ["./cli", "--help"],
                ["./cli", "ask", "--help"],
                ["./cli", "doctor", "--json"],
            ],
        )

    def test_checker_failure_remains_failure(self):
        result = self.run_gate({"clean_suite": CLEAN}, checker_status=3)
        self.assertEqual(result.returncode, 1)
        self.assertIn("LEAK/ERROR: clean_suite", result.stderr)
        self.assertIn("Pass: 2, fail: 0, skip: 0.", result.stderr)
        self.assertIn("0 leaks for 0 total leaked bytes", result.stderr)
        self.assertIn("LEAK/ERROR: tny --version", result.stderr)

    def test_checker_success_cannot_hide_unit_failure(self):
        failure = (
            "FAIL broken_test: expected != actual (test.c:12)\n"
            "Total: 2 tests (1 ticks, 0.001 sec), 3 assertions\n"
            "Pass: 1, fail: 1, skip: 0.\n"
        )
        result = self.run_gate(
            {"broken_suite": failure}, target_statuses={"broken_suite": 1}
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL broken_test: expected != actual (test.c:12)", result.stderr)
        self.assertIn("Pass: 1, fail: 1, skip: 0.", result.stderr)
        self.assertIn("LEAK/ERROR: broken_suite", result.stderr)
        self.assertIn("leaks: FAILED", result.stderr)
        self.assertEqual(
            (self.root / "build/leakcheck/broken_suite.log").read_text(),
            failure + "Process 123: 0 leaks for 0 total leaked bytes.\n",
        )

    def test_invalid_or_empty_unit_summary_fails(self):
        invalid = {
            "absent": "suite ran but no summary\n",
            "empty": "",
            "malformed": "Pass: nope, fail: 0, skip: 0.\n",
            "trailing_junk": "Pass: 1, fail: 0, skip: 0. ignored\n",
            "zero_tests": "Total: 0 tests\nPass: 0, fail: 0, skip: 0.\n",
            "only_skips": "Pass: 0, fail: 0, skip: 2.\n",
            "duplicate": CLEAN + CLEAN,
            "unanchored": "prefix Pass: 1, fail: 0, skip: 0.\n",
            "nonzero_fail": "Pass: 1, fail: 1, skip: 0.\n",
            "contradictory": "FAIL broken_test: failed\n" + CLEAN,
        }
        for name, output in invalid.items():
            with self.subTest(name=name):
                result = self.run_gate({name: output})
                self.assertEqual(result.returncode, 1)
                self.assertIn(
                    "unit test result missing, invalid, or failing", result.stderr
                )
                self.assertIn(f"LEAK/ERROR: {name}", result.stderr)
                self.assertNotIn("leaks: clean", result.stdout)

    def test_skips_with_passing_tests_are_allowed(self):
        result = self.run_gate({"some_skips": "Pass: 1, fail: 0, skip: 2.\n"})
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_failure_in_later_suite_does_not_stop_remaining_checks(self):
        outputs = {
            "first": CLEAN,
            "broken": "FAIL regression: failed\nPass: 1, fail: 1, skip: 0.\n",
            "last": CLEAN,
        }
        result = self.run_gate(outputs)
        self.assertEqual(result.returncode, 1)
        self.assertIn("LEAK/ERROR: broken", result.stderr)
        self.assertIn("== leak: last", result.stdout)
        self.assertIn("== leak: tny doctor --json", result.stdout)
        self.assertEqual([call[2] for call in self.calls()[:3]], list(outputs))
        self.assertEqual(len(self.calls()), 7)

    def test_empty_suite_selection_fails(self):
        for outputs in ({}, {" \t ": CLEAN}):
            with self.subTest(outputs=outputs):
                result = self.run_gate(outputs)
                self.assertEqual(result.returncode, 2)
                self.assertIn("LEAK_SUITES is empty", result.stderr)
                self.assertFalse(self.trace.exists())


if __name__ == "__main__":
    # The integration runner appends its native binary; these fixtures do not
    # invoke it, and unittest must not interpret the path as a test name.
    if len(sys.argv) > 1 and Path(sys.argv[1]).is_file():
        sys.argv.pop(1)
    unittest.main()
