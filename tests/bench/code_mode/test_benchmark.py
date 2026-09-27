"""Offline guards for the benchmark, cohort, effect oracle, and Lean translator."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import itertools
import json
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from cases import LANGUAGES, REPEATS, TASKS, VARIANTS, fixture
from execute import score, strict_json, typed_equal
from policy import accept, promote
from run import generation_valid

ROOT = HERE.parents[2]
spec = importlib.util.spec_from_file_location(
    "lean_check", ROOT / "tests/formal/check_code_mode_language.py"
)
lean_check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lean_check)


class PolicyTests(unittest.TestCase):
    def test_acceptance_truth_table(self):
        for flags in itertools.product((False, True), repeat=3):
            self.assertEqual(accept(*flags), flags == (True, True, True))

    def test_promotion_boundaries(self):
        for flags in itertools.product((False, True), repeat=3):
            for counts in itertools.product((-1, 0, 1, 36), repeat=4):
                a, b, c, d = counts
                self.assertEqual(
                    promote(*flags, *counts), all(flags) and a >= b and c >= d
                )

    def test_exact_source_translation(self):
        generated = lean_check.translate(lean_check.POLICY.read_text())
        self.assertIn("decide (first_candidate ≥ first_baseline)", generated)
        self.assertIn("execution_ok && output_ok && trace_ok", generated)

    def test_translator_rejects_unsupported_syntax(self):
        source = lean_check.POLICY.read_text()
        for mutant in (
            source.replace("execution_ok and output_ok", "execution_ok or output_ok"),
            source.replace("first_candidate >=", "first_candidate >"),
            source + "\nprint('side effect')\n",
            source.replace("return execution_ok", "pass\n    return execution_ok"),
        ):
            with self.assertRaises(ValueError):
                lean_check.translate(mutant)


class OracleTests(unittest.TestCase):
    def test_strict_types_and_shapes(self):
        for a, b in (
            (False, 0),
            (True, 1),
            (None, False),
            ([], {}),
            ("1", 1),
            ([1], {"1": 1}),
        ):
            self.assertFalse(typed_equal(a, b))
        self.assertTrue(
            typed_equal({"a": [False, None, {}, []]}, {"a": [False, None, {}, []]})
        )
        self.assertTrue(typed_equal(1, 1.0))
        self.assertFalse(typed_equal({"a": 1}, {"b": 1}))

    def test_nonfinite_and_duplicate_keys_are_not_json_evidence(self):
        for text in ('{"a":1,"a":2}', "NaN", "Infinity", "-Infinity", '{"a":NaN}'):
            with self.assertRaises(ValueError):
                strict_json(text)

    def test_all_fixtures_have_independent_oracles(self):
        self.assertEqual(len(LANGUAGES) * REPEATS * len(TASKS), 108)
        for task, _ in TASKS:
            for variant in range(VARIANTS):
                case = fixture(task, variant)
                self.assertIn("output.json", case["expected"])
                self.assertNotIn("expected", case["runtime"])
                self.assertTrue(case["required_calls"])

    def test_every_scoring_boundary_fails_closed(self):
        case = fixture("filter", 0)
        valid = {
            "runtime_ok": True,
            "invalid_call": False,
            "stdout": "done\n",
            "calls": case["required_calls"],
            "writes": {"output.json": json.dumps(case["expected"]["output.json"])},
        }
        self.assertTrue(score(valid, case)["passed"])
        for update in (
            {"runtime_ok": False},
            {"runtime_ok": 1},
            {"invalid_call": True},
            {"stdout": "done"},
            {"stdout": "I succeeded\n"},
            {"calls": []},
            {"writes": {}},
            {"writes": {"output.json": '["not-a-fixture-id"]'}},
            {"writes": {**valid["writes"], "extra.txt": "unexpected effect"}},
            {"calls": case["required_calls"] * 65},
        ):
            with self.subTest(update=update):
                self.assertFalse(score({**valid, **update}, case)["passed"])

    def test_null_and_empty_shape_not_interchangeable(self):
        case = fixture("json_shapes", 0)
        expected = case["expected"]["output.json"]
        result = {
            "runtime_ok": True,
            "invalid_call": False,
            "stdout": "done\n",
            "calls": case["required_calls"],
            "writes": {"output.json": json.dumps(expected)},
        }
        self.assertTrue(score(result, case)["passed"])
        for key, value in (
            ("items", {}),
            ("empty", []),
            ("enabled", 0),
            ("count", False),
            ("optional", False),
        ):
            result["writes"]["output.json"] = json.dumps({**expected, key: value})
            self.assertFalse(score(result, case)["passed"])


class GenerationTests(unittest.TestCase):
    def setUp(self):
        self.events = [
            {"type": "item.completed", "item": {"type": "agent_message"}},
            {
                "type": "turn.completed",
                "usage": {
                    "input_tokens": 100,
                    "cached_input_tokens": 0,
                    "output_tokens": 12,
                },
            },
        ]
        self.answer = {"code": 'print("done")'}

    def test_requires_successful_complete_no_tool_generation(self):
        self.assertTrue(generation_valid(self.events, 0, self.answer))
        self.assertFalse(generation_valid(self.events, 1, self.answer))
        self.assertFalse(generation_valid(self.events * 2, 0, self.answer))
        self.assertFalse(generation_valid([], 0, self.answer))
        self.assertFalse(generation_valid(self.events, 0, {"code": ""}))
        self.assertFalse(generation_valid(self.events, 0, {"code": "x", "extra": True}))
        for item in ("command_execution", "mcp_tool_call", "file_change"):
            extra = {"type": "item.completed", "item": {"type": item}}
            self.assertFalse(generation_valid(self.events + [extra], 0, self.answer))

    def test_missing_or_invalid_usage_is_not_zero(self):
        for usage in (
            {},
            {"input_tokens": 1, "cached_input_tokens": 0, "output_tokens": -1},
            {"input_tokens": True, "cached_input_tokens": 0, "output_tokens": 1},
        ):
            self.assertFalse(
                generation_valid(
                    [{"type": "turn.completed", "usage": usage}], 0, self.answer
                )
            )


class EvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = ROOT / "docs/verification/code-mode-language/data"
        cls.rows = json.loads((cls.directory / "samples.json").read_text())

    def test_complete_cohort_and_report_reconcile(self):
        from analyze import summarize, validate_cohort

        validate_cohort(self.rows)
        report = json.loads((self.directory / "analysis.json").read_text())
        for language in LANGUAGES:
            observed = summarize([r for r in self.rows if r["language"] == language])
            self.assertEqual(observed, report["languages"][language])

    def test_incomplete_duplicate_wrong_model_and_attempts_rejected(self):
        from analyze import validate_cohort

        with self.assertRaises(ValueError):
            validate_cohort(self.rows[:-1])
        with self.assertRaises(ValueError):
            validate_cohort(self.rows[:-1] + [self.rows[0]])
        for mutation in (
            "model",
            "extra_attempt",
            "missing_variant",
            "numeric_success",
            "missing_usage",
        ):
            rows = copy.deepcopy(self.rows)
            first = rows[0]
            attempt = first["attempts"][0]
            if mutation == "model":
                attempt["generation"]["requested_model"] = "another-model"
            elif mutation == "extra_attempt":
                first["attempts"] = [attempt, attempt, attempt]
            elif mutation == "missing_variant":
                attempt["evaluation"]["variants"].pop()
            elif mutation == "numeric_success":
                first["passed"] = 1
            else:
                del attempt["generation"]["usage"]["output_tokens"]
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                validate_cohort(rows)

    def test_intervals_and_retention_inputs_recompute(self):
        from analyze import paired_comparison

        report = json.loads((self.directory / "analysis.json").read_text())
        decision = json.loads((self.directory / "decision.json").read_text())
        for language in ("javascript", "python"):
            comparison = paired_comparison(self.rows, language)
            self.assertEqual(comparison, report["comparisons"][language])
            candidate = next(
                c for c in decision["candidates"] if c["language"] == language
            )
            self.assertEqual(
                candidate["confirmed_gain"],
                comparison["supported_gain_under_preregistered_threshold"],
            )
            self.assertEqual(
                candidate["first_candidate"],
                report["languages"][language]["first_pass"],
            )
            self.assertEqual(
                candidate["first_baseline"], report["languages"]["lua"]["first_pass"]
            )
            self.assertFalse(
                promote(
                    *(
                        candidate[k]
                        for k in (
                            "complete",
                            "confirmed_gain",
                            "parity",
                            "first_candidate",
                            "first_baseline",
                            "final_candidate",
                            "final_baseline",
                        )
                    )
                )
            )

    def test_exported_evidence_hashes(self):
        manifest = json.loads((self.directory / "SHA256.json").read_text())
        for name, expected in manifest.items():
            content = (self.directory / name).read_bytes()
            self.assertEqual(len(content), expected["bytes"])
            self.assertEqual(hashlib.sha256(content).hexdigest(), expected["sha256"])


if __name__ == "__main__":
    unittest.main()
