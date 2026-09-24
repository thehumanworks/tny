"""Offline checks for saved-run rescoring and task-version refusal."""

import gzip
import json
import shutil
import tempfile
import unittest
from pathlib import Path

from rescore import rescore
from run import _git_init


class RescoreTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.tasks = root / "tasks"
        self.task = self.tasks / "demo"
        repo = self.task / "repo"
        repo.mkdir(parents=True)
        (repo / "fixture.txt").write_text("original\n")
        (self.task / "task.json").write_text(
            json.dumps(
                {
                    "id": "demo",
                    "category": "bugfix",
                    "prompt": "Repair the demo.",
                    "timeout_s": 30,
                }
            )
        )
        (self.task / "verify.sh").write_text(
            "#!/bin/sh\n"
            'if [ -f "$1/done.txt" ]; then echo "pass: done"; '
            'else echo "fail: missing done"; exit 1; fi\n'
        )
        self.runs = root / "runs"
        self.run_dir = self.runs / "fake" / "demo" / "rep-01"
        self.workspace = self.run_dir / "workspace"
        shutil.copytree(repo, self.workspace)
        _git_init(self.workspace)
        (self.run_dir / "final_message.txt").write_text("")
        proxy = self.run_dir / "proxy"
        proxy.mkdir()
        (proxy / "request-0001.json.gz").write_bytes(
            gzip.compress(
                json.dumps(
                    {"input": [{"role": "user", "content": "Repair the demo."}]}
                ).encode()
            )
        )
        self.result_file = self.run_dir / "result.json"
        self.result_file.write_text(
            json.dumps(
                {
                    "task": "demo",
                    "category": "old",
                    "pass": False,
                    "status": "fail",
                    "reason": "fail: old oracle",
                    "exit_code": 0,
                    "timeout": False,
                    "measurement_valid": True,
                    "model_effort_valid": True,
                    "request_rows": [{"body_file": "request-0001.json.gz"}],
                }
            )
        )

    def test_rescore_rechecks_workspace_and_preserves_original_verdict(self):
        self.assertEqual(rescore(self.runs, self.tasks), 1)
        failed = json.loads(self.result_file.read_text())
        self.assertFalse(failed["pass"])
        self.assertFalse(failed["pass_original"])
        self.assertEqual(failed["reason"], "fail: missing done")
        (self.workspace / "done.txt").write_text("fixed\n")
        self.assertEqual(rescore(self.runs, self.tasks), 1)
        passed = json.loads(self.result_file.read_text())
        self.assertTrue(passed["pass"])
        self.assertFalse(passed["pass_original"])
        self.assertEqual(passed["status"], "pass")
        self.assertEqual(passed["category"], "bugfix")
        self.assertIn("verify_rescored_at", passed)

    def test_changed_repo_or_prompt_refuses_rescore_without_rewriting(self):
        original = self.result_file.read_bytes()
        (self.task / "repo" / "fixture.txt").write_text("changed\n")
        with self.assertRaisesRegex(ValueError, "repo/ changed"):
            rescore(self.runs, self.tasks)
        self.assertEqual(self.result_file.read_bytes(), original)
        (self.task / "repo" / "fixture.txt").write_text("original\n")
        task = json.loads((self.task / "task.json").read_text())
        task["prompt"] = "A different request."
        (self.task / "task.json").write_text(json.dumps(task))
        with self.assertRaisesRegex(ValueError, "prompt changed"):
            rescore(self.runs, self.tasks)
        self.assertEqual(self.result_file.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
