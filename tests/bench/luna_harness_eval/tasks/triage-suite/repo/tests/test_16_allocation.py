import unittest

from commerceops import allocation


class TestAllocation(unittest.TestCase):
    def test_regression(self):
        self.assertEqual(allocation.allocate_cents(5, [1, 1, 1]), [2, 2, 1])
