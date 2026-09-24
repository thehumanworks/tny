import importlib
import unittest

from ledgerapi.store import Record, RecordStore

DOMAINS = (
    "billing",
    "shipping",
    "catalog",
    "inventory",
    "fraud",
    "returns",
    "notifications",
    "tax",
    "discounts",
    "fulfillment",
    "identity",
    "search",
    "recommendations",
    "payments",
    "invoicing",
    "subscriptions",
    "support",
    "analytics",
    "experiments",
    "localization",
    "compliance",
    "audit",
    "routing",
    "quotas",
    "rate_limits",
    "webhooks",
    "exports",
    "imports",
    "scheduling",
    "settlements",
)


class ClientMigrationTests(unittest.TestCase):
    def test_all_historical_clients(self):
        for domain in DOMAINS:
            with self.subTest(domain=domain):
                client = importlib.import_module(f"ledgerapi.clients.{domain}")
                key = client.setting_key("mode")
                store = RecordStore(
                    [Record("acme", key, "old", 1), Record("acme", key, "new", 4)]
                )
                reader = getattr(client, f"read_{domain}")
                self.assertEqual(reader(store, "acme", "mode", as_of=2), "old")
                self.assertEqual(reader(store, "acme", "mode"), "new")
