"""Exercise the actual preregistered gate with no prover or model account.

These are finite regression checks, not an all-integer proof. Each dimension
of the decision table has independently labelled accept/reject examples;
Fraction provides an exact ratio oracle without copying cross-multiplication.
The preregistered policy and recorded evidence remain byte-for-byte unchanged.
"""

from __future__ import annotations

import itertools
import json
import unittest
from fractions import Fraction
from pathlib import Path

from policy import select_lighter

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BASE = {
    "corpus_passed": 36,
    "corpus_total": 36,
    "trials_complete": True,
    "first_lighter": 35,
    "first_cpython": 35,
    "final_lighter": 36,
    "final_cpython": 35,
    "tokens_lighter": 9000,
    "solved_lighter": 36,
    "tokens_cpython": 8750,
    "solved_cpython": 35,
    "semantics_ok": True,
    "bytes_lighter": 4000000,
    "bytes_cpython": 4668312,
}


def decision_cases():
    """All combinations of the labelled Boolean and comparison partitions."""
    dimensions = (
        tuple(
            ({"corpus_passed": passed, "corpus_total": total}, expected)
            for passed, total, expected in (
                (-1, -1, False),
                (0, 0, False),
                (1, 0, False),
                (35, 36, False),
                (36, 36, True),
                (37, 36, False),
            )
        ),
        tuple(({"trials_complete": flag}, flag) for flag in (False, True)),
        tuple(
            ({"first_lighter": value}, expected)
            for value, expected in ((34, False), (35, True), (36, True))
        ),
        tuple(
            ({"final_lighter": value}, expected)
            for value, expected in ((34, False), (35, True), (36, True))
        ),
        tuple(
            (
                dict(
                    zip(
                        (
                            "tokens_lighter",
                            "solved_lighter",
                            "tokens_cpython",
                            "solved_cpython",
                        ),
                        values,
                    )
                ),
                expected,
            )
            for values, expected in (
                ((1, 1, 2, 2), True),  # equal cost per solved task
                ((1, 1, 3, 2), True),  # cheaper
                ((2, 1, 3, 2), False),  # more expensive
                ((2, 3, 1, 2), False),  # floor division would hide this regression
                ((0, 0, 0, 1), False),
                ((0, 1, 0, 0), False),
                ((0, 0, 0, 0), False),
                ((0, -1, 0, 1), False),
                ((0, 1, 0, -1), False),
            )
        ),
        tuple(({"semantics_ok": flag}, flag) for flag in (False, True)),
        tuple(
            ({"bytes_lighter": value}, expected)
            for value, expected in ((4668311, True), (4668312, False), (4668313, False))
        ),
    )
    for parts in itertools.product(*dimensions):
        inputs = dict(BASE)
        for update, _ in parts:
            inputs.update(update)
        yield inputs, all(expected for _, expected in parts)


