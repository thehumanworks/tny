import unittest

from commerceops import override


class TestOverride(unittest.TestCase):
    def test_regression(self):
        self.assertEqual(override.endpoint_for("t1", {"t1": ""}, "https://default"), "")
