import unittest

from commerceops import sla


class TestSla(unittest.TestCase):
    def test_regression(self):
        self.assertTrue(sla.within_sla(500, 500))
