import unittest

from commerceops import ledger


class TestLedger(unittest.TestCase):
    def test_regression(self):
        self.assertEqual(
            ledger.net_cents(
                [{"kind": "charge", "cents": 500}, {"kind": "refund", "cents": 100}]
            ),
            400,
        )
