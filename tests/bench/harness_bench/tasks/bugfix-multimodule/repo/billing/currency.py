from decimal import Decimal

from .money import cents


def to_eur(usd, rate):
    return cents(Decimal(usd) * Decimal(rate))
