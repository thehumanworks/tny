"""Offline accounting guards; never runs inference."""

from __future__ import annotations

import unittest

from analyze_eval import request_valid, summary


class Accounting(unittest.TestCase):
    def setUp(self):
        self.row = {
            "completion": "response.completed",
            "http_status": 200,
            "requested_model": "gpt-6-luna",
            "reported_model": "gpt-6-luna",
            "requested_effort": "low",
            "input_tokens": 100,
            "cached_input_tokens": 80,
            "cache_write_tokens": 0,
            "output_tokens": 10,
            "reasoning_tokens": 0,
        }

    def test_actual_model_and_effort(self):
        self.assertTrue(request_valid(self.row))
        for key in ("requested_model", "reported_model", "requested_effort"):
            self.assertFalse(request_valid({**self.row, key: "different"}))

    def test_unknown_usage_is_not_zero(self):
        for key in (
            "input_tokens",
            "cached_input_tokens",
            "cache_write_tokens",
            "output_tokens",
            "reasoning_tokens",
        ):
            for value in (None, -1, True, "0"):
                self.assertFalse(request_valid({**self.row, key: value}))

    def test_cache_is_bounded_by_input(self):
        self.assertFalse(request_valid({**self.row, "cached_input_tokens": 101}))
        self.assertFalse(request_valid({**self.row, "cache_write_tokens": 101}))

    def test_only_complete_responses_count(self):
        for value in ("response.failed", "response.incomplete", None):
            self.assertFalse(request_valid({**self.row, "completion": value}))
        self.assertFalse(request_valid({**self.row, "http_status": 429}))

    def test_weighted_cache_and_failure_denominators(self):
        base = {
            **self.row,
            "harness": "tny",
            "pass": True,
            "measurement_valid": True,
            "status": "pass",
            "noncached_input_tokens": 20,
            "model_tool_calls": 2,
            "requests": 3,
            "unique_tool_output_bytes": 30,
            "wall_s": 4,
            "first_request_input": 100,
            "first_request_cached": 80,
            "later_input": 0,
            "later_cached": 0,
        }
        fail = {
            **base,
            "pass": False,
            "status": "fail",
            "input_tokens": 900,
            "cached_input_tokens": 90,
            "noncached_input_tokens": 810,
        }
        result = summary([base, fail])
        self.assertEqual(result["cached_input_fraction"], 170 / 1000)
        self.assertEqual(result["passes"], 1)
        self.assertEqual(result["output_tokens_per_pass"], 20)
        self.assertEqual(result["input_tokens_per_pass"], 1000)
        self.assertEqual(result["model_tool_calls_per_pass"], 4)


if __name__ == "__main__":
    unittest.main()
