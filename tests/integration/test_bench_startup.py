#!/usr/bin/env python3
"""Deterministic oracles for the startup gate, without timing assertions in CI."""

import importlib.util
import os
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "bench_startup", Path(__file__).resolve().parents[1] / "bench" / "bench_startup.py"
)
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


class StartupContractTests(unittest.TestCase):
    def test_batches_preserve_all_requested_samples(self):
        self.assertEqual(BENCH.batch_sizes(100, 3), [34, 33, 33])
        self.assertEqual(BENCH.batch_sizes(102, 3), [34, 34, 34])
        self.assertEqual(BENCH.batch_sizes(20, 3), [7, 7, 6])
        with self.assertRaises(ValueError):
            BENCH.batch_sizes(2, 3)

    def test_help_and_version_enforce_absolute_and_added_latency(self):
        for mode in ("version", "help"):
            self.assertTrue(BENCH.compare(mode, [1.0], [1.25])["passed"])
            self.assertFalse(BENCH.compare(mode, [1.0], [1.251])["passed"])
            self.assertTrue(BENCH.compare(mode, [3.0], [3.29])["passed"])
            self.assertFalse(BENCH.compare(mode, [3.0], [3.31])["passed"])
            self.assertFalse(BENCH.compare(mode, [4.9], [5.0])["passed"])

    def test_first_prompt_uses_its_own_thresholds(self):
        self.assertTrue(BENCH.compare("first-prompt", [2.0], [2.5])["passed"])
        self.assertFalse(BENCH.compare("first-prompt", [2.0], [2.501])["passed"])
        self.assertTrue(BENCH.compare("first-prompt", [7.0], [7.69])["passed"])
        self.assertFalse(BENCH.compare("first-prompt", [9.9], [10.0])["passed"])

    def test_invalid_samples_never_pass(self):
        for values in ([], [-1], [float("inf")], [float("nan")]):
            with self.assertRaises(ValueError):
                BENCH.summary(values)
        with self.assertRaises(ValueError):
            BENCH.compare("ttft", [1], [1])

    def test_summary_retains_tail_latency(self):
        result = BENCH.summary(list(range(1, 101)))
        self.assertEqual(result["median_ms"], 50.5)
        self.assertEqual(result["p95_ms"], 95)
        self.assertEqual(result["count"], 100)

    def test_marker_requires_full_composer_paint_not_banner_or_raw_mode(self):
        prefix = b"\x1b[?2004h/help for commands"
        self.assertFalse(BENCH.prompt_ready(prefix))
        self.assertFalse(BENCH.prompt_ready(prefix + BENCH.PROMPT_END))
        self.assertFalse(BENCH.prompt_ready(prefix + b"\x1b[?7l> "))
        self.assertTrue(
            BENCH.prompt_ready(prefix + b"\x1b[?7lstatus\r\n> " + BENCH.PROMPT_END)
        )
        self.assertFalse(BENCH.prompt_ready(b"> " + BENCH.PROMPT_END + prefix))

    def test_environment_does_not_inherit_credentials_or_live_state(self):
        with patch.dict(
            os.environ,
            {
                "LIVE_API_KEY": "do-not-copy",
                "TNY_HOME": "/live",
                "LD_PRELOAD": "/injection",
            },
        ):
            env = BENCH.isolated_env(Path("/fixture"))
        self.assertNotIn("LIVE_API_KEY", env)
        self.assertNotIn("TNY_HOME", env)
        self.assertNotIn("LD_PRELOAD", env)
        self.assertEqual(env["HOME"], "/fixture")
        self.assertEqual(env["TNY_ISOLATE"], "0")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
