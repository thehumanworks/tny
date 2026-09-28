import unittest

from commerceops import pagination


class TestPagination(unittest.TestCase):
    def test_regression(self):
        self.assertEqual(pagination.page_after(["a", "b", "c"], "b", 2), ["c"])
