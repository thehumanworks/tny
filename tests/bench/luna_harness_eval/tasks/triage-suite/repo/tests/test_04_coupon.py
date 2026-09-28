import unittest

from commerceops import coupon


class TestCoupon(unittest.TestCase):
    def test_regression(self):
        self.assertFalse(coupon.active(10, 20, 20))
