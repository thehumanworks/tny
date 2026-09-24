"""Hidden migration and behavior checks for every client."""

import importlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(sys.argv[1])))
from ledgerapi.store import Record, RecordStore

domains = [
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
]
assert not hasattr(RecordStore, "lookup")
for domain in domains:
    client = importlib.import_module(f"ledgerapi.clients.{domain}")
    reader = getattr(client, f"read_{domain}")
    key = client.setting_key("mode")
    store = RecordStore(
        [
            Record("acme", key, "old", 2),
            Record("acme", key, None, 4),
            Record("acme", key, "ignored", 7, deleted=True),
        ]
    )
    assert reader(store, "acme", "mode", as_of=3) == "old", domain
    assert reader(store, "acme", "mode", as_of=5, fallback="fallback") is None, domain
    assert reader(store, "acme", "mode", fallback="fallback") == "fallback", domain
    assert reader(store, "other", "mode", fallback="missing") == "missing", domain
    assert client.snapshot(store, "acme", "mode", 3) == "old", domain
    assert client.summarize(store, "acme", ["mode"], as_of=3)["mode"] == "old", domain
    assert client.describe(store, "acme", "mode", as_of=3)["value"] == "old", domain
    expiry = RecordStore(
        [Record("acme", key, 7, 1, expires_at=5), Record("other", "clock", "tick", 6)]
    )
    assert reader(expiry, "acme", "mode", as_of=4) == 7, domain
    assert reader(expiry, "acme", "mode", as_of=5, fallback=8) == 8, domain
assert RecordStore().resolve("none", "none").reason == "absent"
try:
    RecordStore().resolve("a", "b", as_of=-1)
except ValueError:
    pass
else:
    raise AssertionError("negative revision accepted")
assert "lookup" not in (Path(sys.argv[1]) / "docs" / "api.md").read_text()
print("migration behavior passed")
