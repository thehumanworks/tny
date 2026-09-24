import unittest

from commerceops import notifications


class TestNotifications(unittest.TestCase):
    def test_regression(self):
        rows = [{"recipient": "a", "event": "e"}, {"recipient": "b", "event": "e"}]
        self.assertEqual(notifications.dedupe(rows), rows)
