"""Offline runner checks for verifier status and workspace isolation."""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from report import aggregate, markdown
from run import run_one, verification_outcome, verify_task


class VerificationTest(unittest.TestCase):
    def test_default_scored_suite_excludes_smoke_task(self):
        root = Path(__file__).parent
        scored = sorted(
            path.parent.name for path in (root / "tasks").glob("*/task.json")
        )
        self.assertEqual(len(scored), 12)
        self.assertNotIn("smoke-hello", scored)
        self.assertTrue((root / "tasks-smoke" / "smoke-hello" / "task.json").is_file())

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.task = self.root / "task"
        self.task.mkdir()
        self.workspace = self.root / "workspace"
        self.workspace.mkdir()
        self.final = self.root / "result" / "final_message.txt"
        self.final.parent.mkdir()
        self.final.write_text("")

    def _script(self, body):
        (self.task / "verify.sh").write_text("#!/bin/sh\nset -eu\n" + body)

    def test_verifier_runs_from_task_dir_and_cleans_private_files(self):
        self._script(
            'pwd > "$(dirname "$2")/cwd.txt"\n'
            'touch "$TMPDIR/private"\n'
            'echo "pass: checked"\n'
        )
        result = verify_task(self.task, self.workspace, self.final, timeout_s=2)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(
            (self.final.parent / "cwd.txt").read_text().strip(), str(self.task)
        )
        self.assertFalse(list(self.final.parent.glob(".verify-tmp-*")))

    def test_timeout_and_missing_asan_are_errors(self):
        self._script("sleep 2\n")
        timed = verify_task(self.task, self.workspace, self.final, timeout_s=0.1)
        self.assertEqual(timed.returncode, 124)
        self.assertEqual(verification_outcome(timed, self.final)[0], "error")
        self.assertIn("error: verification timed out", timed.stdout)
        self._script(
            'echo "RuntimeError: ASan runtime is unavailable for this task" '
            '> "$(dirname "$2")/verify.log"\n'
            'echo "fail: oracle failed"\nexit 1\n'
        )
        missing = verify_task(self.task, self.workspace, self.final, timeout_s=2)
        self.assertEqual(verification_outcome(missing, self.final)[0], "error")
        self.assertTrue(
            verification_outcome(missing, self.final)[1].startswith("error:")
        )
        (self.final.parent / "verify.log").unlink()
        self._script('echo "fail: assertion failed"\nexit 1\n')
        failed = verify_task(self.task, self.workspace, self.final, timeout_s=2)
        self.assertEqual(
            verification_outcome(failed, self.final), ("fail", "fail: assertion failed")
        )

    def test_active_workspace_has_own_root_and_no_verifier_files(self):
        task = self.task
        (task / "repo").mkdir()
        (task / "repo" / "README.md").write_text("fixture\n")
        (task / "task.json").write_text(
            json.dumps(
                {
                    "id": "task",
                    "category": "test",
                    "prompt": "test",
                    "timeout_s": 5,
                    "verify_timeout_s": 2,
                }
            )
        )
        self._script(
            'echo "hidden result" > "$(dirname "$2")/verify.log"\n'
            'echo "pass: checked"\n'
        )
        agent = """from pathlib import Path
w = Path.cwd()
assert w.parent.name.startswith('ws-')
assert {p.name for p in w.parent.iterdir()} == {'workspace'}
assert not (w.parent.parent / 'verify.log').exists()
assert not list(w.parent.parent.glob('.harness-hidden.*'))
(w / 'observed.txt').write_text(str(w))
"""

        class FakeProxy:
            def __init__(self, path, auth_file):
                self.base_url = "http://127.0.0.1:1"
                self.rows = []

            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

        out = self.root / "runs"
        args = SimpleNamespace(
            out=out,
            label="offline",
            auth_file=self.root / "auth.json",
            model="gpt-6-luna",
            effort="low",
            tny_bin="unused",
        )
        command = SimpleNamespace(
            command=[sys.executable, "-c", agent], env=os.environ.copy()
        )
        with (
            patch("run.RecordingProxy", FakeProxy),
            patch("run.invocation", return_value=command),
            patch("run.verify_task", wraps=verify_task) as verify_call,
        ):
            row = run_one(args, task, "fake", 1)
        self.assertEqual(verify_call.call_args.args[-1], 2)
        run_dir = out / "offline" / "fake" / "task" / "rep-01"
        self.assertEqual(row["status"], "fail")  # no model requests in the stub
        self.assertEqual((run_dir / "verify.log").read_text().strip(), "hidden result")
        self.assertTrue((run_dir / "workspace" / "observed.txt").is_file())
        self.assertFalse(list(run_dir.glob("ws-*")))

    def test_report_separates_environment_error_from_task_failure(self):
        common = {
            "_path": str(self.root / "result.json"),
            "harness": "fake",
            "model": "gpt-6-luna",
            "task": "task",
            "requests": 0,
            "request_rows": [],
            "wall_s": 1,
        }
        report = aggregate(
            [
                {**common, "rep": 1, "pass": True, "status": "pass"},
                {
                    **common,
                    "rep": 2,
                    "pass": False,
                    "status": "error",
                    "reason": "error: ASan runtime is unavailable",
                },
            ]
        )
        headline = report["headline"][0]
        self.assertEqual((headline["evaluated_runs"], headline["error_runs"]), (1, 1))
        self.assertEqual(headline["pass_rate"], 1)
        self.assertEqual(report["pass_matrix"]["task"]["fake"]["errors"], 1)
        self.assertIn("ASan runtime is unavailable", markdown(report))


if __name__ == "__main__":
    unittest.main()
