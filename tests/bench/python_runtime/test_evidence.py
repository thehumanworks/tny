"""Offline cohort-integrity regressions; no live provider or interpreter needed."""

from __future__ import annotations

import copy
import unittest

import audit_evidence as audit
from execute import strict_json


class EvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.samples = strict_json((audit.DATA / "heldout-samples.json").read_text())

    def test_full_cohort_and_raw_receipts(self):
        report = audit.verify_samples(self.samples)
        self.assertEqual(report["samples"], 108)
        self.assertEqual(report["variant_outcomes_checked"], 333)
        self.assertEqual(
            audit.verify_raw(
                self.samples, audit.DATA / "heldout-raw-generations.tar.gz"
            ),
            111,
        )
        self.assertEqual(report["totals"]["cpython"]["output_tokens"], 9428)
        self.assertEqual(report["totals"]["monty"]["final"], 36)

    def test_incomplete_or_duplicate_cohort_rejected(self):
        for samples in (self.samples[:-1], self.samples[:-1] + [self.samples[0]]):
            with self.assertRaises(ValueError):
                audit.verify_samples(samples)

    def test_untrusted_metadata_cannot_be_success_or_zero_usage(self):
        for mutation in ("usage", "model", "effort", "numeric", "variant", "extra"):
            samples = copy.deepcopy(self.samples)
            first = samples[0]
            attempt = first["attempts"][0]
            if mutation == "usage":
                del attempt["generation"]["usage"]["output_tokens"]
            elif mutation == "model":
                attempt["generation"]["requested_model"] = "different-model"
            elif mutation == "effort":
                attempt["generation"]["effort"] = "high"
            elif mutation == "numeric":
                first["passed"] = 1
            elif mutation == "variant":
                attempt["evaluation"]["variants"].pop()
            else:
                first["attempts"] = [attempt, attempt, attempt]
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                audit.verify_samples(samples)

    def test_json_shapes_and_order_remain_distinct(self):
        for left, right in ((False, 0), (None, False), ([], {}), ("1", 1)):
            self.assertNotEqual(audit.canonical(left), audit.canonical(right))
        left, right = {"a": 1, "b": 2}, {"b": 2, "a": 1}
        self.assertEqual(audit.canonical(left), audit.canonical(right))
        self.assertNotEqual(audit.canonical(left, True), audit.canonical(right, True))


if __name__ == "__main__":
    unittest.main()
