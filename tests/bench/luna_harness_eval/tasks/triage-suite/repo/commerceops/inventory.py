def reserve(stock, lines):
    """Reserve all requested (sku, quantity) lines or leave stock unchanged."""
    for sku, quantity in lines:
        if stock.get(sku, 0) < quantity:
            return False
        stock[sku] -= quantity
    return True
