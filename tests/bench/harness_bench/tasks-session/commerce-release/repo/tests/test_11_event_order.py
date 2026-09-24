import unittest

from commerceops import event_order


class TestEventOrder(unittest.TestCase):
    def test_regression(self):
        events = [
            {"id": "late", "ts": "2026-03-01T07:00:00-05:00"},
            {"id": "early", "ts": "2026-03-01T08:00:00Z"},
        ]
        self.assertEqual(event_order.earliest(events)["id"], "early")
