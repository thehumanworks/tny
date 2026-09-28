import unittest

from ledgerapi.store import Record, RecordStore


class StoreMigrationTests(unittest.TestCase):
    def test_resolution_and_history(self):
        store = RecordStore(
            [
                Record("acme", "billing:mode", "old", 1),
                Record("acme", "billing:mode", "new", 3),
            ]
        )
        self.assertEqual(store.resolve("acme", "billing:mode", as_of=2).value, "old")
        self.assertEqual(store.resolve("acme", "billing:mode").value, "new")

    def test_explicit_null_and_tombstone(self):
        store = RecordStore(
            [
                Record("acme", "tax:mode", None, 1),
                Record("acme", "tax:mode", None, 3, deleted=True),
            ]
        )
        self.assertTrue(store.resolve("acme", "tax:mode", as_of=1).found)
        self.assertFalse(store.resolve("acme", "tax:mode").found)
