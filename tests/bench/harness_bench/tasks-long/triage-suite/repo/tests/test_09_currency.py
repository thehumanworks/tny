import unittest

from commerceops import currency


class TestCurrency(unittest.TestCase):
    def test_regression(self):
        self.assertEqual(currency.convert_minor(105, 3, 2), 158)
