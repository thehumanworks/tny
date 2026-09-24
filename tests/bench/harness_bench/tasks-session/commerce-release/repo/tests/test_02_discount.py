import unittest

from commerceops import discount


class TestDiscount(unittest.TestCase):
    def test_regression(self):
        self.assertEqual(discount.charge_after_discount(500, 700), 0)
