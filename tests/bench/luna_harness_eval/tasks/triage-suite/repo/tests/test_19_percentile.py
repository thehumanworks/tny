import unittest

from commerceops import percentile


class TestPercentile(unittest.TestCase):
    def test_regression(self):
        self.assertEqual(percentile.nearest_rank([1, 2, 3, 4], 0.9), 4)
