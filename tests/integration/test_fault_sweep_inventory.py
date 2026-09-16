#!/usr/bin/env python3
"""Prove allocation coverage cannot silently shrink when scheduling changes."""

import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import test_libtny_faults as faults


class AllocationSweepCoverage(unittest.TestCase):
    def test_public_scope_covers_highest_discovery_not_common_prefix(self):
        discoveries = iter((4, 7, 5, 6))
        injected = []

        def measured(*args):
            if len(args) == 5:
                return next(discoveries)
            injected.append(args[5])
            return args[5]

        with patch.object(faults, "run_measured", side_effect=measured):
            count = faults.sweep("script", "library", "scenario", "scope", "url")
        self.assertEqual(count, 7)
        self.assertEqual(injected, list(range(1, 8)))

    def test_public_scope_never_retries_a_failing_injected_run(self):
        injected = []

        def measured(*args):
            if len(args) == 5:
                return 4
            injected.append(args[5])
            raise AssertionError("broken settlement")

        with patch.object(faults, "run_measured", side_effect=measured):
            with self.assertRaisesRegex(AssertionError, "broken settlement"):
                faults.sweep("script", "library", "scenario", "scope", "url")
        self.assertEqual(injected, [1])

    def test_turn_covers_highest_of_all_discoveries(self):
        discoveries = iter((2, 5, 3))
        injected = []

        def run(index):
            if index == 0:
                return next(discoveries), False
            injected.append(index)
            return index, True

        self.assertEqual(faults.sweep_turn_allocations(run, "fixture"), 5)
        self.assertEqual(injected, [1, 2, 3, 4, 5])

    def test_turn_retains_discovered_tail_even_if_later_runs_are_shorter(self):
        discoveries = iter((2, 5, 3))
        attempts = []

        def run(index):
            if index == 0:
                return next(discoveries), False
            attempts.append(index)
            return 3, index <= 3

        with self.assertRaisesRegex(AssertionError, "never injected"):
            faults.sweep_turn_allocations(run, "fixture")
        self.assertEqual(attempts, [1, 2, 3] + [4] * faults.MISSED_INJECTION_ATTEMPTS)

    def test_turn_retries_only_missed_injection_and_expands_high_water_mark(self):
        attempts = []

        def run(index):
            if index == 0:
                return 2, False
            attempts.append(index)
            if index == 1 and len(attempts) <= 32:
                return 4, False
            return index, True

        self.assertEqual(faults.sweep_turn_allocations(run, "fixture"), 4)
        self.assertEqual(attempts, [1] * 33 + [2, 3, 4])

    def test_turn_failure_is_never_retried(self):
        attempts = []

        def run(index):
            if index == 0:
                return 2, False
            attempts.append(index)
            raise AssertionError("use after free")

        with self.assertRaisesRegex(AssertionError, "use after free"):
            faults.sweep_turn_allocations(run, "fixture")
        self.assertEqual(attempts, [1])

    def test_empty_scope_requires_no_fabricated_injection(self):
        indices = []

        def run(index):
            indices.append(index)
            return 0, False

        self.assertEqual(faults.sweep_turn_allocations(run, "fixture"), 0)
        self.assertEqual(indices, [0, 0, 0])


class SanitizerChildRuntime(unittest.TestCase):
    def check_runtime(self, platform, environment):
        completed = SimpleNamespace(returncode=0, stdout=b"", stderr=b"")
        with (
            patch.object(faults.sys, "platform", platform),
            patch.object(faults.subprocess, "run", return_value=completed) as run,
        ):
            faults.run_child("fixture.py", [], environment)
        return run.call_args.kwargs["env"]

    def test_linux_loads_asan_before_the_matching_cpp_exception_runtime(self):
        environment = self.check_runtime(
            "linux",
            {
                "TNY_TEST_ASAN_RUNTIME": "/toolchain/libasan.so",
                "TNY_TEST_CXX_RUNTIME": "/toolchain/libstdc++.so",
            },
        )
        self.assertEqual(
            environment["LD_PRELOAD"],
            "/toolchain/libasan.so:/toolchain/libstdc++.so",
        )
        self.assertNotIn("DYLD_INSERT_LIBRARIES", environment)

    def test_darwin_keeps_its_existing_single_runtime_loader(self):
        environment = self.check_runtime(
            "darwin",
            {
                "TNY_TEST_ASAN_RUNTIME": "/toolchain/libasan.dylib",
                "TNY_TEST_CXX_RUNTIME": "/unused/linux-runtime.so",
            },
        )
        self.assertEqual(
            environment["DYLD_INSERT_LIBRARIES"], "/toolchain/libasan.dylib"
        )
        self.assertNotIn("LD_PRELOAD", environment)

    def test_plain_child_never_injects_sanitizer_or_cpp_runtime(self):
        environment = self.check_runtime("linux", {"TNY_TEST_CXX_RUNTIME": "/unused"})
        self.assertNotIn("LD_PRELOAD", environment)
        self.assertNotIn("DYLD_INSERT_LIBRARIES", environment)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
