def validate_quantity(q):
    if not isinstance(q, int) or q < 0:
        raise ValueError("quantity")
    return q
