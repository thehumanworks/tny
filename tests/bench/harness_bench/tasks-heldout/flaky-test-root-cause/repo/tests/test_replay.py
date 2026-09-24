import unittest

from replay import replay


class ReplayTests(unittest.TestCase):
    def test_large_replay(self):
        events = [{"seq": i // 3, "message": f"  Message {i}  "} for i in range(240)]
        for i in range(5000):
            print(
                f"diagnostic event={i % 240} shard={i % 17} retry={i % 5} phase=replay"
            )
        expected = [{"seq": i // 3, "message": f"message {i}"} for i in range(240)]
        self.assertEqual(replay(events), expected)
