from decimal import ROUND_HALF_UP, Decimal

CENT = Decimal("0.01")


def cents(v):
    return Decimal(v).quantize(CENT, rounding=ROUND_HALF_UP)
