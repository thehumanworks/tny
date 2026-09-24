import unittest

from commerceops import postcode


class TestPostcode(unittest.TestCase):
    def test_regression(self):
        self.assertEqual(postcode.normalize("  ab  12	3  "), "AB123")
