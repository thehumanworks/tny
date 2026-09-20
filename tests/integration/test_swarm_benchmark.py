#!/usr/bin/env python3
"""Offline checks for live benchmark oracles and multi-agent usage accounting."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench"))
import bench_swarm as bench  # noqa: E402
import swarm_cases as cases  # noqa: E402


class BenchmarkTests(unittest.TestCase):
    def test_stub_solutions_fail_external_oracles(self):
        with tempfile.TemporaryDirectory() as tmp:
            for case in cases.CASES:
                workspace = Path(tmp) / case.name
                cases.prepare(case, workspace)
                self.assertFalse(cases.grade(case.name, workspace)["passed"])

    def test_reference_completion_retry_and_skip_order(self):
        tasks = [
            {"id": "a", "duration": 1, "failures": 2},
            {"id": "b", "duration": 2},
            {"id": "c", "duration": 1, "depends_on": ["a"]},
            {"id": "d", "duration": 1, "depends_on": ["c"]},
        ]
        result = cases.reference_schedule(tasks, workers=2, retries=1)
        self.assertEqual(result["makespan"], 2)
        self.assertEqual(
            result["tasks"]["a"], {"state": "failed", "attempts": 2, "finish": 2}
        )
        self.assertEqual(
            result["tasks"]["d"], {"state": "skipped", "attempts": 0, "finish": 2}
        )
        self.assertEqual(
            [(e["task"], e["type"]) for e in result["events"] if e["time"] == 2],
            [("a", "failed"), ("b", "succeeded"), ("c", "skipped"), ("d", "skipped")],
        )

    def test_tny_sums_parent_and_children_once_and_marks_unknown_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for index, usage in enumerate(
                [
                    {
                        "in": 100,
                        "out": 5,
                        "cached_in": 60,
                        "requests": 2,
                        "cache_read_requests": 2,
                    },
                    {"in": 40, "out": 8, "requests": 1},
                ]
            ):
                directory = root / ".tny/sessions" / str(index)
                directory.mkdir(parents=True)
                (directory / "session.json").write_text(json.dumps({"usage": usage}))
            job = root / ".tny/jobs/run"
            job.mkdir(parents=True)
            (job / "job.json").write_text(
                json.dumps(
                    {
                        "state": "succeeded",
                        "items": [
                            {"state": "succeeded", "session_id": "child"},
                            {"state": "skipped"},
                        ],
                    }
                )
            )
            lead = root / "events.jsonl"
            lead.write_text(json.dumps({"type": "usage", "input_tokens": 100}) + "\n")
            report = bench.tny_metrics(root, lead)
            self.assertEqual(report["input_tokens"], 140)
            self.assertEqual(report["output_tokens"], 13)
            self.assertEqual(report["launched_collaborators"], 1)
            self.assertTrue(report["usage_complete"])
            self.assertFalse(report["cache_coverage_complete"])

    def test_codex_uses_last_cumulative_usage_in_each_session(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "sessions").mkdir()
            for name, totals in (("parent", (10, 30)), ("child", (5, 9))):
                lines = [
                    {"type": "session_meta", "payload": {"id": name}},
                    {"type": "turn_context", "payload": {"model": "gpt-5.6-sol"}},
                ]
                lines += [
                    {
                        "type": "event_msg",
                        "payload": {
                            "type": "token_count",
                            "info": {
                                "total_token_usage": {
                                    "input_tokens": total,
                                    "output_tokens": 2,
                                    "cached_input_tokens": 4,
                                }
                            },
                        },
                    }
                    for total in totals
                ]
                (root / "sessions" / f"{name}.jsonl").write_text(
                    "\n".join(map(json.dumps, lines))
                )
            report = bench.codex_metrics(root, root / "missing-lead")
            self.assertEqual(report["input_tokens"], 39)
            self.assertEqual(report["output_tokens"], 4)
            self.assertEqual(report["session_count"], 2)
            self.assertTrue(report["usage_complete"])
            self.assertTrue(
                all(item["models"] == ["gpt-5.6-sol"] for item in report["sessions"])
            )

    def test_codex_root_only_fallback_is_not_complete_swarm_usage(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lead = root / "events.jsonl"
            lead.write_text(
                json.dumps(
                    {
                        "type": "turn.completed",
                        "usage": {
                            "input_tokens": 10,
                            "output_tokens": 2,
                            "cached_input_tokens": 4,
                        },
                    }
                )
            )
            report = bench.codex_metrics(root, lead)
            self.assertTrue(report["rollout_usage_fallback"])
            self.assertFalse(report["usage_complete"])


if __name__ == "__main__":
    # The aggregate integration runner supplies the tny binary as argv[1].
    unittest.main(argv=[sys.argv[0]])
