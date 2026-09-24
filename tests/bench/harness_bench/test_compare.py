"""Offline checks for task-paired comparison and saved-body firing counts."""

import gzip
import json
import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from compare import compare_runs


class ComparisonTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.arm_a = self.root / "before"
        self.arm_b = self.root / "after"
        cases = (
            ("x", 1, True, True, 10),
            ("x", 2, False, True, 20),
            ("y", 1, False, False, 30),
            ("y", 2, True, False, 40),
        )
        for task, rep, passed_a, passed_b, input_a in cases:
            self._write(self.arm_a, task, rep, passed_a, input_a, 2, input_a, False)
            self._write(
                self.arm_b,
                task,
                rep,
                passed_b,
                input_a // 2,
                1,
                input_a / 2,
                rep == 2,
            )

    def _write(
        self, arm, task, rep, passed, input_tokens, output_tokens, wall_s, fired
    ):
        run = arm / "tny" / task / f"rep-{rep:02d}"
        proxy = run / "proxy"
        proxy.mkdir(parents=True)
        (proxy / "request-0001.json.gz").write_bytes(
            gzip.compress(
                json.dumps({"input": "FLAG_TEST" if fired else "plain"}).encode()
            )
        )
        (run / "result.json").write_text(
            json.dumps(
                {
                    "harness": "tny",
                    "task": task,
                    "rep": rep,
                    "pass": passed,
                    "model": "gpt-6-luna",
                    "effort": "low",
                    "requests": 1,
                    "wall_s": wall_s,
                    "request_rows": [
                        {
                            "body_file": "request-0001.json.gz",
                            "input_tokens": input_tokens,
                            "cached_input_tokens": 0,
                            "cache_write_tokens": 0,
                            "output_tokens": output_tokens,
                            "reasoning_tokens": 0,
                        }
                    ],
                }
            )
        )

    def test_paired_metrics_count_failed_runs_and_feature_firing(self):
        report = compare_runs(self.arm_a, self.arm_b, fires=["compact=FLAG_TEST"])
        self.assertEqual(report["paired_runs"], 4)
        self.assertEqual(report["bootstrap_resamples"], 10_000)
        self.assertEqual(report["arm_a"]["passes"], 2)
        self.assertEqual(report["arm_b"]["passes"], 2)
        self.assertEqual(report["paired"]["delta_success_pp"]["value"], 0)
        self.assertEqual(report["paired"]["delta_success_pp"]["ci95"], [-50, 50])
        self.assertEqual(
            report["paired"]["delta_success_pp"]["verdict"], "inconclusive"
        )
        self.assertEqual(
            report["arm_a"]["metrics"]["ite_per_completed_task"]["value"], 70
        )
        self.assertEqual(
            report["arm_a"]["metrics"]["ite_per_completed_task"]["ci95"],
            [50, 90],
        )
        self.assertEqual(
            report["arm_b"]["metrics"]["ite_per_completed_task"]["value"], 35
        )
        self.assertEqual(
            report["arm_b"]["metrics"]["ite_per_completed_task"]["ci95"],
            [12.5, None],
        )
        self.assertTrue(
            math.isclose(
                report["arm_a"]["metrics"]["usd_per_completed_task"]["value"], 7e-6
            )
        )
        for name in ("ite", "usd", "mean_context_tokens", "output_tokens", "wall_s"):
            ratio = report["paired"]["geometric_ratios_b_over_a"][name]
            self.assertTrue(math.isclose(ratio["value"], 0.5), name)
            self.assertTrue(
                all(math.isclose(value, 0.5) for value in ratio["ci95"]), name
            )
        self.assertEqual(
            report["paired"]["geometric_ratios_b_over_a"]["requests"]["value"], 1
        )
        self.assertEqual(
            [(row["fail_to_pass"], row["pass_to_fail"]) for row in report["flips"]],
            [(1, 0), (0, 1)],
        )
        self.assertEqual(
            report["features"][0]["a"], {"matches": 0, "requests": 0, "runs": 0}
        )
        self.assertEqual(
            report["features"][0]["b"], {"matches": 2, "requests": 2, "runs": 2}
        )

    def test_cli_writes_markdown_and_json_and_rejects_missing_pair(self):
        output = self.root / "comparison.md"
        completed = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).with_name("report.py")),
                "--compare",
                str(self.arm_a),
                str(self.arm_b),
                "--harness",
                "tny",
                "--fire",
                "compact=FLAG_TEST",
                "--out",
                str(output),
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        self.assertIn("Per-task flips", completed.stdout)
        self.assertIn("Feature firing", output.read_text())
        self.assertEqual(
            json.loads(output.with_suffix(".json").read_text())["paired_runs"], 4
        )
        (self.arm_b / "tny" / "y" / "rep-02" / "result.json").unlink()
        with self.assertRaisesRegex(ValueError, "unpaired task/rep"):
            compare_runs(self.arm_a, self.arm_b)

    def test_single_run_per_task_can_pair_without_rep(self):
        for arm in (self.arm_a, self.arm_b):
            for task in ("x", "y"):
                (arm / "tny" / task / "rep-02" / "result.json").unlink()
                if arm == self.arm_b:
                    continue
                source = arm / "tny" / task / "rep-01" / "result.json"
                row = json.loads(source.read_text())
                row.pop("rep")
                destination = arm / "tny" / task / "result.json"
                destination.write_text(json.dumps(row))
                # The sibling proxy body stays beside the result record.
                proxy = destination.parent / "proxy"
                proxy.mkdir()
                (proxy / "request-0001.json.gz").write_bytes(
                    (source.parent / "proxy" / "request-0001.json.gz").read_bytes()
                )
                (arm / "tny" / task / "rep-01" / "result.json").unlink()
        report = compare_runs(self.arm_a, self.arm_b)
        self.assertEqual(report["paired_runs"], 2)


if __name__ == "__main__":
    unittest.main()
