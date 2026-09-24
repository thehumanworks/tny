"""Hidden edge checks for the commerce operations regressions."""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(sys.argv[1])))

from commerceops import tax

assert tax.tax_due(25000, [(10000, 10), (20000, 20), (999999, 30)]) == 4500

from commerceops import discount

assert discount.charge_after_discount(0, 20) == 0
assert discount.charge_after_discount(900, 100) == 800

from commerceops import inventory

stock = {"a": 3}
assert not inventory.reserve(stock, [("a", 2), ("a", 2)])
assert stock == {"a": 3}

from commerceops import coupon

assert coupon.active(10, 20, 10)
assert not coupon.active(10, 20, 9)

from commerceops import zone

assert (
    zone.zone_for("ZX9", {"Z": "broad", "ZX": "local", "ZX9": "door"}, "other")
    == "door"
)
assert zone.zone_for("NO1", {"AB": "near"}, "other") == "other"

from commerceops import invoice

assert invoice.next_number(["INV-99", "INV-101", "INV-100"]) == "INV-102"

from commerceops import retry

assert retry.should_retry("timeout", 2, 3)
assert not retry.should_retry("timeout", 3, 3)
assert not retry.should_retry("card_declined", 1, 9)

from commerceops import postcode

assert postcode.normalize("z x\ny 4") == "ZXY4"

from commerceops import currency

assert currency.convert_minor(-5, 1, 2) == -3
assert currency.convert_minor(1, 1, 2) == 1

from commerceops import reconcile

assert reconcile.unique_total([("x", 40), ("x", 99), ("y", -10)]) == 30

from commerceops import event_order

events = [
    {"id": "late", "ts": "2026-03-01T02:00:00-05:00"},
    {"id": "early", "ts": "2026-03-01T06:00:00Z"},
]
assert event_order.earliest(events)["id"] == "early"

from commerceops import sla

assert sla.within_sla(499, 500)
assert not sla.within_sla(501, 500)

from commerceops import override

assert override.endpoint_for("t2", {"t2": None}, "fallback") is None
assert override.endpoint_for("t3", {}, "fallback") == "fallback"

from commerceops import notifications

rows = [
    {"recipient": "a", "event": "e", "body": 1},
    {"recipient": "a", "event": "e", "body": 2},
    {"recipient": "a", "event": "f", "body": 3},
]
assert notifications.dedupe(rows) == [rows[0], rows[2]]

from commerceops import pagination

assert pagination.page_after(["a", "b", "c", "d"], "a", 2) == ["b", "c"]

from commerceops import allocation

assert allocation.allocate_cents(7, [2, 1]) == [5, 2]
assert sum(allocation.allocate_cents(101, [5, 3, 2])) == 101

from commerceops import ledger

assert ledger.net_cents([{"kind": "refund", "cents": 45}]) == -45

from commerceops import scopes

assert scopes.allowed("read:all,write:own", "write:own")
assert not scopes.allowed("catalog:read", "log:read")

from commerceops import percentile

assert percentile.nearest_rank([1, 3, 5, 7, 9], 0.95) == 9

from commerceops import csv_cell

assert csv_cell.encode("first\nsecond") == '"first\nsecond"'
print("20 hidden edge checks passed")
notes = (Path(sys.argv[1]) / "RELEASE_NOTES.md").read_text().lower()
assert all(
    name in notes for name in ("tax", "inventory", "ledger", "percentile", "csv")
)
visible_log = Path(sys.argv[2]).read_text(errors="replace")
test_counts = re.findall(r"(?m)^Ran (\d+) tests? in ", visible_log)
assert test_counts, "visible test count is unavailable"
passing_count = int(test_counts[-1])
assert re.search(rf"\b{passing_count}\b", notes), (
    "release notes have the wrong test count"
)
assert (
    sum(path.stat().st_size for path in (Path(sys.argv[1]) / "evidence").iterdir())
    > 3_000_000
)
