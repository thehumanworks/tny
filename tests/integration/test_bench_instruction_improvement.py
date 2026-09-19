#!/usr/bin/env python3
"""Offline fixture oracles, replay protocol, and real-controller integration.

Run directly with Python. Set TNY_IMPROVE_CONTROLLER to test a separate controller
checkout; otherwise the integration test skips explicitly until it is merged.
No provider credentials, model calls, or mock selection controller are used.
"""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = (
    Path(__file__).resolve().parents[1] / "bench" / "bench_instruction_improvement.py"
)
SPEC = importlib.util.spec_from_file_location("bench_instruction_improvement", SCRIPT)
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)
CONTROLLER = Path(os.environ.get("TNY_IMPROVE_CONTROLLER", BENCH.DEFAULT_CONTROLLER))


class InstructionBenchmarkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cases, self.hashes = BENCH.make_fixtures(self.root)

    def evaluate(self, strategy, case):
        response = BENCH.evaluate(
            {"instructions": BENCH.INSTRUCTIONS[strategy], "case": case}
        )
        record = json.loads(response["feedback"])
        self.assertEqual(
            response["passed"], record["actual_hex"] == record["expected_hex"]
        )
        self.assertEqual(
            response["cost"], sum(op.get("bytes", 0) for op in record["operations"])
        )
        return record

    def test_actual_reads_and_exact_oracle_across_all_splits(self):
        for cases in self.cases.values():
            for case in cases:
                rows = [
                    self.evaluate(strategy, case) for strategy in BENCH.STRATEGIES[:3]
                ]
                self.assertTrue(all(row["passed"] for row in rows))
                self.assertGreater(rows[0]["cost"], rows[1]["cost"])
                self.assertGreater(rows[1]["cost"], rows[2]["cost"])
                self.assertEqual([row["file_reads"] for row in rows], [8, 4, 4])
                for row in rows:
                    self.assertEqual(row["directory_lists"], 1)
                    for operation in row["operations"][1:]:
                        path = Path(case["input"]["root"]) / operation["path"]
                        with path.open("rb") as stream:
                            actual = (
                                stream.readline()
                                if operation["operation"] == "read_header"
                                else stream.read()
                            )
                        self.assertEqual(operation["bytes"], len(actual))
                        self.assertEqual(operation["sha256"], BENCH.sha256(actual))

    def test_shortcut_is_cheaper_but_really_fails_validation(self):
        for case in self.cases["train"]:
            shortcut = self.evaluate("first-record", case)
            self.assertTrue(shortcut["passed"])
            self.assertLess(
                shortcut["cost"], self.evaluate("record-headers", case)["cost"]
            )
        results = [
            self.evaluate("first-record", case) for case in self.cases["validation"]
        ]
        self.assertEqual([row["passed"] for row in results], [False, True, False])
        self.assertEqual(results[0]["actual_hex"], "")
        self.assertNotEqual(results[0]["expected_hex"], "")

    def test_corruption_not_declared_success_controls_outcome(self):
        case = self.cases["train"][0]
        target = Path(case["input"]["root"]) / "00.record"
        original = target.read_bytes()
        target.write_bytes(original.replace(b"answer-", b"broken-", 1))
        for strategy in BENCH.STRATEGIES:
            self.assertFalse(self.evaluate(strategy, case)["passed"])
        target.write_bytes(original)
        Path(case["input"]["oracle"]).write_bytes(b"different oracle\n")
        self.assertFalse(self.evaluate("scan-all", case)["passed"])

    def test_missing_newline_and_duplicate_matches_fail_exact_check(self):
        case = self.cases["train"][0]
        target = Path(case["input"]["root"]) / "00.record"
        expected = Path(case["input"]["oracle"]).read_bytes()
        target.write_bytes(expected.rstrip(b"\n"))
        self.assertFalse(self.evaluate("record-headers", case)["passed"])
        target.write_bytes(expected)
        (target.parent / "01.record").write_bytes(expected)
        for strategy in BENCH.STRATEGIES[:3]:
            self.assertFalse(self.evaluate(strategy, case)["passed"])

    def test_read_cost_changes_when_actual_file_grows(self):
        case = self.cases["train"][0]
        before = self.evaluate("scan-all", case)
        noise = Path(case["input"]["root"]) / "noise-0.txt"
        with noise.open("ab") as stream:
            stream.write(b"123456789")
        after = self.evaluate("scan-all", case)
        self.assertTrue(after["passed"])
        self.assertEqual(after["cost"] - before["cost"], 9)

    def test_fresh_inputs_are_reproducible_and_split_content_is_distinct(self):
        with tempfile.TemporaryDirectory() as other:
            _, hashes = BENCH.make_fixtures(Path(other))
        self.assertEqual(hashes, self.hashes)
        self.assertEqual(len(hashes), 81)
        self.assertEqual(len(set(hashes.values())), 81)

    def test_proposer_requires_parent_feedback_not_round_alone(self):
        for index, strategy in enumerate(BENCH.STRATEGIES[:3]):
            feedback = [self.evaluate(strategy, case) for case in self.cases["train"]]
            for key in ("parent_instructions", "instructions", "parent"):
                request = {
                    key: BENCH.INSTRUCTIONS[strategy],
                    "round": index + 1,
                    "feedback": [],
                }
                self.assertEqual(
                    BENCH.propose(request)["instructions"], BENCH.INSTRUCTIONS[strategy]
                )
                request["feedback"] = {
                    "train": [{"feedback": json.dumps(row)} for row in feedback]
                }
                self.assertEqual(
                    BENCH.propose(request)["instructions"],
                    BENCH.INSTRUCTIONS[BENCH.STRATEGIES[index + 1]],
                )

    def test_hidden_modes_are_real_subprocesses_and_reject_unknown_markers(self):
        case = self.cases["train"][0]
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--evaluate"],
            input=json.dumps(
                {"instructions": BENCH.INSTRUCTIONS["scan-all"], "case": case}
            ),
            text=True,
            capture_output=True,
            check=True,
            timeout=30,
        )
        response = json.loads(result.stdout)
        self.assertTrue(response["passed"])
        proposal = subprocess.run(
            [sys.executable, str(SCRIPT), "--propose"],
            input=json.dumps(
                {
                    "parent_instructions": BENCH.INSTRUCTIONS["scan-all"],
                    "round": 1,
                    "training_feedback": [dict(response, id=case["id"])],
                }
            ),
            text=True,
            capture_output=True,
            check=True,
            timeout=30,
        )
        self.assertEqual(
            json.loads(proposal.stdout)["instructions"],
            BENCH.INSTRUCTIONS["record-files"],
        )
        with self.assertRaisesRegex(ValueError, "unsupported instructions"):
            BENCH.interpret(
                "unknown-policy", Path(case["input"]["root"]), case["input"]["key"]
            )

    def test_missing_controller_fails_without_writing_report(self):
        output = self.root / "report.json"
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--out",
                str(output),
                "--controller",
                str(self.root / "missing.py"),
            ],
            text=True,
            capture_output=True,
            timeout=30,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("controller unavailable", result.stderr)
        self.assertFalse(output.exists())

    def test_existing_report_is_preserved(self):
        output = self.root / "report.json"
        output.write_text("existing user data")
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--out", str(output)],
            text=True,
            capture_output=True,
            timeout=30,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--out must be a new file", result.stderr)
        self.assertEqual(output.read_text(), "existing user data")

    def test_summary_counts_fixed_cases_and_rejects_changed_repeats(self):
        rows = [
            self.evaluate(strategy, case)
            for cases in self.cases.values()
            for case in cases
            for strategy in ("scan-all", "record-headers")
        ]
        self.assertEqual(
            BENCH.summarize(rows, "record-headers"),
            BENCH.summarize(rows + rows, "record-headers"),
        )
        with self.assertRaisesRegex(ValueError, "missing"):
            BENCH.summarize(rows[1:], "record-headers")
        with self.assertRaisesRegex(ValueError, "nondeterministic"):
            BENCH.summarize(rows + [dict(rows[0], cost=0)], "record-headers")

    @unittest.skipUnless(
        CONTROLLER.is_file(), "controller not merged; local fixture checks still run"
    )
    def test_real_controller_report_and_holdout_order(self):
        output = self.root / "report.json"
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--controller",
                str(CONTROLLER),
                "--out",
                str(output),
            ],
            text=True,
            capture_output=True,
            timeout=240,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(output.read_text())
        self.assertEqual(report["final_strategy"], "record-headers")
        self.assertEqual(report["fixture_sha256"], self.hashes)
        self.assertEqual(
            report["sources_sha256"]["controller"],
            BENCH.sha256(CONTROLLER.read_bytes()),
        )
        self.assertEqual(
            report["sources_sha256"]["benchmark"], BENCH.sha256(SCRIPT.read_bytes())
        )
        self.assertIn("NOT language-model", " ".join(report["limitations"]))
        artifacts = report["controller_artifacts"]
        self.assertTrue(artifacts)
        manifest = json.loads(artifacts["manifest.json"])["sha256"]
        for name, digest in manifest.items():
            self.assertEqual(BENCH.sha256(artifacts[name].encode()), digest, name)
        decisions = report["controller_report"]["rounds"]
        self.assertEqual([r["accepted"] for r in decisions], [True, True, False])
        self.assertIn("validation", decisions[-1]["reason"])
        self.assertEqual(report["controller_report"]["accepted_count"], 2)
        self.assertEqual(artifacts["final.md"], BENCH.INSTRUCTIONS["record-headers"])
        for index, decision in enumerate(decisions, 1):
            self.assertEqual(
                decision["parent_sha256"],
                BENCH.sha256(BENCH.INSTRUCTIONS[BENCH.STRATEGIES[index - 1]].encode()),
            )
        rows = report["evaluations"]
        holdout_start = next(
            i for i, row in enumerate(rows) if row["case_id"].startswith("test-")
        )
        self.assertTrue(
            all(row["case_id"].startswith("test-") for row in rows[holdout_start:])
        )
        self.assertEqual(len(rows[holdout_start:]), 6)
        self.assertEqual(
            {row["strategy"] for row in rows[holdout_start:]},
            {"scan-all", "record-headers"},
        )
        for strategy in BENCH.STRATEGIES:
            for split in ("train", "validation"):
                selected = [
                    r
                    for r in rows
                    if r["strategy"] == strategy
                    and r["case_id"].startswith(split + "-")
                ]
                self.assertEqual(
                    {r["case_id"] for r in selected},
                    {case["id"] for case in self.cases[split]},
                )
                self.assertEqual(
                    all(r["passed"] for r in selected),
                    strategy != "first-record" or split == "train",
                )
        self.assertEqual(report["summary"], BENCH.summarize(rows, "record-headers"))
        for split in report["summary"].values():
            self.assertEqual(split["baseline"]["passed"], 3)
            self.assertEqual(split["final"]["passed"], 3)
            self.assertGreater(split["reduction_bytes"], 0)
            self.assertGreater(split["reduction_fraction"], 0)
        # A second fresh run must reproduce workload data, not environment paths.
        again = BENCH.run(CONTROLLER)
        for field in ("fixture_sha256", "summary", "evaluations", "final_strategy"):
            self.assertEqual(report[field], again[field], field)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
