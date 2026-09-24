def active(start, end, instant):
    """Coupon is valid from start until, but not including, end."""
    return start <= instant <= end
