import unittest

from commerceops import tax


class TestTax(unittest.TestCase):
    def test_regression(self):
        self.assertEqual(tax.tax_due(15000, [(10000, 10), (20000, 20)]), 2000)
