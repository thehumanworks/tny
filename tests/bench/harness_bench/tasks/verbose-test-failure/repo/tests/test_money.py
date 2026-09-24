import unittest

from parcelcalc.money import parse_cents


class MoneyTests(unittest.TestCase):
    pass


def test_for(value, expected):
    def test(self):
        self.assertEqual(parse_cents(value), expected)

    return test


for i in range(200):
    setattr(
        MoneyTests,
        f"test_decimal_{i:03d}",
        test_for(f"{10**16 + i}.29", (10**16 + i) * 100 + 29),
    )
