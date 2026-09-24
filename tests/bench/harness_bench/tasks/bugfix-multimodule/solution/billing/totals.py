from decimal import Decimal

from .currency import to_eur


def cart_total_eur(lines, rate):
    usd = sum((x["usd_price"] * x["quantity"] for x in lines), Decimal(0))
    return to_eur(usd, rate)
