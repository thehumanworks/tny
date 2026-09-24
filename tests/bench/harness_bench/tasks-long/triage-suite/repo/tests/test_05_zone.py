import unittest

from commerceops import zone


class TestZone(unittest.TestCase):
    def test_regression(self):
        self.assertEqual(
            zone.zone_for("AB12", {"A": "far", "AB": "near"}, "other"), "near"
        )
