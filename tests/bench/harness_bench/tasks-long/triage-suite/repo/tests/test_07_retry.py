import unittest

from commerceops import retry


class TestRetry(unittest.TestCase):
    def test_regression(self):
        self.assertFalse(retry.should_retry("card_declined", 1, 3))
        self.assertFalse(retry.should_retry("timeout", 3, 3))
