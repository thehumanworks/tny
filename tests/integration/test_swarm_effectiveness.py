#!/usr/bin/env python3
"""Offline integrity checks for the paired swarm-effectiveness experiment."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench"))
import bench_swarm_effectiveness as experiment  # noqa: E402


class EffectivenessTests(unittest.TestCase):
    def test_order_is_counterbalanced_without_dropping_conditions(self):
        self.assertEqual(experiment.trial_order(0, 1), ("baseline", "candidate"))
        self.assertEqual(experiment.trial_order(1, 1), ("candidate", "baseline"))
        self.assertEqual(experiment.trial_order(0, 2), ("candidate", "baseline"))

    def test_incomplete_usage_and_failed_trials_are_not_zero_cost_success(self):
        rows = [
            {
                "case": "ledger",
                "condition": "candidate",
                "passed": True,
                "end_to_end_seconds": 10,
                "metrics": {
                    "usage_complete": True,
                    "cache_coverage_complete": True,
                    "input_tokens": 100,
                    "output_tokens": 8,
                    "cached_input_tokens": 60,
                    "launched_collaborators": 3,
                },
            },
            {
                "case": "ledger",
                "condition": "candidate",
                "passed": False,
                "end_to_end_seconds": 30,
                "metrics": {
                    "usage_complete": False,
                    "cache_coverage_complete": False,
                    "input_tokens": 12,
                    "output_tokens": 2,
                    "cached_input_tokens": 0,
                    "launched_collaborators": 2,
                },
            },
            {
                "case": "ledger",
                "condition": "candidate",
                "passed": False,
                "infrastructure_error": "TimeoutExpired",
            },
        ]
        result = experiment.summarize(rows)["ledger"]["candidate"]
        self.assertEqual(result["attempted_runs"], 3)
        self.assertEqual(result["accepted_runs"], 1)
        self.assertEqual(result["infrastructure_failures"], 1)
        self.assertEqual(result["median_seconds_all_observed"], 20)
        self.assertEqual(result["median_seconds_accepted"], 10)
        self.assertIsNone(result["median_input_tokens"])
        self.assertIsNone(result["median_cached_input_tokens"])
        self.assertFalse(result["usage_complete"])

    def test_protocol_metrics_keep_tool_failures_and_do_not_treat_plaintext_as_evidence(
        self,
    ):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp)
            session = home / ".tny/sessions/root/session.json"
            session.parent.mkdir(parents=True)
            session.write_text(
                json.dumps(
                    {
                        "messages": [
                            {
                                "tool_calls": [
                                    {"function": {"name": "swarm_message"}},
                                    {"function": {"name": "team_mailbox"}},
                                ]
                            }
                        ]
                    }
                )
            )
            job = home / ".tny/jobs/job"
            job.mkdir(parents=True)
            (job / "mailbox.json").write_text(
                json.dumps(
                    {
                        "messages": [
                            {"text": "I claim success"},
                            {
                                "text": json.dumps(
                                    {
                                        "version": 1,
                                        "kind": "finding",
                                        "topic": "rollback",
                                        "body": "evidence",
                                    }
                                )
                            },
                            {
                                "text": json.dumps(
                                    {
                                        "version": 1,
                                        "kind": "challenge",
                                        "topic": "rollback",
                                        "body": "counterexample",
                                    }
                                )
                            },
                        ]
                    }
                )
            )
            (job / "attempt-1-item-0.log").write_text(
                json.dumps(
                    {"type": "tool_end", "tool_name": "swarm_message", "tool_ok": False}
                )
            )
            result = experiment.evidence_metrics(home, home / "missing")
            self.assertEqual(
                result["tool_attempts_by_name"], {"swarm_message": 1, "team_mailbox": 1}
            )
            self.assertEqual(result["tool_errors_by_name"], {"swarm_message": 1})
            self.assertEqual(result["distinct_typed_topics"], 1)
            self.assertEqual(
                result["typed_message_kinds"], {"finding": 1, "challenge": 1}
            )
            self.assertNotIn("accepted", result)

    def test_infrastructure_failure_is_saved_and_aborts_before_another_trial(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary, definition, auth = (
                root / "binary",
                root / "definition",
                root / "auth.json",
            )
            binary.write_bytes(b"synthetic binary; never executed")
            definition.write_text("synthetic definition; validator mocked")
            auth.write_text("{}")
            output = root / "result"
            argv = [
                "bench",
                "--live",
                "--baseline",
                str(binary),
                "--candidate",
                str(binary),
                "--baseline-definition",
                str(definition),
                "--candidate-definition",
                str(definition),
                "--codex",
                str(binary),
                "--codex-home",
                str(root),
                "--cases",
                "slug",
                "ledger",
                "--output",
                str(output),
            ]
            checked = subprocess.CompletedProcess([], 0, stdout='{"participants":3}')
            with (
                mock.patch.object(sys, "argv", argv),
                mock.patch.object(
                    experiment.platform, "platform", return_value="synthetic-host"
                ),
                mock.patch.object(experiment.subprocess, "run", return_value=checked),
                mock.patch.object(
                    experiment.subprocess,
                    "check_output",
                    return_value="fixture-version",
                ),
                mock.patch.object(
                    experiment.bench,
                    "run_trial",
                    side_effect=OSError("private diagnostics"),
                ) as trial,
            ):
                with self.assertRaises(SystemExit) as stopped:
                    experiment.main()
            self.assertEqual(stopped.exception.code, 2)
            self.assertEqual(trial.call_count, 1)
            result = json.loads((output / "result.json").read_text())
            self.assertIn("aborted_utc", result)
            self.assertNotIn("completed_utc", result)
            self.assertEqual(len(result["runs"]), 1)
            self.assertEqual(result["runs"][0]["infrastructure_error"], "OSError")
            self.assertNotIn("private diagnostics", json.dumps(result))

    def test_live_flag_is_required_before_any_files_or_account_access(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "must-not-exist"
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tests/bench/bench_swarm_effectiveness.py"),
                    "--baseline",
                    "/nonexistent",
                    "--candidate",
                    "/nonexistent",
                    "--baseline-definition",
                    "/nonexistent",
                    "--candidate-definition",
                    "/nonexistent",
                    "--output",
                    str(output),
                ],
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("requires explicit --live", result.stderr)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
