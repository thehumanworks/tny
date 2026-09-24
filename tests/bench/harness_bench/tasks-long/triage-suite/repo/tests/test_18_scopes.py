import unittest

from commerceops import scopes


class TestScopes(unittest.TestCase):
    def test_regression(self):
        self.assertFalse(scopes.allowed("read:all,write:own", "read"))
