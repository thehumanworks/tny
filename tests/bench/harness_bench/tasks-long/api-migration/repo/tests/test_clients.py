import unittest

from ledgerapi.clients import billing, catalog, fraud, inventory, shipping
from ledgerapi.store import Record, RecordStore


class ClientMigrationTests(unittest.TestCase):
    def test_historical_clients(self):
        for client in (billing, shipping, catalog, inventory, fraud):
            key = client.setting_key("mode")
            store = RecordStore(
                [Record("acme", key, "old", 1), Record("acme", key, "new", 4)]
            )
            reader = getattr(client, f"read_{client.DOMAIN}")
            with self.subTest(client=client.DOMAIN):
                self.assertEqual(reader(store, "acme", "mode", as_of=2), "old")
                self.assertEqual(reader(store, "acme", "mode"), "new")
