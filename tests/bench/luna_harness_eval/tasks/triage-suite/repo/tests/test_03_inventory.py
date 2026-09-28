import unittest

from commerceops import inventory


class TestInventory(unittest.TestCase):
    def test_regression(self):
        stock = {"a": 3, "b": 1}
        self.assertFalse(inventory.reserve(stock, [("a", 2), ("b", 2)]))
        self.assertEqual(stock, {"a": 3, "b": 1})
