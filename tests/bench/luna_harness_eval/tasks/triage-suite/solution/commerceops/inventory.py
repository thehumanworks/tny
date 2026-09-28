from collections import Counter


def reserve(stock, lines):
    """Reserve all requested (sku, quantity) lines or leave stock unchanged."""
    required = Counter()
    for sku, quantity in lines:
        required[sku] += quantity
    if any(stock.get(sku, 0) < quantity for sku, quantity in required.items()):
        return False
    for sku, quantity in required.items():
        stock[sku] -= quantity
    return True
