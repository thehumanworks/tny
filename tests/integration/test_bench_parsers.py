#!/usr/bin/env python3
"""Deterministic parser benchmark oracles; performance is measured separately."""

import copy
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    "bench_parsers", Path(__file__).resolve().parents[1] / "bench" / "bench_parsers.py"
)
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


def measurement(**changes):
    result = {
        "mode": "sse",
        "fragmentation": "byte",
        "iterations": 20,
        "input_bytes": 1024,
        "events": 660,
        "checksum": "oracle",
        "nanoseconds": 1000,
        "peak_rss_bytes": 1000,
        "allocations": 20,
    }
    result.update(changes)
    return result


class ParserBenchmarkTests(unittest.TestCase):
    def test_unchanged_behavior_and_bounded_regression_pass(self):
        result = BENCH.compare(
            [measurement()], [measurement(nanoseconds=1100, peak_rss_bytes=1100)]
        )
        self.assertTrue(result["passed"])
        self.assertEqual(result["maximum_ratio"], 1.10)

    def test_either_time_or_memory_regression_fails(self):
        for key in ("nanoseconds", "peak_rss_bytes"):
            with self.subTest(metric=key):
                self.assertFalse(
                    BENCH.compare([measurement()], [measurement(**{key: 1101})])[
                        "passed"
                    ]
                )

    def test_semantic_differences_cannot_be_reported_as_faster(self):
        for key, value in (
            ("checksum", "wrong"),
            ("events", 659),
            ("input_bytes", 1023),
            ("iterations", 19),
            ("fragmentation", "whole"),
        ):
            with self.subTest(field=key), self.assertRaises(ValueError):
                BENCH.compare(
                    [measurement()], [measurement(**{key: value, "nanoseconds": 1})]
                )

    def test_empty_and_invalid_measurements_are_rejected(self):
        for a, b in (
            ([], []),
            ([measurement()], []),
            ([measurement()], [measurement(), measurement()]),
        ):
            with self.assertRaises(ValueError):
                BENCH.compare(a, b)
        for key in (
            "nanoseconds",
            "peak_rss_bytes",
            "input_bytes",
            "events",
            "iterations",
        ):
            for value in (0, -1, "1", float("nan")):
                with (
                    self.subTest(field=key, value=value),
                    self.assertRaises(ValueError),
                ):
                    BENCH.compare([measurement()], [measurement(**{key: value})])
        with self.assertRaises(ValueError):
            BENCH.compare([measurement()], [measurement(allocations=-1)])

    def test_medians_use_all_batches_and_keep_allocation_counts(self):
        samples = [
            measurement(nanoseconds=n, allocations=i)
            for i, n in enumerate((1000, 10000, 900))
        ]
        expected = copy.deepcopy(samples)
        result = BENCH.compare(samples, copy.deepcopy(samples))
        self.assertEqual(result["baseline"]["median_nanoseconds"], 1000)
        self.assertEqual(result["baseline"]["allocations"], [0, 1, 2])
        self.assertEqual(samples, expected)

    def test_source_selection_rejects_missing_or_ambiguous_migration(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with self.assertRaises(ValueError):
                BENCH.source_file(root, "parser")
            (root / "parser.c").write_text("/* C */\n")
            self.assertEqual(BENCH.source_file(root, "parser"), root / "parser.c")
            (root / "parser.cpp").write_text("/* C++ */\n")
            with self.assertRaises(ValueError):
                BENCH.source_file(root, "parser")
            (root / "parser.c").unlink()
            self.assertEqual(BENCH.source_file(root, "parser"), root / "parser.cpp")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
