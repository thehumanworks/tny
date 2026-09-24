from decimal import Decimal

from billing import invoice_total

lines = [
    {"sku": "a", "usd_price": Decimal("0.01"), "quantity": 1},
    {"sku": "b", "usd_price": Decimal("0.01"), "quantity": 1},
]
assert invoice_total(lines, Decimal("0.50")) == Decimal("0.01")