class SelectionTests(unittest.TestCase):
    def check_decisions(self, select):
        self.assertIs(select(**BASE), True)  # A gate that always rejects must fail.
        for inputs, expected in decision_cases():
            self.assertIs(select(**inputs), expected, inputs)

    def check_integer_boundaries(self, select):
        # Python ints are unbounded. Preserve adjacent values beyond binary64
        # and uint64, including strict size ordering and exact equality.
        for base in (2**53, 2**64, 10**100):
            for delta in (-1, 0, 1):
                for candidate, baseline, expected in (
                    ("first_lighter", "first_cpython", delta >= 0),
                    ("final_lighter", "final_cpython", delta >= 0),
                    ("bytes_lighter", "bytes_cpython", delta < 0),
                ):
                    inputs = {**BASE, candidate: base + delta, baseline: base}
                    self.assertIs(select(**inputs), expected, inputs)

    def check_ratios(self, select):
        for tl, sl, tc, sc in itertools.product(
            (-1, 0, 1, 2, 2**53 + 1, 10**100), repeat=4
        ):
            inputs = {
                **BASE,
                "tokens_lighter": tl,
                "solved_lighter": sl,
                "tokens_cpython": tc,
                "solved_cpython": sc,
            }
            expected = sl > 0 and sc > 0 and Fraction(tl, sl) <= Fraction(tc, sc)
            self.assertIs(select(**inputs), expected, inputs)

    def test_all_decision_partitions(self):
        self.check_decisions(select_lighter)

    def test_integer_boundaries(self):
        self.check_integer_boundaries(select_lighter)

    def test_exact_ratios(self):
        self.check_ratios(select_lighter)

    def test_token_ratio_scale_invariance(self):
        for tl, sl, tc, sc in itertools.product((0, 1, 2, 35), repeat=4):
            inputs = {
                **BASE,
                "tokens_lighter": tl,
                "solved_lighter": sl,
                "tokens_cpython": tc,
                "solved_cpython": sc,
            }
            expected = select_lighter(**inputs)
            for scale in (1, 3, 2**65):
                for tokens, solved in (
                    ("tokens_lighter", "solved_lighter"),
                    ("tokens_cpython", "solved_cpython"),
                ):
                    scaled = {
                        **inputs,
                        tokens: inputs[tokens] * scale,
                        solved: inputs[solved] * scale,
                    }
                    self.assertIs(select_lighter(**scaled), expected)

    def test_recorded_decision(self):
        path = ROOT / "docs/verification/python-code-mode/data/heldout-analysis.json"
        recorded = json.loads(path.read_text())["selection"]
        self.assertIs(select_lighter(**recorded["inputs"]), recorded["select_lighter"])
        self.assertIs(recorded["select_lighter"], False)
        # Reproduce each recorded rejection reason with all other gates passing.
        self.assertIs(
            select_lighter(
                **{**BASE, "semantics_ok": recorded["inputs"]["semantics_ok"]}
            ),
            False,
        )
        self.assertIs(
            select_lighter(
                **{
                    **BASE,
                    "bytes_lighter": recorded["inputs"]["bytes_lighter"],
                    "bytes_cpython": recorded["inputs"]["bytes_cpython"],
                }
            ),
            False,
        )

    def test_policy_mutations_are_rejected(self):
        source = (HERE / "policy.py").read_text()
        mutants = (
            ("corpus_total > 0", "True"),
            ("corpus_passed == corpus_total", "corpus_passed >= corpus_total"),
            ("and trials_complete", "and True"),
            ("first_lighter >= first_cpython", "first_lighter <= first_cpython"),
            ("first_lighter >= first_cpython", "first_lighter > first_cpython"),
            ("final_lighter >= final_cpython", "final_lighter <= final_cpython"),
            ("final_lighter >= final_cpython", "final_lighter > final_cpython"),
            ("solved_lighter > 0", "True"),
            ("solved_cpython > 0", "True"),
            (
                "tokens_lighter * solved_cpython <= tokens_cpython * solved_lighter",
                "True",
            ),
            (
                "tokens_lighter * solved_cpython <= tokens_cpython * solved_lighter",
                "tokens_lighter // solved_lighter <= tokens_cpython // solved_cpython",
            ),
            (
                "tokens_lighter * solved_cpython",
                "float(tokens_lighter) * solved_cpython",
            ),
            ("and semantics_ok", "and True"),
            ("bytes_lighter < bytes_cpython", "True"),
            ("bytes_lighter < bytes_cpython", "bytes_lighter <= bytes_cpython"),
            (
                "bytes_lighter < bytes_cpython",
                "float(bytes_lighter) < float(bytes_cpython)",
            ),
            ("corpus_total > 0", "False"),
        )
        self.check_decisions(select_lighter)
        self.check_integer_boundaries(select_lighter)
        self.check_ratios(select_lighter)
        for before, after in mutants:
            with self.subTest(mutation=(before, after)):
                self.assertEqual(source.count(before), 1)
                namespace = {}
                exec(
                    compile(source.replace(before, after), "policy-mutant", "exec"),
                    namespace,
                )
                with self.assertRaises(AssertionError):
                    self.check_decisions(namespace["select_lighter"])
                    self.check_integer_boundaries(namespace["select_lighter"])
                    self.check_ratios(namespace["select_lighter"])


if __name__ == "__main__":
    unittest.main()
