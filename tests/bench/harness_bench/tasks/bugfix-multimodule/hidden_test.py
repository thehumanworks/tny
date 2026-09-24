import subprocess
import sys
from decimal import ROUND_HALF_UP, Decimal
from pathlib import Path

w = Path(sys.argv[1])
sys.path.insert(0, str(w))
from billing import invoice_total
from billing.currency import to_eur

for items, rate in [
    ([("0.01", 1), ("0.01", 1)], "0.50"),
    ([("0.01", 5), ("0.02", 3)], "0.37"),
    ([("19.99", 2), ("0.99", 7)], "0.9234"),
    ([], "1.20"),
]:
    lines = [
        dict(sku=str(i), usd_price=Decimal(p), quantity=q)
        for i, (p, q) in enumerate(items)
    ]
    original = repr(lines)
    expect = (
        sum((Decimal(p) * q for p, q in items), Decimal(0)) * Decimal(rate)
    ).quantize(Decimal("0.01"), rounding=ROUND_HALF_UP)
    assert invoice_total(lines, Decimal(rate)) == expect
    assert repr(lines) == original
assert to_eur(Decimal("0.015"), Decimal("1")) == Decimal("0.02")
subprocess.run(
    [sys.executable, "test_invoice.py"],
    cwd=w,
    check=True,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
