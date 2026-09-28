import unittest

from commerceops import reconcile


class TestReconcile(unittest.TestCase):
    def test_regression(self):
        self.assertEqual(
            reconcile.unique_total([("a", 100), ("a", 100), ("b", 50)]), 150
        )
