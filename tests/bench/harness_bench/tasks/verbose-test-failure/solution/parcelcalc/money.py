import re
from decimal import Decimal

VALID = re.compile(r"[+-]?(?:[0-9]+(?:\.[0-9]{0,2})?|\.[0-9]{1,2})\Z")


def parse_cents(amount):
    if not isinstance(amount, str) or not VALID.fullmatch(amount):
        raise ValueError("amount")
    return int(Decimal(amount) * 100)
