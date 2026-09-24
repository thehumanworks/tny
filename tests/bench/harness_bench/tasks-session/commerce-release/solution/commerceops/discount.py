def charge_after_discount(subtotal, discount):
    """Apply a fixed discount without creating a negative charge."""
    return max(0, subtotal - discount)
